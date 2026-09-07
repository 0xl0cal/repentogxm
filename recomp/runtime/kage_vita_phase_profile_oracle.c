/* Deterministic host oracle for the aggregate physical-Vita phase profiler. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#if defined(ISAAC_VITA_ROOM_LOG_MARKERS) && ISAAC_VITA_ROOM_LOG_MARKERS
#include <errno.h>
static int s_room_thread = 7;
static unsigned s_room_thread_reads;
int sceKernelGetThreadId(void)
{
    ++s_room_thread_reads;
    errno = EDOM;
    return s_room_thread;
}
#define ORACLE_ROOM_LOG_RECORDS 1u
#else
#define ORACLE_ROOM_LOG_RECORDS 0u
#endif

#include "kage_vita_phase_profile.h"
#include "guest.h"
#include "gl_vita_backend.h"
#if defined(ISAAC_VITA_HEAP_CENSUS)
#include "host_vita_heap.h"
#endif
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE) || \
    defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
#include "host_vita_import_id.h"
/* guest.c owns the one per-import census array in production. */
uint32_t g_guest_phase_profile_import_calls[GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS];
#endif

GuestPhaseProfileCounters g_guest_phase_profile_counters;
static uint32_t s_fusion_boundaries;
void gl_vita_backend_phase_profile_window_boundary(void)
{
    ++s_fusion_boundaries;
}
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
/* This gauge is independent of the aggregate counters: a pending request may
 * straddle an emission. The observer must not flush or reset native state. */
static uint32_t s_attrib_pending = 3u;
uint32_t gl_vita_backend_attrib_pending(void)
{
    return s_attrib_pending;
}
#endif

#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
static uint32_t s_texture_window_takes;
static uint32_t s_texture_window_order_bad;
void gl_vita_backend_texture_churn_profile_take_window(
    IsaacVitaTextureChurnProfile *profile)
{
    if (s_fusion_boundaries != s_texture_window_takes + 1u)
        ++s_texture_window_order_bad;
    ++s_texture_window_takes;
    if (profile) {
        memset(profile, 0, sizeof *profile);
        profile->bad = s_texture_window_order_bad;
    }
}
# define ORACLE_TEXTURE_CHURN_RECORDS 2U
#else
# define ORACLE_TEXTURE_CHURN_RECORDS 0U
#endif

#if defined(ISAAC_VITA_GL_FILL_CENSUS)
/* Stub of the GL backend's take-and-zero: field k of take t reads
 * 100*t + k so both windows pin distinct values; a take outside the
 * boundary order (one per report, after the window's own boundary) taints
 * `bad` by 1000 and the exact-record checks below fail. */
static uint32_t s_fill_takes;
static uint32_t s_fill_order_bad;
static uint32_t s_fill_last_window;
static uint32_t s_fill_last_render_p50;
void gl_vita_backend_fill_census_take_window(
    IsaacVitaGlFillCensus *census, uint32_t window, uint32_t render_p50_us)
{
    uint32_t base = 100u * s_fill_takes;
    uint32_t index;

    if (s_fusion_boundaries != s_fill_takes + 1u)
        ++s_fill_order_bad;
    ++s_fill_takes;
    s_fill_last_window = window;
    s_fill_last_render_p50 = render_p50_us;
    if (!census)
        return;
    census->draws = base + 1u;
    census->triangles = base + 2u;
    census->kpx_unclipped = base + 3u;
    census->kpx_clipped = base + 4u;
    census->kpx_clear = base + 5u;
    census->max_draw_kpx = base + 6u;
    census->big = base + 7u;
    census->bad = base + 8u + 1000u * s_fill_order_bad;
    census->synthesized = base + 9u;
    census->projective = base + 10u;
    census->tiles = base + 11u;
    census->viewport_width = base + 12u;
    census->viewport_height = base + 13u;
    census->viewport_changes = base + 14u;
    census->miss = base + 15u;
    census->prog_kpx[0] = base + 16u;
    census->prog_kpx[1] = base + 17u;
    census->blend_kpx[0] = base + 18u;
    census->blend_kpx[1] = base + 19u;
    census->blend_kpx[2] = base + 20u;
    for (index = 0u; index < 5u; ++index) {
        census->pass_draws[index] = base + 21u + index;
        census->pass_kpx[index] = base + 26u + index;
        census->pass_clears[index] = base + 31u + index;
    }
    census->attachment_width = base + 36u;
    census->attachment_height = base + 37u;
    census->clear_unknown = base + 38u;
}
# define ORACLE_FILL_RECORDS 2U
#else
# define ORACLE_FILL_RECORDS 0U
#endif

#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
uint32_t g_guest_lookup_cache_hits;
uint32_t g_guest_lookup_cache_misses;
# define ORACLE_GUEST_CACHE_RECORDS 1U
#else
# define ORACLE_GUEST_CACHE_RECORDS 0U
#endif
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
/* Stands in for host_vita_import_id.c: the two sync IDs carry the SYNC
 * binding kind (16), everything else is reported as CRT (2). */
int guest_host_import_id_kind(uint32_t import_id)
{
    if (import_id >= GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS)
        return -1;
    return import_id == 60u || import_id == 61u ? 16 : 2;
}
# define ORACLE_DISPATCH_RECORDS 2U
# define ORACLE_MAX_DISPATCH_LENGTH 132u
# define ORACLE_MAX_IMPORTS_LENGTH 269u
# if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
/* Both census readers on one array.  Window 1: the table hunk adds 360
 * entries and 3 imports per loop; the import-kinds hunk adds one import per
 * loop and, so the identity g(c)-g(l) == imports + dyn is exact, two more
 * slow entries (all 360 entries slow) and the three host-import increments
 * the table hunk's notes stand for.  Window 2 adds the one increment for the
 * table hunk's ID-60 note (240 entries, 120 slow). */
#  define ORACLE_WINDOW1_GUEST "g(c,l,i)=720,240,3240 "
#  define ORACLE_WINDOW2_GUEST "g(c,l,i)=720,480,4800 "
#  define ORACLE_WINDOW1_CACHE "gc(h,m)=120,120\n"
#  define ORACLE_WINDOW2_CACHE "gc(h,m)=360,120\n"
#  define ORACLE_WINDOW1_DISPATCH "d(c,s,sf)=360,360,120\n"
#  define ORACLE_WINDOW2_DISPATCH "d(c,s,sf)=240,120,0\n"
#  define ORACLE_WINDOW1_IMPORTS \
    "imp(n,sf)=480,120 " \
    "top=5:135,60:120,61:120,0:15,1:15,2:15,3:15,4:15 " \
    "kind=2:240,16:240,0:0,1:0\n"
#  define ORACLE_WINDOW2_IMPORTS \
    "imp(n,sf)=240,0 " \
    "top=60:120,0:15,1:15,2:15,3:15,4:15,5:15,6:15 " \
    "kind=2:120,16:120,0:0,1:0\n"
# else
/* Window 1 adds 360 table entries (240 hits + 120 slow) to the historical
 * 360 calls / 240 lookups; window 2 adds 240 entries (120 + 120). */
#  define ORACLE_WINDOW1_GUEST "g(c,l,i)=720,480,3240 "
#  define ORACLE_WINDOW2_GUEST "g(c,l,i)=720,480,4800 "
#  define ORACLE_WINDOW1_CACHE "gc(h,m)=360,120\n"
#  define ORACLE_WINDOW2_CACHE "gc(h,m)=360,120\n"
#  define ORACLE_WINDOW1_DISPATCH "d(c,s,sf)=360,120,120\n"
#  define ORACLE_WINDOW2_DISPATCH "d(c,s,sf)=240,120,0\n"
/* Ties rank toward the lowest import ID; zero fillers keep the shape. */
#  define ORACLE_WINDOW1_IMPORTS \
    "imp(n,sf)=360,120 " \
    "top=5:120,60:120,61:120,0:0,1:0,2:0,3:0,4:0 " \
    "kind=16:240,2:120,0:0,1:0\n"
#  define ORACLE_WINDOW2_IMPORTS \
    "imp(n,sf)=120,0 " \
    "top=60:120,0:0,1:0,2:0,3:0,4:0,5:0,6:0 " \
    "kind=16:120,0:0,1:0,2:0\n"
# endif
# if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
/* wf/opt-gltok: one typed-GL token hits the table per loop (a table entry
 * that is a GL hit, not a lookup, and one dynamic call): g(c) grows by 120,
 * the derived g(l) excludes the hit and keeps its value, ph120.d gains the
 * hit count as its fourth field (s excludes it, c - s - g = translated
 * hits), and the ph120.ik identity g(c)-g(l) == imports + dyn holds with
 * the 120 dynamic calls; ph120.i and the cache census are unchanged. */
#  undef ORACLE_MAX_DISPATCH_LENGTH
#  define ORACLE_MAX_DISPATCH_LENGTH 145u
#  undef ORACLE_WINDOW1_GUEST
#  undef ORACLE_WINDOW2_GUEST
#  undef ORACLE_WINDOW1_DISPATCH
#  undef ORACLE_WINDOW2_DISPATCH
#  if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
#   define ORACLE_WINDOW1_GUEST "g(c,l,i)=840,240,3240 "
#   define ORACLE_WINDOW1_DISPATCH "d(c,s,sf,g)=480,360,120,120\n"
#  else
#   define ORACLE_WINDOW1_GUEST "g(c,l,i)=840,480,3240 "
#   define ORACLE_WINDOW1_DISPATCH "d(c,s,sf,g)=480,120,120,120\n"
#  endif
#  define ORACLE_WINDOW2_GUEST "g(c,l,i)=840,480,4800 "
#  define ORACLE_WINDOW2_DISPATCH "d(c,s,sf,g)=360,120,0,120\n"
#  define ORACLE_WINDOW1_IK_DYN "dyn=120"
#  define ORACLE_WINDOW2_IK_DYN "dyn=127"
# else
#  define ORACLE_WINDOW1_IK_DYN "dyn=0"
#  define ORACLE_WINDOW2_IK_DYN "dyn=7"
# endif
#else
# define ORACLE_DISPATCH_RECORDS 0U
# define ORACLE_WINDOW1_GUEST "g(c,l,i)=360,240,3240 "
# define ORACLE_WINDOW2_GUEST "g(c,l,i)=480,360,4800 "
# define ORACLE_WINDOW1_CACHE "gc(h,m)=120,120\n"
# define ORACLE_WINDOW2_CACHE "gc(h,m)=240,120\n"
#endif
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
# define ORACLE_GL_REDUNDANCY_RECORDS 1U
#else
# define ORACLE_GL_REDUNDANCY_RECORDS 0U
#endif
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
# define ORACLE_GL_TYPED_STATE_RECORDS 1U
#else
# define ORACLE_GL_TYPED_STATE_RECORDS 0U
#endif
#if defined(ISAAC_VITA_GL_TIME_PROFILE)
IsaacVitaGlTimeProfile g_isaac_vita_gl_time_profile;
# if defined(ISAAC_VITA_GL_WRAPPER_TIME)
/* gl_bridge.c owns this in production; the oracle fills it like the gt
 * buckets (bucket i of base b: calls b+i, us 10(b+i); slot j: b+20+j; draw
 * split part i: b+40+i; canonical hits b+50; replay part (j,k): b+60+3j+k;
 * uniform kind i: b+70+i). */
IsaacVitaGlWrapperTimeProfile g_isaac_vita_gl_wrapper_time;
static void set_gl_wrapper_time_bucket(
    IsaacVitaGlTimeBucket *bucket, uint32_t value)
{
    bucket->calls = value;
    bucket->total_us = 10u * value;
    bucket->max_us = 100u * value;
}
static void set_gl_wrapper_time_window(uint32_t base)
{
    uint32_t i;

    for (i = 0u; i < ISAAC_VITA_GL_WRAPPER_BUCKETS; ++i)
        set_gl_wrapper_time_bucket(
            &g_isaac_vita_gl_wrapper_time.wrapper[i], base + i);
    for (i = 0u; i < ISAAC_VITA_GL_WRAPPER_K_SLOTS; ++i)
        set_gl_wrapper_time_bucket(
            &g_isaac_vita_gl_wrapper_time.slot[i], base + 20u + i);
    for (i = 0u; i < ISAAC_VITA_GL_DRAW_SPLIT_BUCKETS; ++i)
        set_gl_wrapper_time_bucket(
            &g_isaac_vita_gl_wrapper_time.draw[i], base + 40u + i);
    g_isaac_vita_gl_wrapper_time.draw_canonical = base + 50u;
    for (i = 0u; i < ISAAC_VITA_GL_WRAPPER_K_SLOTS * ISAAC_VITA_GL_REPLAY_PARTS;
            ++i)
        set_gl_wrapper_time_bucket(
            &g_isaac_vita_gl_wrapper_time.replay[i / ISAAC_VITA_GL_REPLAY_PARTS]
                                                [i % ISAAC_VITA_GL_REPLAY_PARTS],
            base + 60u + i);
    for (i = 0u; i < ISAAC_VITA_GL_UNIFORM_KINDS; ++i)
        set_gl_wrapper_time_bucket(
            &g_isaac_vita_gl_wrapper_time.uniform[i], base + 70u + i);
    g_isaac_vita_gl_wrapper_time.bad_clock = 0u;
}
/* ph120.gw, ph120.gd, ph120.gu directly after the ph120.gt row(s), before
 * ph120.gx. */
#  define ORACLE_GL_WRAPPER_RECORDS 3U
# else
#  define ORACLE_GL_WRAPPER_RECORDS 0U
# endif
# if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE) && ISAAC_VITA_GL_TIME_SDK_SPARSE
# include "../vita/vitagl-stock-reference/isaac_sdk_sparse_profile.h"
static uint32_t s_sparse_takes, s_sparse_discards, s_sparse_controls;
static uint32_t s_sparse_next_window, s_sparse_order_bad;
/* Transport fixture, not a performance model: distinct synthetic fields
 * identify every bucket and window. Native helper behavior is tested at its
 * owner; here NULL must discard without calibration, and each snapshot must
 * request the following window without shifting the current residue. */
void vglIsaacSdkSparseTake(IsaacSdkSparseProfile *out, uint32_t next_window)
{
    ++s_sparse_takes;
    if (out) {
        uint32_t i, base = 10u * s_sparse_next_window;
        if (!s_sparse_next_window || next_window != s_sparse_next_window + 1u)
            ++s_sparse_order_bad;
        ++s_sparse_controls;
        memset(out, 0, sizeof *out);
        out->residue = s_sparse_next_window & 31u;
        out->canonical_seen = base + 1u;
        out->canonical_selected = base + 2u;
        out->canonical_issued = base + 3u;
        out->canonical_unpaired = base + 4u;
        out->canonical_draw_errors = base + 11u;
        out->other_draw_calls = base + 5u;
        out->begin_calls = base + 6u;
        out->end_calls = base + 7u;
        out->clock_reads = base + 8u;
        out->bad_clock = base + 9u;
        out->saturation = base + 10u;
        for (i = 0u; i < ISAAC_SDK_BUCKET_COUNT; ++i) {
            out->bucket[i].calls = base + i + 1u;
            out->bucket[i].us = 10u * (base + i + 1u);
            out->bucket[i].max_us = 100u * (base + i + 1u);
        }
    } else {
        ++s_sparse_discards;
    }
    s_sparse_next_window = next_window;
}
#  if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE_ATTRIB)
/* Stand-in for the replay TU's take (host_vita_shader_attrib_fastpath.c is
 * not linked here): same transport fixture as the sdk32 stub above, distinct
 * synthetic fields per window, NULL discards, order pinned. */
static uint32_t s_attrib_takes, s_attrib_discards, s_attrib_controls;
static uint32_t s_attrib_next_window, s_attrib_order_bad;
void isaac_vita_attrib_replay_sparse_take(
    IsaacVitaAttribReplaySparse *out, uint32_t next_window)
{
    ++s_attrib_takes;
    if (out) {
        uint32_t base = 10u * s_attrib_next_window;
        if (!s_attrib_next_window || next_window != s_attrib_next_window + 1u)
            ++s_attrib_order_bad;
        ++s_attrib_controls;
        memset(out, 0, sizeof *out);
        out->residue = s_attrib_next_window & 31u;
        out->enable_replays = base + 31u;
        out->disable_replays = base + 32u;
        out->bad_clock = base + 33u;
        out->enable.calls = base + 34u;
        out->enable.us = 10u * (base + 34u);
        out->enable.max_us = 100u * (base + 34u);
        out->disable.calls = base + 35u;
        out->disable.us = 10u * (base + 35u);
        out->disable.max_us = 100u * (base + 35u);
    } else {
        ++s_attrib_discards;
    }
    s_attrib_next_window = next_window;
}
#   define ORACLE_GL_SCOPE_RECORDS 5U
#  else
#   define ORACLE_GL_SCOPE_RECORDS 4U
#  endif
# else
#  define ORACLE_GL_SCOPE_RECORDS 1U
# endif
static uint32_t s_scene_takes;
/* Stands in for the 0005 gxm.c hook: take-and-zero semantics, so each take
 * returns values that identify the take. */
void vglIsaacSceneTimes(
    uint32_t *begin_us, uint32_t *end_us, uint32_t *count,
    uint32_t *begin_max_us)
{
    ++s_scene_takes;
    *count = 5u * s_scene_takes;
    *begin_us = 700u * s_scene_takes;
    *end_us = 90u * s_scene_takes;
    *begin_max_us = 300u * s_scene_takes;
}
/* Stands in for the 0006 gxm.c hook, taken together with the one above (the
 * profiler must call both at every take, including the discarded first). */
static uint32_t s_scene_wait_takes;
void vglIsaacSceneWaits(
    uint32_t *fbo_us, uint32_t *display_us, uint32_t *max_us,
    uint32_t *slow, uint32_t *ordinal_us, uint32_t *rt_created,
    uint32_t *rt_failed, uint32_t *depth_created)
{
    ++s_scene_wait_takes;
    *fbo_us = 80u * s_scene_wait_takes;
    *display_us = 10u * s_scene_wait_takes;
    *max_us = 45u * s_scene_wait_takes;
    *slow = 3u * s_scene_wait_takes;
    ordinal_us[0] = 20u * s_scene_wait_takes;
    ordinal_us[1] = 30u * s_scene_wait_takes;
    ordinal_us[2] = 25u * s_scene_wait_takes;
    ordinal_us[3] = 5u * s_scene_wait_takes;
    *rt_created = s_scene_wait_takes;
    *rt_failed = 0u;
    *depth_created = 2u * s_scene_wait_takes;
}
static void set_gl_time_window(uint32_t base)
{
    IsaacVitaGlTimeBucket *buckets[] = {
        &g_isaac_vita_gl_time_profile.draw,
        &g_isaac_vita_gl_time_profile.clear,
        &g_isaac_vita_gl_time_profile.bind_fb,
        &g_isaac_vita_gl_time_profile.tex_upload,
        &g_isaac_vita_gl_time_profile.state,
        &g_isaac_vita_gl_time_profile.present,
    };
    uint32_t i;

    for (i = 0u; i < 6u; ++i) {
        buckets[i]->calls = base + i;
        buckets[i]->total_us = 10u * (base + i);
        buckets[i]->max_us = 100u * (base + i);
    }
    g_isaac_vita_gl_time_profile.bad_clock = 0u;
}
# if defined(ISAAC_VITA_STOCK_FBO_VALID_REGION)
/* Stands in for the 0009 gxm.c hook, taken with the two above at every
 * take: counter i = (i + 1) * takes, census tuple i = the pow2 texture and
 * logical rect halved i times with (i + 1) * 100 * takes scenes. */
static uint32_t s_valid_region_takes;
void vglIsaacFboValidRegionStats(uint32_t *counters, uint32_t *census)
{
    uint32_t i;

    ++s_valid_region_takes;
    for (i = 0u; i < 8u; ++i)
        counters[i] = (i + 1u) * s_valid_region_takes;
    for (i = 0u; i < 4u; ++i) {
        census[i * 5u + 0u] = 1024u >> i;
        census[i * 5u + 1u] = 1024u >> i;
        census[i * 5u + 2u] = 960u >> i;
        census[i * 5u + 3u] = 540u >> i;
        census[i * 5u + 4u] = (i + 1u) * 100u * s_valid_region_takes;
    }
}
/* ph120.gt, ph120.gx with the vr(...) suffix, then the ph120.vz census. */
#  define ORACLE_GL_TIME_RECORDS \
    (ORACLE_GL_SCOPE_RECORDS + ORACLE_GL_WRAPPER_RECORDS + 2U)
#  define ORACLE_GX_VR_WIN1 " vr(s,p,u,a,e,v,c)=2,4,6,8,10,12,14"
#  define ORACLE_GX_VR_WIN2 " vr(s,p,u,a,e,v,c)=3,6,9,12,15,18,21"
# else
/* ph120.gt and, directly after it, ph120.gx (the 0006 EndScene split). */
#  define ORACLE_GL_TIME_RECORDS \
    (ORACLE_GL_SCOPE_RECORDS + ORACLE_GL_WRAPPER_RECORDS + 1U)
#  define ORACLE_GX_VR_WIN1 ""
#  define ORACLE_GX_VR_WIN2 ""
# endif
#else
# define ORACLE_GL_TIME_RECORDS 0U
#endif
#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
# define ORACLE_VITAGL_RECORDS 3U
typedef struct vglIsaacGpuDrawStats {
    uint32_t abi_version;
    uint32_t enabled;
    uint32_t draw_elements_requests;
    uint32_t coalesce_attempts;
    uint32_t coalesce_hits;
    uint32_t coalesce_reject_count;
    uint32_t coalesce_reject_stream_index;
    uint32_t coalesce_reject_base;
    uint32_t coalesce_reject_descriptor;
    uint32_t stream_slots_before;
    uint32_t stream_slots_after;
    uint32_t vertex_program_requests;
    uint32_t vertex_program_hits;
    uint32_t vertex_program_misses;
    uint32_t vertex_program_stores;
    uint32_t vertex_program_full_fallbacks;
    uint32_t vertex_program_store_failures;
    uint32_t vertex_program_create_failures;
    uint32_t vertex_program_releases;
    uint32_t index_bytes;
    uint32_t vertex_bytes;
    uint32_t index_count;
    uint32_t vertex_count;
    uint32_t top_idx_high_water;
    uint32_t top_idx_unknown;
    uint32_t vertex_set_hits;
    uint32_t vertex_set_misses;
    uint32_t fragment_set_hits;
    uint32_t fragment_set_misses;
    uint32_t texture_set_hits;
    uint32_t texture_set_misses;
} vglIsaacGpuDrawStats;
static vglIsaacGpuDrawStats s_vitagl_stats = {
    .abi_version = 2u,
    .enabled = 1u,
};
#else
# define ORACLE_VITAGL_RECORDS 0U
#endif
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION) || \
    defined(ISAAC_VITA_FBO_RASTER_SCALE)
/* ph120.e (offscreen render-target policies) follows the vitaGL records;
 * the production max-record flush does not emit it (its worst case, five or
 * eight ten-digit counters, is far below the 384-byte line allowance). */
# define ORACLE_FBO_POLICY_RECORDS 1U
#else
# define ORACLE_FBO_POLICY_RECORDS 0U
#endif
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
# define ORACLE_COLOROFFSET_RECORDS 1U
typedef struct vglIsaacColorOffsetStats {
    uint32_t abi_version;
    uint32_t enabled;
    uint32_t source_selected;
    uint32_t source_mark_rejected;
    uint32_t compile_successes;
    uint32_t compile_failures;
    uint32_t exact_draw_requests;
    uint32_t opaque_eligible;
    uint32_t opaque_hits;
    uint32_t opaque_viewport_pixels;
    uint32_t fail_program;
    uint32_t fail_blend;
    uint32_t fail_sampler;
    uint32_t fail_layout;
    uint32_t fail_vertex_range;
    uint32_t fail_vertex_value;
    uint32_t fail_output;
    uint32_t fail_cache;
    uint32_t cache_hits;
    uint32_t cache_creates;
    uint32_t cache_releases;
    uint32_t static_eligible;
    uint32_t static_hits;
    uint32_t static_pixels;
} vglIsaacColorOffsetStats;
static vglIsaacColorOffsetStats s_coloroffset_stats = {
    .abi_version = 1u,
    .enabled = 1u,
};
#else
# define ORACLE_COLOROFFSET_RECORDS 0U
#endif
#if defined(ISAAC_VITA_COLOROFFSET_FS_PROBE)
/* Stands in for the 0008 custom_shaders.c probe endpoint: fixed link state
 * (one exact program linked, no failures) and per-loop draw/cache counts. */
# define ORACLE_FS_PROBE_RECORDS 1U
# include "gl_vita_coloroffset_fs_probe.h"
static uint32_t s_plain_window[3];
#if defined(ISAAC_VITA_COLOROFFSET_PLAIN_FP16_ORACLE)
static uint32_t s_half_window[2];
static uint32_t s_half_takes;
#endif
#if defined(ISAAC_VITA_COLOROFFSET_PLAIN_FP16_ORACLE) || !defined(__GNUC__)
uint32_t vglTakeIsaacColorOffsetPlainFp16Stats(uint32_t *out, uint32_t words)
{
#if defined(ISAAC_VITA_COLOROFFSET_PLAIN_FP16_ORACLE)
    if (!out || words != 2u)
        return 0u;
    memcpy(out, s_half_window, sizeof s_half_window);
    memset(s_half_window, 0, sizeof s_half_window);
    ++s_half_takes;
    return 1u;
#else
    (void)out;
    (void)words;
    return 0u;
#endif
}
#endif
#if defined(ISAAC_VITA_COLOROFFSET_PLAIN_VERTEX_PAIR_ORACLE)
static uint32_t s_pair_window[3];
#endif
#if defined(ISAAC_VITA_COLOROFFSET_PLAIN_VERTEX_PAIR_ORACLE) || !defined(__GNUC__)
uint32_t vglTakeIsaacColorOffsetPlainVertexPairStats(uint32_t *out, uint32_t words)
{
#if defined(ISAAC_VITA_COLOROFFSET_PLAIN_VERTEX_PAIR_ORACLE)
    if (!out || words != 3u)
        return 0u;
    memcpy(out, s_pair_window, sizeof s_pair_window);
    memset(s_pair_window, 0, sizeof s_pair_window);
    return 1u;
#else
    (void)out;
    (void)words;
    return 0u;
#endif
}
#endif
uint32_t vglTakeIsaacColorOffsetPlainStats(uint32_t *out, uint32_t words)
{
    if (!out || words != 3u)
        return 0u;
    memcpy(out, s_plain_window, sizeof s_plain_window);
    memset(s_plain_window, 0, sizeof s_plain_window);
    return 1u;
}
static vglIsaacColorOffsetFsProbeStats s_fs_probe_stats = {
    .abi_version = 1u,
    .mode = ISAAC_VITA_COLOROFFSET_FS_PROBE,
    .link_attempts = 1u,
    .link_ready = 1u,
    .last_program = 7u,
    .gxp_size = 811u,
    .gxp_fnv1a = 0x0badf00du,
    .source_size = 546u,
    .source_fnv1a = 0x12345678u,
};
# define ORACLE_FS_PROBE_STR2(x) #x
# define ORACLE_FS_PROBE_STR(x) ORACLE_FS_PROBE_STR2(x)
#else
# define ORACLE_FS_PROBE_RECORDS 0U
#endif
#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
# define ORACLE_SCHEDULER_RECORDS 3U
#else
# define ORACLE_SCHEDULER_RECORDS 0U
#endif
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
# define ORACLE_IMPORT_KINDS_RECORDS 2U
#else
# define ORACLE_IMPORT_KINDS_RECORDS 0U
#endif
#if defined(ISAAC_VITA_PILL_BLOOM_BYPASS)
# define ORACLE_PILL_BLOOM_RECORDS 1U
#else
# define ORACLE_PILL_BLOOM_RECORDS 0U
#endif
#if defined(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES)
# define ORACLE_BLOOM_HALF_RECORDS 1U
#else
# define ORACLE_BLOOM_HALF_RECORDS 0U
#endif
#if defined(ISAAC_VITA_POOP_FX_PROFILE)
# define ORACLE_POOP_FX_RECORDS 1U
#else
# define ORACLE_POOP_FX_RECORDS 0U
#endif
#if defined(ISAAC_VITA_LASER_PROFILE)
# define ORACLE_LASER_RECORDS 3U
#else
# define ORACLE_LASER_RECORDS 0U
#endif
/* ph120.a (location queries + timeGetTime) is unconditional under
 * ISAAC_VITA_PHASE_PROFILE and follows ph120.c. */
#define ORACLE_LOCATION_RECORDS 1U
/* ISAAC_VITA_KAGE_QUAD_FASTPATH: ph120.kq follows ph120.mem.  Stand-in for
 * the replay TU's take (host_vita_kage_quad_fastpath.c is not linked here):
 * distinct synthetic fields per take, NULL discards, counts pinned. */
#if defined(ISAAC_VITA_KAGE_QUAD_FASTPATH)
# include "host_vita_kage_quad_fastpath.h"
# define ORACLE_KAGE_QUAD_RECORDS 1U
/* All-ones fields: 23 ten-digit numbers behind the 32-byte build id. */
# define ORACLE_MAX_KAGE_QUAD_LENGTH 378u
static uint32_t s_kage_quad_takes, s_kage_quad_discards;
void isaac_vita_kage_quad_fastpath_take(IsaacVitaKageQuadCounters *out)
{
    ++s_kage_quad_takes;
    if (out) {
        uint32_t base = 100u * s_kage_quad_takes, i;
        memset(out, 0, sizeof *out);
        out->mode = 2u;
        out->calls = base + 1u;
        out->handled = base + 2u;
        out->culled = base + 3u;
        out->constant_cull = base + 4u;
        out->getters = base + 5u;
        out->snapped = base + 6u;
        out->unbatched = base + 7u;
        out->translucent = base + 8u;
        out->fresh_records = base + 9u;
        out->ring_growths = base + 10u;
        out->dirty_growths = base + 11u;
        for (i = 0u; i < ISAAC_VITA_KAGE_QUAD_DECLINE_REASONS; ++i)
            out->declined[i] = base + 20u + i;
    } else {
        ++s_kage_quad_discards;
    }
}
#else
# define ORACLE_KAGE_QUAD_RECORDS 0U
#endif
/* ISAAC_VITA_HEAP_CENSUS: ph120.mem is the last record of every window. */
#if defined(ISAAC_VITA_HEAP_CENSUS)
# define ORACLE_HEAP_CENSUS_RECORDS 1U
/* All-ones fields, why=badalloc: the longest ph120.mem (inside the 512-byte
 * allowance ph120.s already uses; ISAAC_VITA_LOG_ASYNC_LINE_MAX is 1024). */
# define ORACLE_MAX_HEAP_LENGTH 459u
# if defined(ISAAC_VITA_HEAP_CENSUS_LUA)
#  define ORACLE_HEAP_LUA "lua(l,pk,fb)=11264,17920,0 "
# else
#  define ORACLE_HEAP_LUA "lua(l,pk,fb)=0,0,0 "
# endif
#else
# define ORACLE_HEAP_CENSUS_RECORDS 0U
#endif
#if defined(ISAAC_VITA_LASER_HALO_PROFILE) && ISAAC_VITA_LASER_HALO_PROFILE
# include "../vita/vitagl-stock-reference/isaac_laser_halo_profile.h"
# define ORACLE_HALO_RECORDS 1u
static vglIsaacLaserHaloStats s_halo = { .abi_version = ISAAC_LASER_HALO_STATS_ABI };
static uint32_t s_halo_reads, s_halo_unavailable;
uint32_t vglGetIsaacLaserHaloStats(vglIsaacLaserHaloStats *out, uint32_t words)
{
    if (!out || words != ISAAC_LASER_HALO_STATS_WORDS) return 0u;
    ++s_halo_reads;
    if (s_halo_unavailable) return 0u;
    *out = s_halo;
    return 1u;
}
#else
# define ORACLE_HALO_RECORDS 0u
#endif
#define ORACLE_RECORDS_PER_WINDOW \
    (2U + ORACLE_LOCATION_RECORDS + ORACLE_GL_TIME_RECORDS + \
     ORACLE_TEXTURE_CHURN_RECORDS + ORACLE_GUEST_CACHE_RECORDS + \
     ORACLE_DISPATCH_RECORDS + \
     ORACLE_GL_REDUNDANCY_RECORDS + ORACLE_GL_TYPED_STATE_RECORDS + \
     ORACLE_VITAGL_RECORDS + ORACLE_FILL_RECORDS + \
     ORACLE_FBO_POLICY_RECORDS + \
      ORACLE_COLOROFFSET_RECORDS + ORACLE_FS_PROBE_RECORDS + \
      ORACLE_PILL_BLOOM_RECORDS + \
      ORACLE_BLOOM_HALF_RECORDS + ORACLE_POOP_FX_RECORDS + \
      ORACLE_LASER_RECORDS + \
      ORACLE_SCHEDULER_RECORDS + ORACLE_IMPORT_KINDS_RECORDS + \
      ORACLE_HEAP_CENSUS_RECORDS + ORACLE_KAGE_QUAD_RECORDS + \
      ORACLE_HALO_RECORDS + ORACLE_ROOM_LOG_RECORDS)
IsaacVitaGlPhaseProfileCounters g_isaac_vita_gl_phase_profile_counters;
/* host_vita_post_com.c owns this in production; the oracle links without it. */
uint32_t g_isaac_vita_post_com_time_get_time_calls;

static uint64_t s_now;
static uint32_t s_clock_reads;
static char s_logs[8192];
static size_t s_log_length;
static unsigned s_log_calls;

static void add_gl_counts(uint32_t base)
{
#if defined(ISAAC_VITA_LASER_HALO_PROFILE) && ISAAC_VITA_LASER_HALO_PROFILE
    for (uint32_t i = 0u; i < ISAAC_HALO_OUTCOMES; ++i) {
        s_halo.outcome[i] += base + i;
        s_halo.attempts += base + i;
    }
    s_halo.too_big += base;
    s_halo.ordinary_plain_requests += base + 1u;
    s_halo.ordinary_white_requests += base;
#endif
    g_isaac_vita_gl_phase_profile_counters.draw_elements += base;
    g_isaac_vita_gl_phase_profile_counters.clear += base + 1u;
    g_isaac_vita_gl_phase_profile_counters.bind_texture += base + 2u;
    g_isaac_vita_gl_phase_profile_counters.use_program += base + 3u;
    g_isaac_vita_gl_phase_profile_counters.tex_image += base + 4u;
    g_isaac_vita_gl_phase_profile_counters.tex_sub_image += base + 5u;
    g_isaac_vita_gl_phase_profile_counters.uniform += base + 6u;
    g_isaac_vita_gl_phase_profile_counters.attrib_pointer += base + 7u;
    g_isaac_vita_gl_phase_profile_counters.attrib_toggle += base + 8u;
    g_isaac_vita_gl_phase_profile_counters.bind_framebuffer += base + 9u;
    g_isaac_vita_gl_phase_profile_counters.state += base + 10u;
    g_isaac_vita_gl_phase_profile_counters.canonical_quad_hits += base + 13u;
    g_isaac_vita_gl_phase_profile_counters.canonical_quad_reject_callsite += base + 14u;
    g_isaac_vita_gl_phase_profile_counters.canonical_quad_reject_shape += base + 15u;
    g_isaac_vita_gl_phase_profile_counters.canonical_quad_reject_bounds += base + 16u;
    g_isaac_vita_gl_phase_profile_counters.canonical_quad_reject_pointer += base + 17u;
    g_isaac_vita_gl_phase_profile_counters.canonical_quad_driver_fallback += base + 18u;
    g_isaac_vita_gl_phase_profile_counters.canonical_quad_index_bytes_saved += base + 19u;
    g_isaac_vita_gl_phase_profile_counters.fusion_index_bytes += base + 20u;
    g_isaac_vita_gl_phase_profile_counters.fusion_blend_additive += base + 21u;
    g_isaac_vita_gl_phase_profile_counters.fusion_blend_alpha += base + 22u;
    g_isaac_vita_gl_phase_profile_counters.fusion_blend_other += base + 23u;
    g_isaac_vita_gl_phase_profile_counters.fusion_shader_same += base + 24u;
    g_isaac_vita_gl_phase_profile_counters.fusion_shader_change += base + 25u;
    g_isaac_vita_gl_phase_profile_counters.fusion_fbo_default += base + 26u;
    g_isaac_vita_gl_phase_profile_counters.fusion_fbo_offscreen += base + 27u;
    g_isaac_vita_gl_phase_profile_counters.fusion_atlas_same += base + 28u;
    g_isaac_vita_gl_phase_profile_counters.fusion_atlas_change += base + 29u;
    g_isaac_vita_gl_phase_profile_counters.fusion_additive_draws += base + 30u;
    g_isaac_vita_gl_phase_profile_counters.fusion_additive_runs += base + 31u;
    g_isaac_vita_gl_phase_profile_counters.attrib_location += base + 42u;
    g_isaac_vita_gl_phase_profile_counters.uniform_location += base + 43u;
    g_isaac_vita_gl_phase_profile_counters.location_cache_hit += base + 44u;
    g_isaac_vita_post_com_time_get_time_calls += base + 45u;
    g_isaac_vita_gl_phase_profile_counters.location_cache_mismatch +=
        base + 46u;
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION) || \
    defined(ISAAC_VITA_FBO_RASTER_SCALE)
    /* ph120.e clear(x,r,p[,a,d]) raster(t,v,s): the printed fields, in
     * print order, on the same base + k scheme. */
    g_isaac_vita_gl_phase_profile_counters.clear_suppressed += base + 47u;
    g_isaac_vita_gl_phase_profile_counters.clear_replayed += base + 48u;
    g_isaac_vita_gl_phase_profile_counters.fbo_elision_poison += base + 49u;
# if defined(ISAAC_VITA_FBO_CLEAR_ELISION_DEPTH_DROP)
    g_isaac_vita_gl_phase_profile_counters.clear_dropped_attach +=
        base + 50u;
    g_isaac_vita_gl_phase_profile_counters.clear_dropped_scene +=
        base + 51u;
# endif
    g_isaac_vita_gl_phase_profile_counters.fbo_raster_textures += base + 52u;
    g_isaac_vita_gl_phase_profile_counters.fbo_raster_viewports +=
        base + 53u;
    g_isaac_vita_gl_phase_profile_counters.fbo_raster_respecified +=
        base + 54u;
#endif
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    g_isaac_vita_gl_phase_profile_counters.use_program_suppressed +=
        base + 11u;
    g_isaac_vita_gl_phase_profile_counters.uniform_suppressed +=
        base + 12u;
#endif
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    g_isaac_vita_gl_phase_profile_counters
        .typed_state_hit_active_texture += base + 32u;
    g_isaac_vita_gl_phase_profile_counters
        .typed_state_hit_bind_texture += base + 33u;
    g_isaac_vita_gl_phase_profile_counters
        .typed_state_hit_blend += base + 34u;
    g_isaac_vita_gl_phase_profile_counters
        .typed_state_hit_depth += base + 35u;
    g_isaac_vita_gl_phase_profile_counters
        .typed_state_hit_viewport += base + 36u;
    g_isaac_vita_gl_phase_profile_counters
        .typed_state_hit_attrib_toggle += base + 37u;
    g_isaac_vita_gl_phase_profile_counters
        .typed_state_hit_attrib_pointer += base + 38u;
    g_isaac_vita_gl_phase_profile_counters.typed_state_miss += base + 39u;
    g_isaac_vita_gl_phase_profile_counters
        .typed_state_reject_invalid += base + 40u;
    g_isaac_vita_gl_phase_profile_counters
        .typed_state_reject_unknown += base + 41u;
#endif
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
    g_isaac_vita_gl_phase_profile_counters.attrib_deferred += base * 4u;
    g_isaac_vita_gl_phase_profile_counters.attrib_cancelled += base * 2u;
    s_attrib_pending = base == 1u ? 5u : 1u;
#endif
}

#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
static void add_coloroffset_counts(uint32_t base)
{
#define ORACLE_COLOROFFSET_ADD(field) s_coloroffset_stats.field += base
    ORACLE_COLOROFFSET_ADD(source_selected);
    ORACLE_COLOROFFSET_ADD(source_mark_rejected);
    ORACLE_COLOROFFSET_ADD(compile_successes);
    ORACLE_COLOROFFSET_ADD(compile_failures);
    ORACLE_COLOROFFSET_ADD(exact_draw_requests);
    ORACLE_COLOROFFSET_ADD(opaque_eligible);
    ORACLE_COLOROFFSET_ADD(opaque_hits);
    ORACLE_COLOROFFSET_ADD(opaque_viewport_pixels);
    ORACLE_COLOROFFSET_ADD(fail_program);
    ORACLE_COLOROFFSET_ADD(fail_blend);
    ORACLE_COLOROFFSET_ADD(fail_sampler);
    ORACLE_COLOROFFSET_ADD(fail_layout);
    ORACLE_COLOROFFSET_ADD(fail_vertex_range);
    ORACLE_COLOROFFSET_ADD(fail_vertex_value);
    ORACLE_COLOROFFSET_ADD(fail_output);
    ORACLE_COLOROFFSET_ADD(fail_cache);
    ORACLE_COLOROFFSET_ADD(cache_hits);
    ORACLE_COLOROFFSET_ADD(cache_creates);
    ORACLE_COLOROFFSET_ADD(cache_releases);
    ORACLE_COLOROFFSET_ADD(static_eligible);
    ORACLE_COLOROFFSET_ADD(static_hits);
    ORACLE_COLOROFFSET_ADD(static_pixels);
#undef ORACLE_COLOROFFSET_ADD
}

void vglGetIsaacColorOffsetStats(vglIsaacColorOffsetStats *stats)
{
    if (stats)
        *stats = s_coloroffset_stats;
}
#endif

#if defined(ISAAC_VITA_COLOROFFSET_FS_PROBE)
static void add_fs_probe_counts(uint32_t base)
{
    s_plain_window[0] += base * 2u;
    s_plain_window[1] += base;
    s_plain_window[2] += base;
#if defined(ISAAC_VITA_COLOROFFSET_PLAIN_FP16_ORACLE)
    s_half_window[0] += base * 2u;
    s_half_window[1] += base;
#endif
#if defined(ISAAC_VITA_COLOROFFSET_PLAIN_VERTEX_PAIR_ORACLE)
    s_pair_window[0] += base * 2u;
    s_pair_window[1] += base;
    s_pair_window[2] += base;
#endif
    s_fs_probe_stats.draw_requests += base;
    s_fs_probe_stats.draws_bound += base;
    s_fs_probe_stats.draws_opaque_overridden += base;
    s_fs_probe_stats.fail_cache += base;
    s_fs_probe_stats.fail_create += base;
    s_fs_probe_stats.cache_creates += base;
    s_fs_probe_stats.cache_releases += base;
}

void vglGetIsaacColorOffsetFsProbeStats(vglIsaacColorOffsetFsProbeStats *stats)
{
    if (stats)
        *stats = s_fs_probe_stats;
}
#endif

#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
static void add_vitagl_counts(uint32_t base)
{
    s_vitagl_stats.draw_elements_requests += base;
    s_vitagl_stats.coalesce_attempts += base;
    s_vitagl_stats.coalesce_hits += base;
    s_vitagl_stats.stream_slots_before += base * 8u;
    s_vitagl_stats.stream_slots_after += base;
    s_vitagl_stats.vertex_program_requests += base;
    s_vitagl_stats.vertex_program_hits += base;
    s_vitagl_stats.index_bytes += base * 12u;
    s_vitagl_stats.vertex_bytes += base * 16u;
    s_vitagl_stats.index_count += base * 6u;
    s_vitagl_stats.vertex_count += base * 4u;
    s_vitagl_stats.top_idx_high_water = base * 4u;
    s_vitagl_stats.top_idx_unknown += base;
    s_vitagl_stats.vertex_set_hits += base;
    s_vitagl_stats.vertex_set_misses += base + 1u;
    s_vitagl_stats.fragment_set_hits += base + 2u;
    s_vitagl_stats.fragment_set_misses += base + 3u;
    s_vitagl_stats.texture_set_hits += base + 4u;
    s_vitagl_stats.texture_set_misses += base + 5u;
}

void vglGetIsaacGpuDrawStats(vglIsaacGpuDrawStats *stats)
{
    if (stats)
        *stats = s_vitagl_stats;
}
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "phase profile oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static const char *log_line(unsigned index)
{
    const char *line = s_logs;

    while (index-- != 0u) {
        line = strchr(line, '\n');
        if (!line)
            return NULL;
        ++line;
    }
    return line;
}

static size_t log_line_length(const char *line)
{
    const char *newline = line ? strchr(line, '\n') : NULL;
    return newline ? (size_t)(newline - line + 1) : 0u;
}

uint64_t sceKernelGetProcessTimeWide(void)
{
    ++s_clock_reads;
    return s_now;
}

#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
uint64_t isaac_vita_get_process_time(void)
{
    return s_now;
}

int sceKernelDelayThread(unsigned int delay_us)
{
    s_now += delay_us;
    return 0;
}

void isaac_vita_log(const char *format, ...)
{
    (void)format;
}
#endif

#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
/* Import-kinds sources the reporter reads besides the census array: the
 * kind/hot-pin snapshot owned by host_vita_import_id.c and the two aggregate
 * host counters owned by host_vita_first_fault.c.  The oracle owns them so
 * both identity outcomes (residuals 0 -> ok=1; a stray dynamic/import count
 * -> ok=0) are driven without the 413-row table: IDs 0..7 map to one binding
 * kind each (everything else is unresolved -> "oth") and the fourteen hot
 * slots read IDs 0..13 directly. */
unsigned g_host_import_calls;
unsigned g_host_dynamic_calls;

void isaac_vita_import_id_calls_snapshot(const uint32_t *calls, uint32_t count,
                                         IsaacVitaImportKindSnapshot *snapshot)
{
    static const uint8_t kind_of_id[8] = {
        ISAAC_VITA_IMPORT_SYNC, ISAAC_VITA_IMPORT_HEAP,
        ISAAC_VITA_IMPORT_MEMORY, ISAAC_VITA_IMPORT_CRT,
        ISAAC_VITA_IMPORT_POST_COM, ISAAC_VITA_IMPORT_MATH,
        ISAAC_VITA_IMPORT_LUA, ISAAC_VITA_IMPORT_UNRESOLVED
    };
    uint32_t i;

    memset(snapshot, 0, sizeof *snapshot);
    if (count > ISAAC_VITA_IMPORT_ID_COUNT)
        count = ISAAC_VITA_IMPORT_ID_COUNT;
    for (i = 0u; i < count; ++i) {
        snapshot->total += calls[i];
        snapshot->kind[i < 8u ? kind_of_id[i]
                              : ISAAC_VITA_IMPORT_UNRESOLVED] += calls[i];
    }
    for (i = 0u; i < ISAAC_VITA_IMPORT_HOT_COUNT && i < count; ++i)
        snapshot->hot[i] = calls[i];
    snapshot->hot_ok = 1u;
}
#endif

int sceClibPrintf(const char *format, ...)
{
    va_list arguments;
    size_t available = sizeof s_logs - s_log_length;
    int result;

    va_start(arguments, format);
    result = vsnprintf(s_logs + s_log_length, available, format, arguments);
    va_end(arguments);
#if defined(ISAAC_VITA_ROOM_LOG_MARKERS) && ISAAC_VITA_ROOM_LOG_MARKERS
    if (strstr(format, "ph120.rl") || strstr(format, "roomlog")) errno = ERANGE;
#endif
    if (result > 0 && available != 0u) {
        size_t written = (size_t)result;
        if (written >= available)
            written = available - 1u;
        s_log_length += written;
    }
    ++s_log_calls;
    /* Model a deliberately expensive synchronous logger.  The production
     * profiler must take the next whole-loop origin after this cost. */
    s_now += 777u;
    return result;
}

#if defined(ISAAC_VITA_HEAP_CENSUS)
/* ph120.mem sources.  host_vita_heap.c owns the one-lock getter in
 * production; newlib mallinfo and vitaGL's pool query are reached through
 * the two oracle stand-ins declared in kage_vita_phase_profile.h.  The
 * fixture is the crashed prof-v11 session's shape (81 MiB contract, 66.8
 * MiB sbrk'd, 3294 free chunks, 98 spills, 26 slab pages), with the arena
 * deliberately below the contract so hr/la prove the lazy-growth math. */
static isaac_vita_guest_heap_census_locked s_heap_census;
static kage_vita_heap_mallinfo s_heap_mallinfo;
static unsigned s_heap_census_calls;
static unsigned s_heap_mallinfo_calls;

int isaac_vita_guest_heap_census_window_locked_get(
    isaac_vita_guest_heap_census_locked *out)
{
    ++s_heap_census_calls;
    *out = s_heap_census;
    return out->valid != 0u;
}

void kage_vita_phase_profile_oracle_mallinfo(kage_vita_heap_mallinfo *out)
{
    ++s_heap_mallinfo_calls;
    *out = s_heap_mallinfo;
}

void kage_vita_phase_profile_oracle_vgl_ram(
    uint32_t *free_bytes, uint32_t *total_bytes)
{
    *free_bytes = 96u << 20;
    *total_bytes = 128u << 20;
}

# if defined(ISAAC_VITA_HEAP_CENSUS_LUA)
void isaac_vita_lua_arena_stats(uint32_t *live_kb, uint32_t *peak_kb,
                                uint32_t *fallbacks)
{
    *live_kb = 11264u;
    *peak_kb = 17920u;
    *fallbacks = 0u;
}
# endif

static void set_heap_census_fixture(void)
{
    memset(&s_heap_census, 0, sizeof s_heap_census);
    s_heap_census.heap_total_bytes = 0x05100000u;
    s_heap_census.ledger_live_count = 108583u;
    s_heap_census.ledger_live_requested_bytes = 60000000u;
    s_heap_census.overflow_live_count = 98u;
    s_heap_census.overflow_live_requested_bytes = 6213366u;
    s_heap_census.overflow_native_failures = 1751u;
    s_heap_census.overflow_pool_failures = 1u;
    s_heap_census.overflow_capacity_bytes = 8212480u;
    s_heap_census.overflow_internal_requested_bytes = 1703936u;
    s_heap_census.slab_pages = 26u;
    s_heap_census.slab_live_slots = 103525u;
    s_heap_census.slab_page_bytes = 65536u;
    s_heap_census.ogg_slot_live = 1u;
    s_heap_census.terminal = 0u;
    s_heap_census.valid = 1u;
    s_heap_mallinfo.arena = 70000000u;
    s_heap_mallinfo.ordblks = 3294u;
    s_heap_mallinfo.uordblks = 69000000u;
    s_heap_mallinfo.fordblks = 1000000u;
    s_heap_mallinfo.keepcost = 300000u;
    s_heap_census_calls = 0u;
    s_heap_mallinfo_calls = 0u;
}

/* hr = 84934656 - 69000000; la = 300000 + (84934656 - 70000000);
 * rem = 8212480 - 6213366 - 1703936; slab bytes = 26 * 65536. */
# define ORACLE_HEAP_FIELDS \
    "mi(a,u,f,n,top)=70000000,69000000,1000000,3294,300000 " \
    "hr=15934656 la=15234656 led(n,b)=108583,60000000 " \
    "ov(l,b,nf,pf,rem)=98,6213366,1751,1,295178 " \
    "slab(p,l,b)=26,103525,1703936 " ORACLE_HEAP_LUA \
    "vgl(f,t)=100663296,134217728 slot=1 ok=1 why=win\n"
#endif

static void run_variable_window(uint32_t first_outer_loop)
{
    uint32_t sample;
    uint64_t loop_start = s_now;

    for (sample = 1u; sample <= 120u; ++sample) {
        uint64_t service_start = loop_start + 10u;
        uint64_t update_start = service_start + sample;
        uint64_t render_start = update_start + 100u + sample;
        uint64_t present_start = render_start + 20u;
        uint64_t present_end = present_start + 500u + sample;
        uint64_t render_end = render_start + 1000u + sample;
        uint64_t limiter_start = render_end + 10u;

        GUEST_PHASE_PROFILE_NOTE_CALL();
        GUEST_PHASE_PROFILE_NOTE_CALL();
        GUEST_PHASE_PROFILE_NOTE_CALL();
        GUEST_PHASE_PROFILE_NOTE_LOOKUP();
        GUEST_PHASE_PROFILE_NOTE_LOOKUP();
        GUEST_PHASE_PROFILE_ADD_LOOKUP_ITERATIONS(27u);
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
        /* One host import per loop over eight IDs (one binding kind each):
         * g(c)-g(l) = 120 = imports + 0 dynamic -> id(g,s)=0,0 ok=1. */
        GUEST_PHASE_PROFILE_NOTE_IMPORT(sample % 8u);
        ++g_host_import_calls;
#endif
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
        ++g_guest_lookup_cache_hits;
        ++g_guest_lookup_cache_misses;
#endif
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
        GUEST_PHASE_PROFILE_NOTE_DISPATCH_CALL();
        GUEST_PHASE_PROFILE_NOTE_DISPATCH_CALL();
        GUEST_PHASE_PROFILE_NOTE_DISPATCH_CALL();
        GUEST_PHASE_PROFILE_NOTE_DISPATCH_SLOW();
        GUEST_PHASE_PROFILE_NOTE_SYNC_FASTPATH();
        GUEST_PHASE_PROFILE_NOTE_IMPORT(60u);
        GUEST_PHASE_PROFILE_NOTE_IMPORT(61u);
        GUEST_PHASE_PROFILE_NOTE_IMPORT(5u);
        /* Out-of-range IDs are dropped, never written past the array. */
        GUEST_PHASE_PROFILE_NOTE_IMPORT(GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS);
# if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
        /* One typed-GL token through the table per loop: an entry that is a
         * GL hit (neither a lookup nor a slow entry) and, for the import-kinds
         * identity, the dynamic call its trampoline counts. */
        GUEST_PHASE_PROFILE_NOTE_DISPATCH_CALL();
        GUEST_PHASE_PROFILE_NOTE_DISPATCH_GL();
#  if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
        ++g_host_dynamic_calls;
#  endif
# endif
# if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
        /* Both readers: the three notes above are three host imports, and
         * three table entries must have left the probe for them (the two
         * counted as hits above become slow), so the derived g(c)-g(l)
         * equals imports + dyn and ph120.ik keeps id(g,s)=0,0 ok=1. */
        g_host_import_calls += 3u;
        GUEST_PHASE_PROFILE_NOTE_DISPATCH_SLOW();
        GUEST_PHASE_PROFILE_NOTE_DISPATCH_SLOW();
# endif
#endif
#if defined(ISAAC_VITA_LASER_PROFILE)
        s_now = loop_start + 1u;
        kage_vita_phase_profile_laser_begin(
            0x10000000u + sample, 1u, 0u, 0u, 0u, 0u);
        s_now = loop_start + 2u;
        kage_vita_phase_profile_laser_begin(
            0x20000000u + sample, 39u, 1u, 1u, 1u, 0u);
        s_now = loop_start + 3u;
        kage_vita_phase_profile_laser_begin(
            0x30000000u + sample, 39u, 1u, 1u, 1u, 1u);
        /* An inactive nested invocation shares the common epilogue but never
         * entered the active seam.  Its absent token must be a clean no-op. */
        kage_vita_phase_profile_laser_end(
            0xf0000000u + sample, 0u);
#endif
        add_gl_counts(1u);
#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
        add_vitagl_counts(1u);
#endif
#if defined(ISAAC_VITA_LASER_PROFILE)
        s_now = loop_start + 13u;
        kage_vita_phase_profile_laser_end(
            0x30000000u + sample, 1u);
        s_now = loop_start + 23u;
        kage_vita_phase_profile_laser_end(
            0x20000000u + sample, 0u);
        s_now = loop_start + 33u;
        kage_vita_phase_profile_laser_end(
            0x10000000u + sample, 0u);
#endif
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
        add_coloroffset_counts(1u);
#endif
#if defined(ISAAC_VITA_COLOROFFSET_FS_PROBE)
        add_fs_probe_counts(1u);
#endif
#if defined(ISAAC_VITA_PILL_BLOOM_BYPASS)
        if (sample <= 30u) {
            kage_vita_phase_profile_note_pill_bloom_capture_skip();
            kage_vita_phase_profile_note_pill_bloom_composite_skip();
        }
#endif
#if defined(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES)
        if (sample <= 30u) {
            kage_vita_phase_profile_note_bloom_half_capture();
            kage_vita_phase_profile_note_bloom_half_composite();
        }
#endif
#if defined(ISAAC_VITA_POOP_FX_PROFILE)
        if (sample == 1u)
            kage_vita_phase_profile_note_poop_fx_add(0x00388202u);
        else if (sample == 2u)
            kage_vita_phase_profile_note_poop_fx_add(0x002e7e96u);
        else if (sample == 3u)
            kage_vita_phase_profile_note_poop_fx_add(0x00036b42u);
        else if (sample == 4u)
            kage_vita_phase_profile_note_poop_fx_add(0xdeadc0deu);
        if (sample <= 30u) {
            kage_vita_phase_profile_note_poop_fx_render(
                181u - sample, 3u);
# if defined(ISAAC_VITA_POOP_FX_SINGLE_CLOUD)
            kage_vita_phase_profile_note_poop_fx_cap(2u);
# endif
        }
#endif
        s_now = service_start;
        kage_vita_phase_profile_note_service_entry();
        s_now = update_start;
        kage_vita_phase_profile_note_update_entry();
        s_now = render_start;
        kage_vita_phase_profile_note_render_entry();
        s_now = present_start;
        kage_vita_phase_profile_note_present_enter();
        s_now = present_end;
        kage_vita_phase_profile_note_present_return();
        s_now = render_end;
        kage_vita_phase_profile_note_render_return();
        s_now = limiter_start;
        kage_vita_phase_profile_note_limiter_entry();
        s_now = limiter_start + 2000u + sample;
        kage_vita_phase_profile_note_limiter_exit();

        s_now = loop_start + 6000u + sample;
        kage_vita_phase_profile_note_loop_head(first_outer_loop + sample);
        loop_start = s_now;
    }
}

static void run_limiter_skip_window(uint32_t first_outer_loop)
{
    uint32_t sample;
    uint32_t limiter_sample = 0u;
    uint64_t loop_start = s_now;

    for (sample = 1u; sample <= 120u; ++sample) {
        GUEST_PHASE_PROFILE_NOTE_CALL();
        GUEST_PHASE_PROFILE_NOTE_CALL();
        GUEST_PHASE_PROFILE_NOTE_CALL();
        GUEST_PHASE_PROFILE_NOTE_CALL();
        GUEST_PHASE_PROFILE_NOTE_LOOKUP();
        GUEST_PHASE_PROFILE_NOTE_LOOKUP();
        GUEST_PHASE_PROFILE_NOTE_LOOKUP();
        GUEST_PHASE_PROFILE_ADD_LOOKUP_ITERATIONS(40u);
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
        GUEST_PHASE_PROFILE_NOTE_IMPORT(sample % 8u);
        ++g_host_import_calls;
        if (sample == 1u) {
            /* Break both identities once: seven dynamic dispatches without
             * a guest_call and one host import without its per-ID note
             * must surface as id(g,s)=-8,1 ok=0. */
            g_host_dynamic_calls += 7u;
            ++g_host_import_calls;
        }
#endif
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
        g_guest_lookup_cache_hits += 2u;
        ++g_guest_lookup_cache_misses;
#endif
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
        GUEST_PHASE_PROFILE_NOTE_DISPATCH_CALL();
        GUEST_PHASE_PROFILE_NOTE_DISPATCH_CALL();
        GUEST_PHASE_PROFILE_NOTE_DISPATCH_SLOW();
        GUEST_PHASE_PROFILE_NOTE_IMPORT(60u);
# if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
        GUEST_PHASE_PROFILE_NOTE_DISPATCH_CALL();
        GUEST_PHASE_PROFILE_NOTE_DISPATCH_GL();
#  if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
        ++g_host_dynamic_calls;
#  endif
# endif
# if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
        /* The ID-60 note is one host import; one entry left the probe for
         * it (the slow one above), so the injected -8,1 residuals survive. */
        ++g_host_import_calls;
# endif
#endif
        add_gl_counts(2u);
#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
        add_vitagl_counts(2u);
#endif
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
        add_coloroffset_counts(2u);
#endif
#if defined(ISAAC_VITA_COLOROFFSET_FS_PROBE)
        add_fs_probe_counts(2u);
#endif
        s_now = loop_start + 10u;
        kage_vita_phase_profile_note_service_entry();
        s_now += 10u;
        kage_vita_phase_profile_note_update_entry();
        s_now += 20u;
        kage_vita_phase_profile_note_render_entry();
        s_now += 5u;
        kage_vita_phase_profile_note_present_enter();
        s_now += 7u;
        kage_vita_phase_profile_note_present_return();
        s_now += 23u;
        kage_vita_phase_profile_note_render_return();
        if (sample & 1u) {
            ++limiter_sample;
            s_now += 5u;
            kage_vita_phase_profile_note_limiter_entry();
            s_now += limiter_sample;
            kage_vita_phase_profile_note_limiter_exit();
        }
        s_now = loop_start + 5000u;
        kage_vita_phase_profile_note_loop_head(first_outer_loop + sample);
        loop_start = s_now;
    }
}

/* Exercise the existing real profiling API, not a duplicate gap algorithm.
 * No new test file/framework: these cases run in the established matrix. */
static int verify_present_gap_edges(void)
{
    uint32_t scenario;

    for (scenario = 0u; scenario < 9u; ++scenario) {
        uint32_t i;
        uint32_t n = scenario == 1u ? 59u :
            scenario == 2u || scenario == 4u || scenario == 5u ? 117u :
            scenario == 3u || scenario == 7u || scenario == 8u ? 118u : 119u;
        uint32_t gap = scenario == 1u ? 20000u :
            scenario == 6u ? UINT32_MAX : 10000u;
        uint64_t stride = scenario == 6u ? (uint64_t)UINT32_MAX + 100u : 10000u;
        char expected[128];

        kage_vita_phase_profile_reset();
        memset(s_logs, 0, sizeof s_logs);
        s_log_length = s_log_calls = 0u;
        s_now = 1000000u;
        kage_vita_phase_profile_note_loop_head(0u);
        for (i = 1u; i <= 120u; ++i) {
            uint64_t base = 1000000u + stride * (i - 1u);
            if (scenario == 3u && i >= 60u)
                base -= 50000u; /* Clock rollback between valid pairs. */
            if (scenario != 1u || (i & 1u) == 0u) {
                s_now = base + (scenario == 5u && i == 60u ? 50u : 10u);
                if (scenario == 8u && i == 60u)
                    s_now = base - 10000u + 19u; /* Entry clock rolled back. */
                if (scenario != 2u || i != 60u)
                    kage_vita_phase_profile_note_present_enter();
                if (scenario == 7u && i == 60u) {
                    s_now = base + 11u;
                    kage_vita_phase_profile_note_present_enter();
                }
                s_now = base + 20u;
                if (scenario != 4u || i != 60u) {
                    uint32_t reads_before = s_clock_reads;
                    kage_vita_phase_profile_note_present_return();
                    CHECK(s_clock_reads == reads_before + 1u);
                } /* Otherwise leave a dangling pair for the loop head. */
            }
            s_now = base + stride;
            kage_vita_phase_profile_note_loop_head(i);
        }
        snprintf(expected, sizeof expected,
                 "gap(n,min,50,95,max)=%u/%u/%u/%u/%u ",
                 n, gap, gap, gap, gap);
        CHECK(strstr(log_line(0u), expected) != NULL);
        CHECK(s_log_calls == ORACLE_RECORDS_PER_WINDOW);
    }
    return 0;
}

#if defined(ISAAC_VITA_ROOM_LOG_MARKERS) && ISAAC_VITA_ROOM_LOG_MARKERS
/* Separate process invocations exercise actual static initialization: no
 * reset or oracle seed is allowed before these first live loop heads. */
static int verify_room_cold_start(int first_uid)
{
    unsigned i, clocks, threads;
    CHECK(s_clock_reads == 0u && s_room_thread_reads == 0u && s_log_calls == 0u);
    s_room_thread = first_uid;
    errno = EACCES;
    kage_vita_phase_profile_room_log(90u, 90u);
    CHECK(errno == EACCES && s_clock_reads == 0u &&
          s_room_thread_reads == 0u && s_log_calls == 0u);
    s_now = 1000u;
    kage_vita_phase_profile_note_loop_head(1u);
    CHECK(errno == EACCES && s_clock_reads == 1u &&
          s_room_thread_reads == 1u && s_log_calls == 0u);
    /* If the first UID failed, a later valid UID must not silently rebind. */
    s_room_thread = 7;
    clocks = s_clock_reads;
    kage_vita_phase_profile_room_log(4u, 5u);
    CHECK(errno == EACCES && s_log_calls == 0u);
    CHECK(s_clock_reads == clocks + (first_uid > 0 ? 1u : 0u));
    CHECK(s_room_thread_reads == (first_uid > 0 ? 2u : 1u));
    threads = s_room_thread_reads;
    clocks = s_clock_reads;
    s_now += 100u;
    kage_vita_phase_profile_note_loop_head(2u);
    CHECK(errno == EACCES && s_clock_reads == clocks + 1u &&
          s_room_thread_reads == threads && s_log_calls == 0u);
    for (i = 3u; i <= 121u; ++i) {
        s_now += 100u;
        kage_vita_phase_profile_note_loop_head(i);
    }
    CHECK(errno == EACCES && s_room_thread_reads == threads);
    CHECK(strstr(s_logs, "pair=90,90") == NULL);
    if (first_uid > 0) {
        CHECK(s_log_calls == ORACLE_RECORDS_PER_WINDOW + 1u);
        CHECK(strstr(s_logs, "epoch=1 n=1 seq=1 drop=0 reset_lost=0 sat=0\n") != NULL);
        CHECK(strstr(s_logs, "epoch=1 seq=1 at=00000000:000003e8 assigned=1 win=1 loop=1 pair=4,5 sat=0\n") != NULL);
    } else {
        CHECK(s_log_calls == ORACLE_RECORDS_PER_WINDOW);
        CHECK(strstr(s_logs, "epoch=1 n=0 seq=0 drop=0 reset_lost=0 sat=0\n") != NULL);
        CHECK(strstr(s_logs, "[kage-vita] roomlog ") == NULL);
        clocks = s_clock_reads;
        kage_vita_phase_profile_room_log(6u, 7u);
        CHECK(errno == EACCES && s_clock_reads == clocks && s_room_thread_reads == threads);
    }
    printf("Room cold BSS start: PASS (first_uid=%d; one bind UID, no reset, pre-head skip, no retry)\n", first_uid);
    return 0;
}
#endif

int main(int argc, char **argv)
{
    const char *timing;
    const char *counts;
    const char *location;
#if defined(ISAAC_VITA_GL_TIME_PROFILE)
    const char *gl_time;
    const char *gl_scene;
    size_t max_gl_time_length;
    size_t max_gl_scene_length;
# if defined(ISAAC_VITA_GL_WRAPPER_TIME)
    const char *gl_wrapper, *gl_draw_split, *gl_uniform_split;
    size_t max_gl_wrapper_length, max_gl_draw_split_length,
           max_gl_uniform_split_length;
# endif
# if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE) && ISAAC_VITA_GL_TIME_SDK_SPARSE
    const char *gl_canonical, *gl_clear, *gl_queue;
    size_t max_gl_canonical_length, max_gl_clear_length, max_gl_queue_length;
#  if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE_ATTRIB)
    const char *gl_attrib;
    size_t max_gl_attrib_length;
#  endif
# endif
# if defined(ISAAC_VITA_STOCK_FBO_VALID_REGION)
    const char *valid_region;
    size_t max_valid_region_length;
# endif
#endif
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
    const char *guest_cache;
#endif
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
    const char *dispatch;
    const char *imports;
    size_t max_dispatch_length;
    size_t max_imports_length;
#endif
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    const char *texture_churn;
    const char *texture_delete;
#endif
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    const char *gl_redundancy;
#endif
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    const char *gl_typed_state;
#endif
#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
    const char *vitagl;
    const char *submission;
    const char *fusion;
#endif
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    const char *fill_totals;
    const char *fill_passes;
    size_t max_fill_totals_length;
    size_t max_fill_passes_length;
#endif
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION) || \
    defined(ISAAC_VITA_FBO_RASTER_SCALE)
    const char *fbo_policy;
#endif
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
    const char *coloroffset;
#endif
#if defined(ISAAC_VITA_COLOROFFSET_FS_PROBE)
    const char *fs_probe;
#endif
#if defined(ISAAC_VITA_PILL_BLOOM_BYPASS)
    const char *pill_bloom;
#endif
#if defined(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES)
    const char *bloom_half;
#endif
#if defined(ISAAC_VITA_POOP_FX_PROFILE)
    const char *poop_fx;
#endif
#if defined(ISAAC_VITA_LASER_PROFILE)
    const char *laser;
    const char *laser_cost;
    const char *laser_gpu;
#endif
#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
    const char *scheduler;
    const char *scheduler_pointer;
    const char *emission;
#endif
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
    const char *import_kinds;
    const char *import_hot;
#endif
#if defined(ISAAC_VITA_HEAP_CENSUS)
    const char *heap_record;
    size_t max_heap_length;
#endif
#if defined(ISAAC_VITA_KAGE_QUAD_FASTPATH)
    const char *kage_quad_record;
    size_t max_kage_quad_length;
#endif
    size_t max_timing_length;
    size_t max_counts_length;
    size_t max_location_length;
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
    size_t max_guest_cache_length;
#endif
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    size_t max_texture_churn_length;
    size_t max_texture_delete_length;
#endif
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    size_t max_gl_redundancy_length;
#endif
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    size_t max_gl_typed_state_length;
#endif
#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
    size_t max_vitagl_length;
    size_t max_submission_length;
    size_t max_fusion_length;
#endif
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
    size_t max_coloroffset_length;
#endif
#if defined(ISAAC_VITA_COLOROFFSET_FS_PROBE)
    size_t max_fs_probe_length;
#endif
#if defined(ISAAC_VITA_PILL_BLOOM_BYPASS)
    size_t max_pill_bloom_length;
#endif
#if defined(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES)
    size_t max_bloom_half_length;
#endif
#if defined(ISAAC_VITA_POOP_FX_PROFILE)
    size_t max_poop_fx_length;
#endif
#if defined(ISAAC_VITA_LASER_PROFILE)
    size_t max_laser_length;
    size_t max_laser_cost_length;
    size_t max_laser_gpu_length;
#endif
#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
    size_t max_scheduler_length;
    size_t max_scheduler_pointer_length;
    size_t max_emission_length;
#endif
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
    size_t max_import_kinds_length;
    size_t max_import_hot_length;
#endif
    unsigned next_line;

#if defined(ISAAC_VITA_ROOM_LOG_MARKERS) && ISAAC_VITA_ROOM_LOG_MARKERS
    if (argc == 2 && strcmp(argv[1], "--room-cold") == 0)
        return verify_room_cold_start(7);
    if (argc == 2 && strcmp(argv[1], "--room-cold-zero") == 0)
        return verify_room_cold_start(0);
    if (argc == 2 && strcmp(argv[1], "--room-cold-negative") == 0)
        return verify_room_cold_start(-1);
#else
    (void)argc;
    (void)argv;
#endif
#if defined(ISAAC_VITA_HEAP_CENSUS)
    set_heap_census_fixture();
#endif
#if defined(ISAAC_VITA_LASER_PROFILE)
    /* Hostile ordering: an outer token cannot close beneath a live nested
     * token.  The profiler must fail closed, clear only its private stack and
     * report the mismatch without affecting the following clean window. */
    kage_vita_phase_profile_reset();
    s_now = 100u;
    kage_vita_phase_profile_note_loop_head(1u);
    s_now = 101u;
    kage_vita_phase_profile_laser_begin(
        0x11111111u, 1u, 0u, 0u, 0u, 0u);
    s_now = 102u;
    kage_vita_phase_profile_laser_begin(
        0x22222222u, 39u, 1u, 1u, 1u, 0u);
    s_now = 103u;
    kage_vita_phase_profile_laser_end(0x11111111u, 0u);
    kage_vita_phase_profile_laser_end(0x22222222u, 0u);
    run_limiter_skip_window(1u);
    CHECK(strstr(s_logs,
                 "laser(r,h,z,p)=2,0,1,1") != NULL);
    CHECK(strstr(s_logs,
                 "seq(ov,mm,dg,clk)=0,1,0,0 bad=1\n") != NULL);
    memset(s_logs, 0, sizeof s_logs);
    s_log_length = 0u;
    s_log_calls = 0u;
    s_fusion_boundaries = 0u;
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    s_texture_window_takes = 0u;
    s_texture_window_order_bad = 0u;
#endif
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    s_fill_takes = 0u;
    s_fill_order_bad = 0u;
#endif
#if defined(ISAAC_VITA_HEAP_CENSUS)
    /* The hostile window above emitted its own ph120.mem (one lock section,
     * one mallinfo); the per-window counts below start from the clean one. */
    s_heap_census_calls = 0u;
    s_heap_mallinfo_calls = 0u;
#endif
#endif

    kage_vita_phase_profile_reset();
    CHECK(s_fusion_boundaries == 1u);
#if defined(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES)
    kage_vita_phase_profile_note_bloom_half_create(1u);
    kage_vita_phase_profile_note_bloom_half_create(2u);
#endif
    s_now = 1000u;
#if defined(ISAAC_VITA_LASER_HALO_PROFILE) && ISAAC_VITA_LASER_HALO_PROFILE
    /* Nonzero startup state and wrap must disappear into the baseline. */
    memset(&s_halo, 0xff, sizeof s_halo);
    s_halo.abi_version = ISAAC_LASER_HALO_STATS_ABI;
    s_halo.white_enabled = 1u;
    s_halo_reads = 0u;
#endif
#if defined(ISAAC_VITA_COLOROFFSET_PLAIN_FP16_ORACLE)
    /* Startup accumulation must be consumed, not attributed to window 1.
     * Invalid calls must neither consume that accumulation nor count a take. */
    s_half_window[0] = 123u;
    s_half_window[1] = 45u;
    s_half_takes = 0u;
    {
        uint32_t out[3] = {7u, 8u, 9u};
        CHECK(vglTakeIsaacColorOffsetPlainFp16Stats(NULL, 2u) == 0u);
        CHECK(vglTakeIsaacColorOffsetPlainFp16Stats(out, 3u) == 0u);
        CHECK(out[0] == 7u && out[1] == 8u && out[2] == 9u);
        CHECK(s_half_window[0] == 123u && s_half_window[1] == 45u);
        CHECK(s_half_takes == 0u);
    }
#endif
#if defined(ISAAC_VITA_GL_WRAPPER_TIME)
    /* Pre-window accumulation (loading-time wrappers) must be discarded by
     * the first loop head exactly like the gt buckets. */
    set_gl_wrapper_time_window(7u);
#endif
    kage_vita_phase_profile_note_loop_head(1u);
#if defined(ISAAC_VITA_COLOROFFSET_PLAIN_FP16_ORACLE)
    CHECK(s_half_takes == 1u);
    CHECK(s_half_window[0] == 0u && s_half_window[1] == 0u);
#endif
#if defined(ISAAC_VITA_GL_TIME_PROFILE)
    /* The first loop head must have discarded the pre-window accumulation
     * (one scene take) and zeroed the buckets. */
    CHECK(s_scene_takes == 1u);
    CHECK(s_scene_wait_takes == 1u);
    CHECK(g_isaac_vita_gl_time_profile.draw.calls == 0u);
    set_gl_time_window(29u);
# if defined(ISAAC_VITA_GL_WRAPPER_TIME)
    CHECK(g_isaac_vita_gl_wrapper_time.wrapper[0].calls == 0u &&
          g_isaac_vita_gl_wrapper_time.slot[1].total_us == 0u &&
          g_isaac_vita_gl_wrapper_time.draw[0].calls == 0u &&
          g_isaac_vita_gl_wrapper_time.draw_canonical == 0u &&
          g_isaac_vita_gl_wrapper_time.replay[1][2].total_us == 0u &&
          g_isaac_vita_gl_wrapper_time.uniform[4].calls == 0u);
    set_gl_wrapper_time_window(29u);
# endif
# if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE) && ISAAC_VITA_GL_TIME_SDK_SPARSE
    CHECK(s_sparse_takes == 1u && s_sparse_discards == 1u);
    CHECK(s_sparse_controls == 0u && s_sparse_next_window == 1u);
#  if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE_ATTRIB)
    /* Same discard at the first loop head as the sdk32 rows. */
    CHECK(s_attrib_takes == 1u && s_attrib_discards == 1u);
    CHECK(s_attrib_controls == 0u && s_attrib_next_window == 1u);
#  endif
# endif
#endif
    run_variable_window(1u);
    CHECK(s_fusion_boundaries == 2u);
#if defined(ISAAC_VITA_GL_TIME_PROFILE)
    CHECK(s_scene_takes == 2u);
    CHECK(g_isaac_vita_gl_time_profile.draw.calls == 0u &&
          g_isaac_vita_gl_time_profile.present.total_us == 0u);
# if defined(ISAAC_VITA_GL_WRAPPER_TIME)
    /* Take-and-zero at the same loop head as the gt buckets. */
    CHECK(g_isaac_vita_gl_wrapper_time.wrapper[8].calls == 0u &&
          g_isaac_vita_gl_wrapper_time.slot[0].total_us == 0u &&
          g_isaac_vita_gl_wrapper_time.draw[6].total_us == 0u &&
          g_isaac_vita_gl_wrapper_time.draw_canonical == 0u &&
          g_isaac_vita_gl_wrapper_time.replay[0][0].calls == 0u &&
          g_isaac_vita_gl_wrapper_time.uniform[0].total_us == 0u);
# endif
# if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE) && ISAAC_VITA_GL_TIME_SDK_SPARSE
    CHECK(s_sparse_takes == 2u && s_sparse_discards == 1u);
    CHECK(s_sparse_controls == 1u && s_sparse_next_window == 2u);
    CHECK(s_sparse_order_bad == 0u);
#  if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE_ATTRIB)
    CHECK(s_attrib_takes == 2u && s_attrib_discards == 1u);
    CHECK(s_attrib_controls == 1u && s_attrib_next_window == 2u);
    CHECK(s_attrib_order_bad == 0u);
#  endif
# endif
#endif
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    CHECK(s_texture_window_takes == 1u);
    CHECK(s_texture_window_order_bad == 0u);
#endif
    CHECK(s_log_calls == ORACLE_RECORDS_PER_WINDOW);
    timing = log_line(0u);
    counts = log_line(1u);
    next_line = 2u;
    location = log_line(next_line++);
#if defined(ISAAC_VITA_GL_TIME_PROFILE)
    gl_time = log_line(next_line++);
# if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE) && ISAAC_VITA_GL_TIME_SDK_SPARSE
    gl_canonical = log_line(next_line++);
    gl_clear = log_line(next_line++);
    gl_queue = log_line(next_line++);
#  if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE_ATTRIB)
    gl_attrib = log_line(next_line++);
#  endif
# endif
# if defined(ISAAC_VITA_GL_WRAPPER_TIME)
    gl_wrapper = log_line(next_line++);
    gl_draw_split = log_line(next_line++);
    gl_uniform_split = log_line(next_line++);
# endif
    gl_scene = log_line(next_line++);
# if defined(ISAAC_VITA_STOCK_FBO_VALID_REGION)
    valid_region = log_line(next_line++);
# endif
#endif
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    texture_churn = log_line(next_line++);
    texture_delete = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
    guest_cache = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
    dispatch = log_line(next_line++);
    imports = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    gl_redundancy = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    gl_typed_state = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
    vitagl = log_line(next_line++);
    submission = log_line(next_line++);
    fusion = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    fill_totals = log_line(next_line++);
    fill_passes = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION) || \
    defined(ISAAC_VITA_FBO_RASTER_SCALE)
    fbo_policy = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
    coloroffset = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_COLOROFFSET_FS_PROBE)
    fs_probe = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_PILL_BLOOM_BYPASS)
    pill_bloom = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES)
    bloom_half = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_POOP_FX_PROFILE)
    poop_fx = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_LASER_PROFILE)
    laser = log_line(next_line++);
    laser_cost = log_line(next_line++);
    laser_gpu = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
    scheduler = log_line(next_line++);
    scheduler_pointer = log_line(next_line++);
    emission = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
    import_kinds = log_line(next_line++);
    import_hot = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_HEAP_CENSUS)
    heap_record = log_line(next_line++);
    CHECK(heap_record != NULL);
    /* One heap-lock section and one mallinfo per window, lazy-growth
     * headroom/largest-bound math, every field in the pinned order. */
    CHECK(s_heap_census_calls == 1u && s_heap_mallinfo_calls == 1u);
    CHECK(strstr(heap_record,
                 "ph120.mem bid=phase:oracle win=1 loops=120 "
                 ORACLE_HEAP_FIELDS) != NULL);
#endif
#if defined(ISAAC_VITA_KAGE_QUAD_FASTPATH)
    kage_quad_record = log_line(next_line++);
    CHECK(kage_quad_record != NULL);
    /* One discard at the first loop head, one take per window flush. */
    CHECK(s_kage_quad_takes == 2u && s_kage_quad_discards == 1u);
    CHECK(strstr(kage_quad_record,
                 "ph120.kq bid=phase:oracle win=1 loops=120 mode=2 n=201 "
                 "h=202 c(x,k,g,s)=203,204,205,206 p(b,t,f,r,d)=207,208,209,"
                 "210,211 d(f,oa,ob,on,c,a,s,g,t)=220,221,222,223,224,225,"
                 "226,227,228\n") != NULL);
#endif
    CHECK(timing != NULL && counts != NULL && location != NULL);
#if defined(ISAAC_VITA_LASER_HALO_PROFILE) && ISAAC_VITA_LASER_HALO_PROFILE
    CHECK(s_halo_reads == 2u);
    CHECK(strstr(log_line(next_line++),
        "ph120.ha bid=phase:oracle win=1 loops=120 abi=2 from=0 attempt=4320 "
        "reject(meta,limits,state,plain,uv,output)=120,240,360,480,600,720 "
        "staged(light,cap)=840,960 big=120 white_on=1 ordinary(p,w)=240,120\n") != NULL);
#endif
#if defined(ISAAC_VITA_ROOM_LOG_MARKERS) && ISAAC_VITA_ROOM_LOG_MARKERS
    CHECK(strstr(log_line(next_line++), "ph120.rl bid=phase:oracle win=1 loops=120 ") != NULL);
#endif
    CHECK(next_line == ORACLE_RECORDS_PER_WINDOW);
    CHECK(strstr(timing,
        "ph120.t bid=phase:oracle win=1 loops=120 us(50,95,max)") != NULL);
    CHECK(strstr(timing, "svc=60/114/120") != NULL);
    CHECK(strstr(timing, "upd=160/214/220") != NULL);
    CHECK(strstr(timing, "rnd=1060/1114/1120") != NULL);
    CHECK(strstr(timing, "swp=560/614/620") != NULL);
    CHECK(strstr(timing, "lim=2060/2114/2120") != NULL);
#if defined(ISAAC_VITA_PHASE_PROFILE_OTHER)
    /* oth of loop s = (limiter_start - render_end) + (next service_start -
     * limiter_exit) = 10 + (2890 - 3s).  Loop 1 has no predecessor and loop
     * 120 closes in window 2: 119 samples 2543..2897, nearest-rank p50 at
     * index 59, p95 at index 113. */
    CHECK(strstr(timing, "lim=2060/2114/2120 oth=2720/2882/2897 all=") != NULL);
#else
    CHECK(strstr(timing, "lim=2060/2114/2120 all=") != NULL);
    CHECK(strstr(timing, "oth=") == NULL);
#endif
    CHECK(strstr(timing, "all=6060/6114/6120 ") != NULL);
    CHECK(strstr(timing, "bad=0 clamp=0\n") != NULL);
    /* There are 119 adjacent valid returns, not 120 intervals for 120
     * presents. The three changing intra-loop offsets add 3 us. */
    CHECK(strstr(timing,
        "gap(n,min,50,95,max)=119/6004/6063/6117/6122 ") != NULL);
    CHECK(strstr(counts,
        "ph120.c bid=phase:oracle win=1 loops=120 ") != NULL);
    CHECK(strstr(counts, "n(s,u,r,p,l,w)=120,120,120,120,120,120 ") != NULL);
    CHECK(strstr(counts,
                 ORACLE_WINDOW1_GUEST
                 "gl(d,c,t,p,i,s,u,a,v,f,x)="
                 "120,240,360,480,600,720,840,960,1080,1200,1320 "
                 "drop=0\n") != NULL);
    CHECK(strstr(location,
                 "ph120.a bid=phase:oracle win=1 loops=120 "
                 "loc(a,u,h,m)=5160,5280,5400,5640 tgt=5520\n") != NULL);
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION) || \
    defined(ISAAC_VITA_FBO_RASTER_SCALE)
    /* ph120.e sits right after the vitaGL records: window deltas of the
     * clear-elision / raster-scale counters, five clear fields only under
     * the depth-drop sub-mode (a, d appended so the OFF record is unchanged). */
    CHECK(fbo_policy != NULL);
# if defined(ISAAC_VITA_FBO_CLEAR_ELISION_DEPTH_DROP)
    CHECK(strstr(fbo_policy,
                 "ph120.e bid=phase:oracle win=1 loops=120 "
                 "clear(x,r,p,a,d)=5760,5880,6000,6120,6240 "
                 "raster(t,v,s)=6360,6480,6600\n") != NULL);
# else
    CHECK(strstr(fbo_policy,
                 "ph120.e bid=phase:oracle win=1 loops=120 "
                 "clear(x,r,p)=5760,5880,6000 "
                 "raster(t,v,s)=6360,6480,6600\n") != NULL);
    CHECK(strstr(fbo_policy, "clear(x,r,p,a,d)") == NULL);
# endif
#endif
#if defined(ISAAC_VITA_GL_TIME_PROFILE)
    CHECK(gl_time != NULL);
# if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE) && ISAAC_VITA_GL_TIME_SDK_SPARSE
    CHECK(strstr(gl_time,
                 "ph120.gt bid=phase:oracle win=1 loops=120 mode=sdk32 scope=scene "
                 "scene(n,b,e,bm)=10,1400,180,600 sdk(b,e)=16,17 "
                 "clocks=18 bucket_bad=19 bucket_sat=20 scene_check=legacy "
                 "ctl(n,us,max)=18,180,1800\n") != NULL);
    CHECK(gl_canonical != NULL && strstr(gl_canonical,
                 "ph120.gt bid=phase:oracle win=1 loops=120 mode=sdk32 scope=canonical "
                 "res=1 seen=11 selected=12 issued=13 unpaired=14 draw_errors=21 other=15 "
                 "outer(n,us,max)=11,110,1100 draw(n,us,max)=12,120,1200\n") != NULL);
    CHECK(gl_clear != NULL && strstr(gl_clear,
                 "ph120.gt bid=phase:oracle win=1 loops=120 mode=sdk32 scope=clear "
                 "outer(n,us,max)=13,130,1300 vertex(n,us,max)=14,140,1400 "
                 "fragment(n,us,max)=15,150,1500 draw(n,us,max)=16,160,1600\n") != NULL);
    CHECK(gl_queue != NULL && strstr(gl_queue,
                 "ph120.gt bid=phase:oracle win=1 loops=120 mode=sdk32 scope=queue "
                 "queue(n,us,max)=17,170,1700\n") != NULL);
#  if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE_ATTRIB)
    CHECK(gl_attrib != NULL && strstr(gl_attrib,
                 "ph120.gt bid=phase:oracle win=1 loops=120 mode=sdk32 scope=attrib "
                 "res=1 replays(en,di)=41,42 en(n,us,max)=44,440,4400 "
                 "di(n,us,max)=45,450,4500 bad=43\n") != NULL);
#  else
    CHECK(strstr(s_logs, "scope=attrib") == NULL);
#  endif
    CHECK(strstr(s_logs, "us(d,c,b,t,s,p)=") == NULL);
    CHECK(strstr(s_logs, "max(d,c,b)=") == NULL);
    CHECK(strstr(s_logs, "n(d,c,b,t,s,p)=") == NULL);
# else
    CHECK(strstr(gl_time,
                 "ph120.gt bid=phase:oracle win=1 loops=120 "
                 "us(d,c,b,t,s,p)=290,300,310,320,330,340 "
                 "max(d,c,b)=2900,3000,3100 "
                 "n(d,c,b,t,s,p)=29,30,31,32,33,34 "
                 "scene(n,b,e,bm)=10,1400,180,600 bad=0\n") != NULL);
    CHECK(strstr(s_logs, "mode=sdk32") == NULL);
# endif
# if defined(ISAAC_VITA_GL_WRAPPER_TIME)
    /* Window 1 carries the fixture set after the first loop head; the
     * pre-head fixture (base 7) was discarded, so no 7x value appears. */
    CHECK(gl_wrapper != NULL);
    CHECK(strstr(gl_wrapper,
                 "ph120.gw bid=phase:oracle win=1 loops=120 "
                 "w(d,c,b,t,s,p,a,u,o)=290,300,310,320,330,340,350,360,370 "
                 "n=29,30,31,32,33,34,35,36,37 k(en,di)=490,49/500,50 "
                 "bad=0\n") != NULL);
    CHECK(strstr(s_logs, "w(d,c,b,t,s,p,a,u,o)=70,") == NULL);
    /* ph120.gd / ph120.gu from the same take (draw split base+40, canonical
     * base+50, replay parts base+60, uniform kinds base+70). */
    CHECK(gl_draw_split != NULL);
    CHECK(strstr(gl_draw_split,
                 "ph120.gd bid=phase:oracle win=1 loops=120 "
                 "d(disp,sync,cls,cen,fbo,gl,tail)="
                 "690,700,710,720,730,740,750 n(dr,cq,ea,da)=69,79,90,93 "
                 "en(own,loc,gl)=890,900,910 di(own,loc,gl)=920,930,940 "
                 "bad=0\n") != NULL);
    CHECK(strstr(s_logs, "d(disp,sync,cls,cen,fbo,gl,tail)=470,") == NULL);
    CHECK(gl_uniform_split != NULL);
    CHECK(strstr(gl_uniform_split,
                 "ph120.gu bid=phase:oracle win=1 loops=120 "
                 "u(m4,4f,2f,1i,o)=990,99/1000,100/1010,101/1020,102/"
                 "1030,103 bad=0\n") != NULL);
    CHECK(strstr(s_logs, "u(m4,4f,2f,1i,o)=770,") == NULL);
# else
    CHECK(strstr(s_logs, "ph120.gw ") == NULL);
    CHECK(strstr(s_logs, "ph120.gd ") == NULL);
    CHECK(strstr(s_logs, "ph120.gu ") == NULL);
# endif
    /* Same take (the second) through the 0006 hook. */
    CHECK(gl_scene != NULL);
    CHECK(strstr(gl_scene,
                 "ph120.gx bid=phase:oracle win=1 loops=120 "
                 "end(f,d,max,slow)=160,20,90,6 ord(1,2,3,4+)=40,60,50,10 "
                 "rt(c,x,z)=2,0,4" ORACLE_GX_VR_WIN1 "\n") != NULL);
# if defined(ISAAC_VITA_STOCK_FBO_VALID_REGION)
    /* Same (second) take through the 0009 hook: o = counter 8, the four
     * census tuples with their window counts. */
    CHECK(valid_region != NULL);
    CHECK(strstr(valid_region,
                 "ph120.vz bid=phase:oracle win=1 loops=120 o=16 "
                 "sz=1024x1024/960x540/200,512x512/480x270/400,"
                 "256x256/240x135/600,128x128/120x67/800\n") != NULL);
# else
    CHECK(strstr(gl_scene, " vr(") == NULL);
# endif
#endif
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    CHECK(texture_churn != NULL);
    CHECK(strstr(texture_churn,
                 "ph120.x bid=phase:oracle win=1 loops=120 ") != NULL);
    CHECK(strstr(texture_churn,
                 "g=0/0/0/0/0 d=0 i=0/0/0/0 p=0/0/0 "
                 "px/b=0/0 us=0/0 cal=0 live=0/0/0 "
                 "bad/clk=0/0\n") != NULL);
    CHECK(texture_delete != NULL);
    CHECK(strstr(texture_delete,
                 "ph120.xd bid=phase:oracle win=1 loops=120 ") != NULL);
    CHECK(strstr(texture_delete,
                 "del=0/0 native-us=0/0 post-us=0/0 bad/clk=0/0\n") !=
          NULL);
#endif
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
    CHECK(strstr(guest_cache,
                 "ph120.g bid=phase:oracle win=1 loops=120 "
                 ORACLE_WINDOW1_CACHE) != NULL);
#endif
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
    CHECK(dispatch != NULL && imports != NULL);
    CHECK(strstr(dispatch,
                 "ph120.d bid=phase:oracle win=1 loops=120 "
                 ORACLE_WINDOW1_DISPATCH) != NULL);
    CHECK(strstr(imports,
                 "ph120.i bid=phase:oracle win=1 loops=120 "
                 ORACLE_WINDOW1_IMPORTS) != NULL);
#endif
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    CHECK(gl_redundancy != NULL);
    CHECK(strstr(gl_redundancy,
                 "ph120.o bid=phase:oracle win=1 loops=120 "
                 "glskip(p,u)=1440,1560\n") != NULL);
#endif
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    CHECK(gl_typed_state != NULL);
    CHECK(strstr(gl_typed_state,
                 "ph120.y bid=phase:oracle win=1 loops=120 "
                 "hit(a,t,b,d,v,e,p)="
                 "3960,4080,4200,4320,4440,4560,4680 "
                 "miss=4800 reject(i,u)=4920,5040"
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
                 " coal(d,c,p0,p1)=480,240,3,5"
#endif
                 "\n") != NULL);
#endif
#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
    CHECK(vitagl != NULL);
    CHECK(strstr(vitagl,
                 "ph120.v bid=phase:oracle win=1 loops=120 abi=2 en=1 "
                 "draw=120 coal=120/120 rej=0/0/0/0 streams=960/120 "
                 "vp=120/120/0/0/0/0/0 rel=0\n") != NULL);
    CHECK(submission != NULL && fusion != NULL);
    CHECK(strstr(submission,
                 "ph120.q bid=phase:oracle win=1 loops=120 "
                 "q(h,s,x,b,p,d,save)=1680,1800,1920,2040,2160,2280,2400 "
                 "io(i,v,n,vc,top,u)=1440,1920,720,480,4,120 "
                 "set(v,f,t)=120/240,360/480,600/720\n") != NULL);
    CHECK(strstr(fusion,
                 "ph120.f bid=phase:oracle win=1 loops=120 "
                 "idx=2520 blend(a,n,o)=2640,2760,2880 "
                 "shader(s,c)=3000,3120 fbo(d,o)=3240,3360 "
                 "atlas(s,c)=3480,3600 add(d,r)=3720,3840\n") != NULL);
#endif
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    CHECK(s_fill_takes == 1u && s_fill_order_bad == 0u &&
          s_fill_last_window == 1u);
    CHECK(fill_totals != NULL && fill_passes != NULL);
    CHECK(strstr(fill_totals,
                 "ph120.fa bid=phase:oracle win=1 loops=120 "
                 "draws=1 tri=2 kpx(u,c,clr)=3,4,5 max=6 "
                 "big=7 bad=8 syn=9 proj=10 prog(e,o)=16,17 "
                 "blend(a,d,o)=18,19,20 vp(w,h,n,miss)=12,13,14,15\n")
          != NULL);
    CHECK(strstr(fill_passes,
                 "ph120.fp bid=phase:oracle win=1 loops=120 "
                 "p0(d,kpx,c)=21,26,31 p1(d,kpx,c)=22,27,32 "
                 "p2(d,kpx,c)=23,28,33 p3(d,kpx,c)=24,29,34 "
                 "disp(d,kpx,c)=25,30,35 att(w,h)=36,37 tiles=11 cunk=38\n")
          != NULL);
#endif
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
    CHECK(coloroffset != NULL);
    CHECK(strstr(coloroffset,
                 "ph120.k bid=phase:oracle win=1 loops=120 abi=1 en=1 "
                 "src=120/120/120/120 opq=120/120/120/120 "
                 "fail=120/120/120/120/120/120/120/120 "
                  "cache=120/120/120 sta=120/120/120\n") != NULL);
#endif
#if defined(ISAAC_VITA_COLOROFFSET_FS_PROBE)
    /* Link fields are state (not deltas); draw/cache fields are deltas. */
    CHECK(fs_probe != NULL);
    CHECK(strstr(fs_probe,
                 "ph120.kp bid=phase:oracle win=1 loops=120 mode="
                 ORACLE_FS_PROBE_STR(ISAAC_VITA_COLOROFFSET_FS_PROBE)
                 " link(a,r)=1,1 lfail(s,c,x,k,r)=0,0,0,0,0 prog=7 "
                 "gxp=811/0badf00d src=546/12345678 "
                 "draw(q,b,o)=120,120,120 "
                 "cache(c,r,f,x)=120,120,120,120 plain(q,b,f)=240,120,120"
#if defined(ISAAC_VITA_COLOROFFSET_PLAIN_VERTEX_PAIR_ORACLE)
                 " pair(q,b,f)=240,120,120"
#endif
#if defined(ISAAC_VITA_COLOROFFSET_PLAIN_FP16_ORACLE)
                 " half(q,b)=240,120"
#endif
                 "\n") != NULL);
#if defined(ISAAC_VITA_COLOROFFSET_PLAIN_FP16_ORACLE)
    CHECK(s_half_takes == 2u);
    CHECK(s_half_window[0] == 0u && s_half_window[1] == 0u);
#else
    CHECK(strstr(fs_probe, " half(q,b)=") == NULL);
#endif
#endif
#if defined(ISAAC_VITA_PILL_BLOOM_BYPASS)
    CHECK(pill_bloom != NULL);
    CHECK(strstr(pill_bloom,
                 "ph120.b bid=phase:oracle win=1 loops=120 "
                 "bloom(skip c,o)=30,30 bad=0\n") != NULL);
#endif
#if defined(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES)
    CHECK(bloom_half != NULL);
    CHECK(strstr(bloom_half,
                 "ph120.h bid=phase:oracle win=1 loops=120 "
                  "half(create i,a,r)=1,1,0 bloom(c,o)=30,30 bad=0\n") != NULL);
#endif
#if defined(ISAAC_VITA_POOP_FX_PROFILE)
    CHECK(poop_fx != NULL);
    CHECK(strstr(poop_fx,
                 "ph120.p bid=phase:oracle win=1 loops=120 abi=1 "
                 "add(p,b,o,u)=1,1,1,1 active(n,clouds)=30,90 "
                 "cd(min,max,last)=151,180,151 cap(n,skip)=") != NULL);
# if defined(ISAAC_VITA_POOP_FX_SINGLE_CLOUD)
    CHECK(strstr(poop_fx, "cap(n,skip)=30,60 bad=1\n") != NULL);
# else
    CHECK(strstr(poop_fx, "cap(n,skip)=0,0 bad=1\n") != NULL);
# endif
#endif
#if defined(ISAAC_VITA_LASER_PROFILE)
    CHECK(laser != NULL && laser_cost != NULL && laser_gpu != NULL);
    CHECK(strstr(laser,
                 "ph120.l bid=phase:oracle win=1 loops=120 "
                 "laser(r,h,z,p)=240,120,120,120 "
                 "src(1,10_999,o)=120,120,0 var(0,1,2,o)=120,120,0,0 "
                 "sub(1_3,o)=120,120 bad=0\n") != NULL);
    CHECK(strstr(laser_cost,
                 "ph120.m bid=phase:oracle win=1 loops=120 "
                 "cost(n/us/mx) z=120/3840/32 p=120/2520/21 "
                 "h=120/1200/10 other=0 seq(ov,mm,dg,clk)=0,0,0,0 "
                 "bad=0\n") != NULL);
# if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
    CHECK(strstr(laser_gpu,
                 "ph120.n bid=phase:oracle win=1 loops=120 "
                 "submit(en,ok,ng)=1,360,0 draw(z,p,h)=120,120,120 "
                 "idx(z,p,h)=720,720,720 vtx(z,p,h)=480,480,480 "
                 "bad=0\n") != NULL);
# else
    CHECK(strstr(laser_gpu,
                 "ph120.n bid=phase:oracle win=1 loops=120 "
                 "submit(en,ok,ng)=0,0,360 draw(z,p,h)=0,0,0 "
                 "idx(z,p,h)=0,0,0 vtx(z,p,h)=0,0,0 bad=0\n") != NULL);
# endif
#endif
#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
    CHECK(scheduler != NULL && scheduler_pointer != NULL);
    CHECK(strstr(scheduler,
                 "ph120.s bid=phase:oracle win=1 loops=120 at=") != NULL);
    /* ph120.w follows ph120.r; no previous window, nothing to report yet. */
    CHECK(emission != NULL);
    CHECK(strstr(emission,
                 "ph120.w bid=phase:oracle win=1 loops=120 "
                 "emit(us,max)=0,0\n") != NULL);
    CHECK(strstr(scheduler_pointer,
                 "ph120.r bid=phase:oracle win=1 loops=120 "
                 "ctr(p,a,v)=0,0,0 gptr(p,a,v)=0,0,0 "
                 "frame(last:s,b,e)=00000000,0,0 "
                 "frame(fail:s,b,e)=00000000,0,0\n") != NULL);
#endif
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
    CHECK(import_kinds != NULL && import_hot != NULL);
# if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
    /* The table hunk's notes land on ID 5 (math; hot slot 5 = fr in this
     * oracle's ID->slot map) and IDs 60/61 (unresolved here -> oth); the
     * identity holds on the derived g(c,l). */
    CHECK(strstr(import_kinds,
                 "ph120.ik bid=phase:oracle win=1 loops=120 "
                 "imp(t,sync,heap,mem,crt,pcom,math,lua,oth)="
                 "480,15,15,15,15,15,135,15,255 " ORACLE_WINDOW1_IK_DYN
                 " ent=0 pace(c,us)=0,0 "
                 "id(g,s)=0,0 ok=1\n") != NULL);
    CHECK(strstr(import_hot,
                 "ph120.ih bid=phase:oracle win=1 loops=120 "
                 "hot(en,lv,te,sl,ma,fr,ca,re,mc,ms,mm,tg,qp,fl)="
                 "15,15,15,15,15,135,15,15,0,0,0,0,0,0 ok=1\n") != NULL);
# else
    CHECK(strstr(import_kinds,
                 "ph120.ik bid=phase:oracle win=1 loops=120 "
                 "imp(t,sync,heap,mem,crt,pcom,math,lua,oth)="
                 "120,15,15,15,15,15,15,15,15 dyn=0 ent=0 pace(c,us)=0,0 "
                 "id(g,s)=0,0 ok=1\n") != NULL);
    CHECK(strstr(import_hot,
                 "ph120.ih bid=phase:oracle win=1 loops=120 "
                 "hot(en,lv,te,sl,ma,fr,ca,re,mc,ms,mm,tg,qp,fl)="
                 "15,15,15,15,15,15,15,15,0,0,0,0,0,0 ok=1\n") != NULL);
# endif
#endif

#if defined(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES)
    /* A second accepted recreation is not allowed to disappear inside an
     * aggregate "some create applied" receipt. */
    kage_vita_phase_profile_note_bloom_half_create(2u);
#endif
    run_limiter_skip_window(121u);
    CHECK(s_fusion_boundaries == 3u);
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    CHECK(s_texture_window_takes == 2u);
    CHECK(s_texture_window_order_bad == 0u);
#endif
    CHECK(s_log_calls == 2U * ORACLE_RECORDS_PER_WINDOW);
    timing = log_line(ORACLE_RECORDS_PER_WINDOW);
    counts = log_line(ORACLE_RECORDS_PER_WINDOW + 1U);
    next_line = ORACLE_RECORDS_PER_WINDOW + 2u;
    location = log_line(next_line++);
    CHECK(location != NULL);
#if defined(ISAAC_VITA_GL_TIME_PROFILE)
    /* Nothing accumulated in the second window: all-zero buckets, third
     * scene take. */
    gl_time = log_line(next_line++);
    CHECK(gl_time != NULL);
# if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE) && ISAAC_VITA_GL_TIME_SDK_SPARSE
    CHECK(s_sparse_takes == 3u && s_sparse_discards == 1u);
    CHECK(s_sparse_controls == 2u && s_sparse_next_window == 3u);
    CHECK(s_sparse_order_bad == 0u);
#  if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE_ATTRIB)
    CHECK(s_attrib_takes == 3u && s_attrib_discards == 1u);
    CHECK(s_attrib_controls == 2u && s_attrib_next_window == 3u);
    CHECK(s_attrib_order_bad == 0u);
#  endif
    CHECK(strstr(gl_time,
                 "ph120.gt bid=phase:oracle win=2 loops=240 mode=sdk32 scope=scene "
                 "scene(n,b,e,bm)=15,2100,270,900 sdk(b,e)=26,27 "
                 "clocks=28 bucket_bad=29 bucket_sat=30 scene_check=legacy "
                 "ctl(n,us,max)=28,280,2800\n") != NULL);
    gl_canonical = log_line(next_line++);
    gl_clear = log_line(next_line++);
    gl_queue = log_line(next_line++);
    CHECK(gl_canonical != NULL && strstr(gl_canonical,
                 "ph120.gt bid=phase:oracle win=2 loops=240 mode=sdk32 scope=canonical "
                 "res=2 seen=21 selected=22 issued=23 unpaired=24 draw_errors=31 other=25 "
                 "outer(n,us,max)=21,210,2100 draw(n,us,max)=22,220,2200\n") != NULL);
    CHECK(gl_clear != NULL && strstr(gl_clear,
                 "ph120.gt bid=phase:oracle win=2 loops=240 mode=sdk32 scope=clear "
                 "outer(n,us,max)=23,230,2300 vertex(n,us,max)=24,240,2400 "
                 "fragment(n,us,max)=25,250,2500 draw(n,us,max)=26,260,2600\n") != NULL);
    CHECK(gl_queue != NULL && strstr(gl_queue,
                 "ph120.gt bid=phase:oracle win=2 loops=240 mode=sdk32 scope=queue "
                 "queue(n,us,max)=27,270,2700\n") != NULL);
#  if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE_ATTRIB)
    gl_attrib = log_line(next_line++);
    CHECK(gl_attrib != NULL && strstr(gl_attrib,
                 "ph120.gt bid=phase:oracle win=2 loops=240 mode=sdk32 scope=attrib "
                 "res=2 replays(en,di)=51,52 en(n,us,max)=54,540,5400 "
                 "di(n,us,max)=55,550,5500 bad=53\n") != NULL);
#  else
    CHECK(strstr(s_logs, "scope=attrib") == NULL);
#  endif
    CHECK(strstr(s_logs, "us(d,c,b,t,s,p)=") == NULL);
    CHECK(strstr(s_logs, "max(d,c,b)=") == NULL);
    CHECK(strstr(s_logs, "n(d,c,b,t,s,p)=") == NULL);
# else
    CHECK(strstr(gl_time,
                 "ph120.gt bid=phase:oracle win=2 loops=240 "
                 "us(d,c,b,t,s,p)=0,0,0,0,0,0 max(d,c,b)=0,0,0 "
                 "n(d,c,b,t,s,p)=0,0,0,0,0,0 "
                 "scene(n,b,e,bm)=15,2100,270,900 bad=0\n") != NULL);
    CHECK(strstr(s_logs, "mode=sdk32") == NULL);
# endif
# if defined(ISAAC_VITA_GL_WRAPPER_TIME)
    /* Zeroed by the window-1 take; nothing accumulated since. */
    gl_wrapper = log_line(next_line++);
    CHECK(gl_wrapper != NULL);
    CHECK(strstr(gl_wrapper,
                 "ph120.gw bid=phase:oracle win=2 loops=240 "
                 "w(d,c,b,t,s,p,a,u,o)=0,0,0,0,0,0,0,0,0 "
                 "n=0,0,0,0,0,0,0,0,0 k(en,di)=0,0/0,0 bad=0\n") != NULL);
    gl_draw_split = log_line(next_line++);
    CHECK(gl_draw_split != NULL);
    CHECK(strstr(gl_draw_split,
                 "ph120.gd bid=phase:oracle win=2 loops=240 "
                 "d(disp,sync,cls,cen,fbo,gl,tail)=0,0,0,0,0,0,0 "
                 "n(dr,cq,ea,da)=0,0,0,0 en(own,loc,gl)=0,0,0 "
                 "di(own,loc,gl)=0,0,0 bad=0\n") != NULL);
    gl_uniform_split = log_line(next_line++);
    CHECK(gl_uniform_split != NULL);
    CHECK(strstr(gl_uniform_split,
                 "ph120.gu bid=phase:oracle win=2 loops=240 "
                 "u(m4,4f,2f,1i,o)=0,0/0,0/0,0/0,0/0,0 bad=0\n") != NULL);
# endif
    gl_scene = log_line(next_line++);
    CHECK(gl_scene != NULL);
    CHECK(strstr(gl_scene,
                 "ph120.gx bid=phase:oracle win=2 loops=240 "
                 "end(f,d,max,slow)=240,30,135,9 ord(1,2,3,4+)=60,90,75,15 "
                 "rt(c,x,z)=3,0,6" ORACLE_GX_VR_WIN2 "\n") != NULL);
    CHECK(s_scene_wait_takes == s_scene_takes);
# if defined(ISAAC_VITA_STOCK_FBO_VALID_REGION)
    valid_region = log_line(next_line++);
    CHECK(valid_region != NULL);
    CHECK(strstr(valid_region,
                 "ph120.vz bid=phase:oracle win=2 loops=240 o=24 "
                 "sz=1024x1024/960x540/300,512x512/480x270/600,"
                 "256x256/240x135/900,128x128/120x67/1200\n") != NULL);
    /* The 0009 hook is taken with the other two at every take, the
     * discarded first one included. */
    CHECK(s_valid_region_takes == s_scene_takes);
# endif
#endif
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    texture_churn = log_line(next_line++);
    texture_delete = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
    guest_cache = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
    dispatch = log_line(next_line++);
    imports = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    gl_redundancy = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    gl_typed_state = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
    vitagl = log_line(next_line++);
    submission = log_line(next_line++);
    fusion = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    fill_totals = log_line(next_line++);
    fill_passes = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION) || \
    defined(ISAAC_VITA_FBO_RASTER_SCALE)
    fbo_policy = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
    coloroffset = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_COLOROFFSET_FS_PROBE)
    fs_probe = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_PILL_BLOOM_BYPASS)
    pill_bloom = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES)
    bloom_half = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_POOP_FX_PROFILE)
    poop_fx = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_LASER_PROFILE)
    laser = log_line(next_line++);
    laser_cost = log_line(next_line++);
    laser_gpu = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
    scheduler = log_line(next_line++);
    scheduler_pointer = log_line(next_line++);
    emission = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
    import_kinds = log_line(next_line++);
    import_hot = log_line(next_line++);
#endif
#if defined(ISAAC_VITA_HEAP_CENSUS)
    heap_record = log_line(next_line++);
    CHECK(heap_record != NULL);
    CHECK(s_heap_census_calls == 2u && s_heap_mallinfo_calls == 2u);
    CHECK(strstr(heap_record,
                 "ph120.mem bid=phase:oracle win=2 loops=240 "
                 ORACLE_HEAP_FIELDS) != NULL);
#endif
#if defined(ISAAC_VITA_KAGE_QUAD_FASTPATH)
    kage_quad_record = log_line(next_line++);
    CHECK(kage_quad_record != NULL);
    CHECK(s_kage_quad_takes == 3u && s_kage_quad_discards == 1u);
    CHECK(strstr(kage_quad_record,
                 "ph120.kq bid=phase:oracle win=2 loops=240 mode=2 n=301 "
                 "h=302 c(x,k,g,s)=303,304,305,306 p(b,t,f,r,d)=307,308,309,"
                 "310,311 d(f,oa,ob,on,c,a,s,g,t)=320,321,322,323,324,325,"
                 "326,327,328\n") != NULL);
#endif
    CHECK(timing != NULL && counts != NULL);
#if defined(ISAAC_VITA_LASER_HALO_PROFILE) && ISAAC_VITA_LASER_HALO_PROFILE
    CHECK(s_halo_reads == 3u);
    CHECK(strstr(log_line(next_line++),
        "ph120.ha bid=phase:oracle win=2 loops=240 abi=2 from=1 attempt=5280 "
        "reject(meta,limits,state,plain,uv,output)=240,360,480,600,720,840 "
        "staged(light,cap)=960,1080 big=240 white_on=1 ordinary(p,w)=360,240\n") != NULL);
#endif
#if defined(ISAAC_VITA_ROOM_LOG_MARKERS) && ISAAC_VITA_ROOM_LOG_MARKERS
    CHECK(strstr(log_line(next_line++), "ph120.rl bid=phase:oracle win=2 loops=240 ") != NULL);
#endif
    CHECK(next_line == 2u * ORACLE_RECORDS_PER_WINDOW);
    CHECK(strstr(timing, "win=2 loops=240") != NULL);
    CHECK(strstr(timing, "lim=30/57/60") != NULL);
#if defined(ISAAC_VITA_PHASE_PROFILE_OTHER)
    {
        /* The first sample closed in this window is loop 120 of the previous
         * one: 10 + (post-emission service start - limiter exit) = 2540 plus
         * the 777 us this oracle charges per record the emission printed.
         * Odd loops here run the limiter: 4935 - limiter_sample (4875..4934);
         * even loops skip it: 4935.  120 samples either way. */
        unsigned boundary =
            2540u + 777u * (unsigned)ORACLE_RECORDS_PER_WINDOW;
        char expected[48];

        if (boundary < 4875u)
            snprintf(expected, sizeof expected,
                     "lim=30/57/60 oth=4933/4935/4935 all=");
        else
            snprintf(expected, sizeof expected,
                     "lim=30/57/60 oth=4934/4935/%u all=", boundary);
        CHECK(strstr(timing, expected) != NULL);
    }
#else
    CHECK(strstr(timing, "lim=30/57/60 all=") != NULL);
#endif
    CHECK(strstr(timing, "all=5000/5000/5000 ") != NULL);
    CHECK(strstr(timing, "bad=0 clamp=0\n") != NULL);
    {
        char expected_gap[96];

        /* Carry the last return across the report. Its synchronous logger
         * cost belongs to the first gap, unlike Whole's deliberate exclusion. */
        snprintf(expected_gap, sizeof expected_gap,
                 "gap(n,min,50,95,max)=120/5000/5000/5000/%u ",
                 5182u + 777u * ORACLE_RECORDS_PER_WINDOW);
        CHECK(strstr(timing, expected_gap) != NULL);
    }
    CHECK(strstr(counts, "win=2 loops=240") != NULL);
    CHECK(strstr(counts, "n(s,u,r,p,l,w)=120,120,120,120,60,120 ") != NULL);
    CHECK(strstr(counts,
                 ORACLE_WINDOW2_GUEST
                 "gl(d,c,t,p,i,s,u,a,v,f,x)="
                 "240,360,480,600,720,840,960,1080,1200,1320,1440 "
                 "drop=0\n") != NULL);
    CHECK(strstr(location,
                 "ph120.a bid=phase:oracle win=2 loops=240 "
                 "loc(a,u,h,m)=5280,5400,5520,5760 tgt=5640\n") != NULL);
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION) || \
    defined(ISAAC_VITA_FBO_RASTER_SCALE)
    CHECK(fbo_policy != NULL);
# if defined(ISAAC_VITA_FBO_CLEAR_ELISION_DEPTH_DROP)
    CHECK(strstr(fbo_policy,
                 "ph120.e bid=phase:oracle win=2 loops=240 "
                 "clear(x,r,p,a,d)=5880,6000,6120,6240,6360 "
                 "raster(t,v,s)=6480,6600,6720\n") != NULL);
# else
    CHECK(strstr(fbo_policy,
                 "ph120.e bid=phase:oracle win=2 loops=240 "
                 "clear(x,r,p)=5880,6000,6120 "
                 "raster(t,v,s)=6480,6600,6720\n") != NULL);
# endif
#endif
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    CHECK(texture_churn != NULL);
    CHECK(strstr(texture_churn,
                 "ph120.x bid=phase:oracle win=2 loops=240 ") != NULL);
    CHECK(texture_delete != NULL);
    CHECK(strstr(texture_delete,
                 "ph120.xd bid=phase:oracle win=2 loops=240 ") != NULL);
#endif
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
    CHECK(strstr(guest_cache,
                 "ph120.g bid=phase:oracle win=2 loops=240 "
                 ORACLE_WINDOW2_CACHE) != NULL);
#endif
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
    CHECK(dispatch != NULL && imports != NULL);
    CHECK(strstr(dispatch,
                 "ph120.d bid=phase:oracle win=2 loops=240 "
                 ORACLE_WINDOW2_DISPATCH) != NULL);
    CHECK(strstr(imports,
                 "ph120.i bid=phase:oracle win=2 loops=240 "
                 ORACLE_WINDOW2_IMPORTS) != NULL);
#endif
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    CHECK(gl_redundancy != NULL);
    CHECK(strstr(gl_redundancy,
                 "ph120.o bid=phase:oracle win=2 loops=240 "
                 "glskip(p,u)=1560,1680\n") != NULL);
#endif
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    CHECK(gl_typed_state != NULL);
    CHECK(strstr(gl_typed_state,
                 "ph120.y bid=phase:oracle win=2 loops=240 "
                 "hit(a,t,b,d,v,e,p)="
                 "4080,4200,4320,4440,4560,4680,4800 "
                 "miss=4920 reject(i,u)=5040,5160"
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
                 " coal(d,c,p0,p1)=960,480,5,1"
#endif
                 "\n") != NULL);
#endif
#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
    CHECK(vitagl != NULL);
    CHECK(strstr(vitagl,
                 "ph120.v bid=phase:oracle win=2 loops=240 abi=2 en=1 "
                 "draw=240 coal=240/240 rej=0/0/0/0 streams=1920/240 "
                 "vp=240/240/0/0/0/0/0 rel=0\n") != NULL);
    CHECK(strstr(submission,
                 "ph120.q bid=phase:oracle win=2 loops=240 "
                 "q(h,s,x,b,p,d,save)=1800,1920,2040,2160,2280,2400,2520 "
                 "io(i,v,n,vc,top,u)=2880,3840,1440,960,8,240 "
                 "set(v,f,t)=240/360,480/600,720/840\n") != NULL);
    CHECK(strstr(fusion,
                 "ph120.f bid=phase:oracle win=2 loops=240 "
                 "idx=2640 blend(a,n,o)=2760,2880,3000 "
                 "shader(s,c)=3120,3240 fbo(d,o)=3360,3480 "
                 "atlas(s,c)=3600,3720 add(d,r)=3840,3960\n") != NULL);
#endif
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    CHECK(s_fill_takes == 2u && s_fill_order_bad == 0u &&
          s_fill_last_window == 2u);
    CHECK(fill_totals != NULL && fill_passes != NULL);
    CHECK(strstr(fill_totals,
                 "ph120.fa bid=phase:oracle win=2 loops=240 "
                 "draws=101 tri=102 kpx(u,c,clr)=103,104,105 max=106 "
                 "big=107 bad=108 syn=109 proj=110 prog(e,o)=116,117 "
                 "blend(a,d,o)=118,119,120 "
                 "vp(w,h,n,miss)=112,113,114,115\n") != NULL);
    CHECK(strstr(fill_passes,
                 "ph120.fp bid=phase:oracle win=2 loops=240 "
                 "p0(d,kpx,c)=121,126,131 p1(d,kpx,c)=122,127,132 "
                 "p2(d,kpx,c)=123,128,133 p3(d,kpx,c)=124,129,134 "
                 "disp(d,kpx,c)=125,130,135 att(w,h)=136,137 tiles=111 "
                 "cunk=138\n")
          != NULL);
#endif
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
    CHECK(coloroffset != NULL);
    CHECK(strstr(coloroffset,
                 "ph120.k bid=phase:oracle win=2 loops=240 abi=1 en=1 "
                 "src=240/240/240/240 opq=240/240/240/240 "
                 "fail=240/240/240/240/240/240/240/240 "
                  "cache=240/240/240 sta=240/240/240\n") != NULL);
#endif
#if defined(ISAAC_VITA_COLOROFFSET_FS_PROBE)
    CHECK(fs_probe != NULL);
    CHECK(strstr(fs_probe,
                 "ph120.kp bid=phase:oracle win=2 loops=240 mode="
                 ORACLE_FS_PROBE_STR(ISAAC_VITA_COLOROFFSET_FS_PROBE)
                 " link(a,r)=1,1 lfail(s,c,x,k,r)=0,0,0,0,0 prog=7 "
                 "gxp=811/0badf00d src=546/12345678 "
                 "draw(q,b,o)=240,240,240 "
                 "cache(c,r,f,x)=240,240,240,240 plain(q,b,f)=480,240,240"
#if defined(ISAAC_VITA_COLOROFFSET_PLAIN_VERTEX_PAIR_ORACLE)
                 " pair(q,b,f)=480,240,240"
#endif
#if defined(ISAAC_VITA_COLOROFFSET_PLAIN_FP16_ORACLE)
                 " half(q,b)=480,240"
#endif
                 "\n") != NULL);
#if defined(ISAAC_VITA_COLOROFFSET_PLAIN_FP16_ORACLE)
    CHECK(s_half_takes == 3u);
    CHECK(s_half_window[0] == 0u && s_half_window[1] == 0u);
#else
    CHECK(strstr(fs_probe, " half(q,b)=") == NULL);
#endif
#endif
#if defined(ISAAC_VITA_PILL_BLOOM_BYPASS)
    CHECK(pill_bloom != NULL);
    CHECK(strstr(pill_bloom,
                 "ph120.b bid=phase:oracle win=2 loops=240 "
                 "bloom(skip c,o)=0,0 bad=0\n") != NULL);
#endif
#if defined(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES)
    CHECK(bloom_half != NULL);
    CHECK(strstr(bloom_half,
                 "ph120.h bid=phase:oracle win=2 loops=240 "
                  "half(create i,a,r)=1,2,0 bloom(c,o)=0,0 bad=1\n") != NULL);
#endif
#if defined(ISAAC_VITA_POOP_FX_PROFILE)
    CHECK(poop_fx != NULL);
    CHECK(strstr(poop_fx,
                 "ph120.p bid=phase:oracle win=2 loops=240 abi=1 "
                 "add(p,b,o,u)=0,0,0,0 active(n,clouds)=0,0 "
                 "cd(min,max,last)=0,0,0 cap(n,skip)=0,0 bad=0\n") != NULL);
#endif
#if defined(ISAAC_VITA_LASER_PROFILE)
    CHECK(laser != NULL && laser_cost != NULL && laser_gpu != NULL);
    CHECK(strstr(laser,
                 "ph120.l bid=phase:oracle win=2 loops=240 "
                 "laser(r,h,z,p)=0,0,0,0 src(1,10_999,o)=0,0,0 "
                 "var(0,1,2,o)=0,0,0,0 sub(1_3,o)=0,0 bad=0\n") != NULL);
    CHECK(strstr(laser_cost,
                 "ph120.m bid=phase:oracle win=2 loops=240 "
                 "cost(n/us/mx) z=0/0/0 p=0/0/0 h=0/0/0 other=0 "
                 "seq(ov,mm,dg,clk)=0,0,0,0 bad=0\n") != NULL);
    CHECK(strstr(laser_gpu,
                 "ph120.n bid=phase:oracle win=2 loops=240 "
                 "submit(en,ok,ng)=") != NULL);
    CHECK(strstr(laser_gpu,
                 "draw(z,p,h)=0,0,0 idx(z,p,h)=0,0,0 "
                 "vtx(z,p,h)=0,0,0 bad=0\n") != NULL);
#endif
#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
    CHECK(scheduler != NULL && scheduler_pointer != NULL);
    CHECK(strstr(scheduler,
                 "ph120.s bid=phase:oracle win=2 loops=240 at=") != NULL);
    {
        /* The first window's emission block held exactly its
         * ORACLE_RECORDS_PER_WINDOW logger calls (777 us each in this
         * oracle's clock) and nothing else advanced the clock inside the
         * bracket, so emit(us,max) names that whole block, no more, no less. */
        char expected_emit[96];

        snprintf(expected_emit, sizeof expected_emit,
                 "ph120.w bid=phase:oracle win=2 loops=240 "
                 "emit(us,max)=%u,%u\n",
                 777u * (unsigned)ORACLE_RECORDS_PER_WINDOW,
                 777u * (unsigned)ORACLE_RECORDS_PER_WINDOW);
        CHECK(emission != NULL && strstr(emission, expected_emit) != NULL);
    }
    CHECK(strstr(scheduler_pointer,
                 "ph120.r bid=phase:oracle win=2 loops=240 "
                 "ctr(p,a,v)=0,0,0 gptr(p,a,v)=0,0,0 "
                 "frame(last:s,b,e)=00000000,0,0 "
                 "frame(fail:s,b,e)=00000000,0,0\n") != NULL);
#endif
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
    /* The second window carried the injected residuals: the gate must print
     * them and clear ok while the per-kind split itself stays exact. */
    CHECK(import_kinds != NULL && import_hot != NULL);
# if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
    CHECK(strstr(import_kinds,
                 "ph120.ik bid=phase:oracle win=2 loops=240 "
                 "imp(t,sync,heap,mem,crt,pcom,math,lua,oth)="
                 "241,15,15,15,15,15,15,15,135 " ORACLE_WINDOW2_IK_DYN
                 " ent=0 pace(c,us)=0,0 "
                 "id(g,s)=-8,1 ok=0\n") != NULL);
# else
    CHECK(strstr(import_kinds,
                 "ph120.ik bid=phase:oracle win=2 loops=240 "
                 "imp(t,sync,heap,mem,crt,pcom,math,lua,oth)="
                 "121,15,15,15,15,15,15,15,15 dyn=7 ent=0 pace(c,us)=0,0 "
                 "id(g,s)=-8,1 ok=0\n") != NULL);
# endif
    CHECK(strstr(import_hot,
                 "ph120.ih bid=phase:oracle win=2 loops=240 "
                 "hot(en,lv,te,sl,ma,fr,ca,re,mc,ms,mm,tg,qp,fl)="
                 "15,15,15,15,15,15,15,15,0,0,0,0,0,0 ok=1\n") != NULL);
#endif
    CHECK(log_line(2U * ORACLE_RECORDS_PER_WINDOW) != NULL &&
          *log_line(2U * ORACLE_RECORDS_PER_WINDOW) == '\0');

#if defined(ISAAC_VITA_HEAP_CENSUS)
    /* Out-of-cadence records from the entry owner: each why= claimed once
     * per process, mallinfo skipped (and ok cleared) while the guest heap is
     * terminal, and no record at all before the first profiled loop head. */
    memset(s_logs, 0, sizeof s_logs);
    s_log_length = 0u;
    s_log_calls = 0u;
    s_heap_mallinfo_calls = 0u;
    kage_vita_phase_profile_heap_census_final("badalloc");
    CHECK(s_log_calls == 1u && s_heap_mallinfo_calls == 1u);
    CHECK(strstr(log_line(0u),
                 "ph120.mem bid=phase:oracle win=2 loops=241 "
                 "mi(a,u,f,n,top)=70000000,") != NULL);
    CHECK(strstr(log_line(0u), " slot=1 ok=1 why=badalloc\n") != NULL);
    kage_vita_phase_profile_heap_census_final("badalloc");
    CHECK(s_log_calls == 1u);
    s_heap_census.terminal = 1u;
    kage_vita_phase_profile_heap_census_final("exit");
    CHECK(s_log_calls == 2u && s_heap_mallinfo_calls == 1u);
    CHECK(strstr(log_line(1u),
                 "ph120.mem bid=phase:oracle win=2 loops=241 "
                 "mi(a,u,f,n,top)=0,0,0,0,0 hr=84934656 la=84934656 "
                 "led(n,b)=108583,60000000 ") != NULL);
    CHECK(strstr(log_line(1u), " slot=1 ok=0 why=exit\n") != NULL);
    kage_vita_phase_profile_heap_census_final("exit");
    kage_vita_phase_profile_heap_census_final(NULL);
    CHECK(s_log_calls == 2u);
    s_heap_census.terminal = 0u;
    kage_vita_phase_profile_reset();
    kage_vita_phase_profile_heap_census_final("exit");
    CHECK(s_log_calls == 2u);
#endif

    memset(s_logs, 0, sizeof s_logs);
    s_log_length = 0u;
    s_log_calls = 0u;
    CHECK(verify_present_gap_edges() == 0);
#if defined(ISAAC_VITA_LASER_HALO_PROFILE) && ISAAC_VITA_LASER_HALO_PROFILE
    /* ABI loss invalidates the interval. Restore needs one baseline, not
     * invented zero counts or subtraction across incompatible snapshots. */
    kage_vita_phase_profile_reset();
    s_halo_reads = 0u;
    for (unsigned unavailable=0u; unavailable<2u; ++unavailable) {
        kage_vita_phase_profile_reset();
        s_halo_reads=0u;
        s_halo.abi_version = unavailable ? ISAAC_LASER_HALO_STATS_ABI : 1u;
        s_halo_unavailable=unavailable;
        kage_vita_phase_profile_note_loop_head(1u);
        for (uint32_t window = 1u; window <= 3u; ++window) {
            memset(s_logs, 0, sizeof s_logs);
            s_log_length = s_log_calls = 0u;
            if (window == 2u) {
                s_halo.abi_version = ISAAC_LASER_HALO_STATS_ABI;
                s_halo_unavailable=0u;
            }
            run_variable_window((window - 1u) * 120u + 1u);
            CHECK(s_halo_reads == window + 1u);
            const char *row = log_line(ORACLE_RECORDS_PER_WINDOW - ORACLE_ROOM_LOG_RECORDS - 1u);
            CHECK(strstr(row, window < 3u ? " abi=0 " : " abi=2 ") != NULL);
            CHECK((strstr(row, "attempt=") != NULL) == (window == 3u));
        }
    }
    /* Disabled census is UNKNOWN, but the old halo population stays valid.
     * A 0->1 transition needs one white baseline; then modular deltas resume. */
    kage_vita_phase_profile_reset(); s_halo_reads=0u; s_halo.white_enabled=0u;
    kage_vita_phase_profile_note_loop_head(1u);
    for (uint32_t window=1u;window<=4u;++window) {
        memset(s_logs,0,sizeof s_logs); s_log_length=s_log_calls=0u;
        if (window==2u) s_halo.white_enabled=1u;
        if (window==4u) s_halo.white_enabled=0u;
        run_variable_window((window-1u)*120u+1u);
        CHECK(s_halo_reads==window+1u);
        const char *row=log_line(ORACLE_RECORDS_PER_WINDOW-ORACLE_ROOM_LOG_RECORDS-1u);
        CHECK(strstr(row," abi=2 ") && strstr(row,"attempt="));
        CHECK(strstr(row,window==3u ? " white_on=1" : " white_on=0"));
        CHECK((strstr(row,"ordinary(p,w)=")!=NULL)==(window==3u));
    }
#endif
    memset(s_logs, 0, sizeof s_logs);
    s_log_length = 0u;
    s_log_calls = 0u;
    kage_vita_phase_profile_oracle_emit_max_records();
    /* The max flush omits ph120.e (bounded by construction, see
     * ORACLE_FBO_POLICY_RECORDS). */
    CHECK(s_log_calls ==
          ORACLE_RECORDS_PER_WINDOW - ORACLE_FBO_POLICY_RECORDS);
    max_timing_length = log_line_length(log_line(0u));
    max_counts_length = log_line_length(log_line(1u));
    max_location_length = log_line_length(log_line(2u));
#if defined(ISAAC_VITA_GL_TIME_PROFILE)
    max_gl_time_length = log_line_length(
        log_line(2u + ORACLE_LOCATION_RECORDS));
# if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE) && ISAAC_VITA_GL_TIME_SDK_SPARSE
    max_gl_canonical_length = log_line_length(
        log_line(3u + ORACLE_LOCATION_RECORDS));
    max_gl_clear_length = log_line_length(
        log_line(4u + ORACLE_LOCATION_RECORDS));
    max_gl_queue_length = log_line_length(
        log_line(5u + ORACLE_LOCATION_RECORDS));
#  if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE_ATTRIB)
    max_gl_attrib_length = log_line_length(
        log_line(6u + ORACLE_LOCATION_RECORDS));
#  endif
# endif
# if defined(ISAAC_VITA_GL_WRAPPER_TIME)
    max_gl_wrapper_length = log_line_length(
        log_line(2u + ORACLE_GL_SCOPE_RECORDS + ORACLE_LOCATION_RECORDS));
    max_gl_draw_split_length = log_line_length(
        log_line(3u + ORACLE_GL_SCOPE_RECORDS + ORACLE_LOCATION_RECORDS));
    max_gl_uniform_split_length = log_line_length(
        log_line(4u + ORACLE_GL_SCOPE_RECORDS + ORACLE_LOCATION_RECORDS));
# endif
    max_gl_scene_length = log_line_length(
        log_line(2u + ORACLE_GL_SCOPE_RECORDS + ORACLE_GL_WRAPPER_RECORDS +
                 ORACLE_LOCATION_RECORDS));
# if defined(ISAAC_VITA_STOCK_FBO_VALID_REGION)
    max_valid_region_length = log_line_length(
        log_line(3u + ORACLE_GL_SCOPE_RECORDS + ORACLE_GL_WRAPPER_RECORDS +
                 ORACLE_LOCATION_RECORDS));
# endif
#endif
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    max_texture_churn_length = log_line_length(
        log_line(2u + ORACLE_LOCATION_RECORDS + ORACLE_GL_TIME_RECORDS));
    max_texture_delete_length = log_line_length(
        log_line(3u + ORACLE_LOCATION_RECORDS + ORACLE_GL_TIME_RECORDS));
#endif
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
    max_guest_cache_length = log_line_length(
        log_line(2u + ORACLE_LOCATION_RECORDS + ORACLE_GL_TIME_RECORDS +
                 ORACLE_TEXTURE_CHURN_RECORDS));
#endif
    next_line = 2u + ORACLE_LOCATION_RECORDS + ORACLE_GL_TIME_RECORDS +
        ORACLE_TEXTURE_CHURN_RECORDS + ORACLE_GUEST_CACHE_RECORDS;
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
    max_dispatch_length = log_line_length(log_line(next_line++));
    max_imports_length = log_line_length(log_line(next_line++));
#endif
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    max_gl_redundancy_length = log_line_length(log_line(next_line++));
#endif
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    max_gl_typed_state_length = log_line_length(log_line(next_line++));
#endif
#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
    max_vitagl_length = log_line_length(log_line(next_line++));
    max_submission_length = log_line_length(log_line(next_line++));
    max_fusion_length = log_line_length(log_line(next_line++));
#endif
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    max_fill_totals_length = log_line_length(log_line(next_line++));
    max_fill_passes_length = log_line_length(log_line(next_line++));
#endif
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
    max_coloroffset_length = log_line_length(log_line(next_line++));
#endif
#if defined(ISAAC_VITA_COLOROFFSET_FS_PROBE)
    max_fs_probe_length = log_line_length(log_line(next_line++));
#endif
#if defined(ISAAC_VITA_PILL_BLOOM_BYPASS)
    max_pill_bloom_length = log_line_length(log_line(next_line++));
#endif
#if defined(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES)
    max_bloom_half_length = log_line_length(log_line(next_line++));
#endif
#if defined(ISAAC_VITA_POOP_FX_PROFILE)
    max_poop_fx_length = log_line_length(log_line(next_line++));
#endif
#if defined(ISAAC_VITA_LASER_PROFILE)
    max_laser_length = log_line_length(log_line(next_line++));
    max_laser_cost_length = log_line_length(log_line(next_line++));
    max_laser_gpu_length = log_line_length(log_line(next_line++));
#endif
#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
    max_scheduler_length = log_line_length(log_line(next_line++));
    max_scheduler_pointer_length = log_line_length(log_line(next_line++));
    max_emission_length = log_line_length(log_line(next_line++));
#endif
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
    max_import_kinds_length = log_line_length(log_line(next_line++));
    max_import_hot_length = log_line_length(log_line(next_line++));
#endif
#if defined(ISAAC_VITA_HEAP_CENSUS)
    max_heap_length = log_line_length(log_line(next_line++));
#endif
#if defined(ISAAC_VITA_KAGE_QUAD_FASTPATH)
    max_kage_quad_length = log_line_length(log_line(next_line++));
#endif
#if defined(ISAAC_VITA_PHASE_PROFILE_OTHER)
    /* oth= adds 37 bytes at ten digits each; ph120.s already uses the
     * 512-byte allowance (503). */
    CHECK(max_timing_length == 470u && max_timing_length <= 512u);
#else
    CHECK(max_timing_length == 433u && max_timing_length <= 512u);
#endif
    CHECK(max_counts_length == 375u && max_counts_length < 384u);
    CHECK(max_location_length == 161u && max_location_length < 384u);
#if defined(ISAAC_VITA_GL_TIME_PROFILE)
# if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE) && ISAAC_VITA_GL_TIME_SDK_SPARSE
    CHECK(max_gl_time_length > 0u && max_gl_time_length <= 512u);
    CHECK(max_gl_canonical_length > 0u && max_gl_canonical_length <= 512u);
    CHECK(max_gl_clear_length > 0u && max_gl_clear_length <= 512u);
    CHECK(max_gl_queue_length > 0u && max_gl_queue_length <= 512u);
#  if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE_ATTRIB)
    /* All-ones scope=attrib row: 273 bytes, within the 384-byte log line. */
    CHECK(max_gl_attrib_length == 273u && max_gl_attrib_length < 384u);
#  endif
# else
    CHECK(max_gl_time_length == 372u && max_gl_time_length < 384u);
# endif
# if defined(ISAAC_VITA_GL_WRAPPER_TIME)
    /* Nine wrapper us + nine counts + two (us,n) slots + bad, all ones, with
     * the 32-character build id: inside the 384-byte isaac_vita_log body. */
    CHECK(max_gl_wrapper_length == 379u && max_gl_wrapper_length < 384u);
    /* Seven draw-split us + four counts + two (own,loc,gl) triples + bad;
     * five (us,n) uniform kinds + bad. */
    CHECK(max_gl_draw_split_length == 370u &&
          max_gl_draw_split_length < 384u);
    CHECK(max_gl_uniform_split_length == 232u &&
          max_gl_uniform_split_length < 384u);
# endif
# if defined(ISAAC_VITA_STOCK_FBO_VALID_REGION)
    /* vr(s,p,u,a,e,v,c) adds 95 bytes at ten digits each; the vz census
     * record (o + four TWxTH/RWxRH/N tuples) is its own line. */
    CHECK(max_gl_scene_length == 348u && max_gl_scene_length < 384u);
    CHECK(max_valid_region_length == 326u &&
          max_valid_region_length < 384u);
# else
    CHECK(max_gl_scene_length == 253u && max_gl_scene_length < 384u);
# endif
#endif
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    CHECK(max_texture_churn_length == 375u &&
          max_texture_churn_length < 384u);
    CHECK(max_texture_delete_length == 208u &&
          max_texture_delete_length < 384u);
#endif
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
    CHECK(max_guest_cache_length == 119u &&
          max_guest_cache_length < 384u);
#endif
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
    CHECK(max_dispatch_length == ORACLE_MAX_DISPATCH_LENGTH &&
          max_dispatch_length < 384u);
    CHECK(max_imports_length == ORACLE_MAX_IMPORTS_LENGTH &&
          max_imports_length < 384u);
#endif
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    CHECK(max_gl_redundancy_length > 0u &&
          max_gl_redundancy_length < 384u);
#endif
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    CHECK(max_gl_typed_state_length > 0u &&
          max_gl_typed_state_length < 384u);
#endif
#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
    CHECK(max_vitagl_length > 0u && max_vitagl_length < 384u);
    CHECK(max_submission_length > 0u && max_submission_length < 384u);
    CHECK(max_fusion_length > 0u && max_fusion_length < 384u);
#endif
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    /* All-ones worst case of both census records: inside the 384-byte
     * isaac_vita_log body. */
    CHECK(max_fill_totals_length == 381u && max_fill_totals_length < 384u);
    CHECK(max_fill_passes_length == 381u && max_fill_passes_length < 384u);
#endif
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
    CHECK(max_coloroffset_length == 383u && max_coloroffset_length < 384u);
#endif
#if defined(ISAAC_VITA_COLOROFFSET_FS_PROBE)
    /* 22 all-ones counters (two as %08x) plus the 32-character build id. */
#if defined(ISAAC_VITA_COLOROFFSET_PLAIN_VERTEX_PAIR_ORACLE) && \
    defined(ISAAC_VITA_COLOROFFSET_PLAIN_FP16_ORACLE)
    /* Both are forbidden in production, but bound the full formatter. */
    CHECK(max_fs_probe_length == 501u && max_fs_probe_length < 512u);
#elif defined(ISAAC_VITA_COLOROFFSET_PLAIN_FP16_ORACLE)
    CHECK(max_fs_probe_length == 456u && max_fs_probe_length < 512u);
#elif defined(ISAAC_VITA_COLOROFFSET_PLAIN_VERTEX_PAIR_ORACLE)
    CHECK(max_fs_probe_length == 469u && max_fs_probe_length < 512u);
#else
    CHECK(max_fs_probe_length == 424u && max_fs_probe_length < 512u);
#endif
#endif
#if defined(ISAAC_VITA_PILL_BLOOM_BYPASS)
    CHECK(max_pill_bloom_length == 133u && max_pill_bloom_length < 384u);
#endif
#if defined(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES)
    CHECK(max_bloom_half_length == 180u && max_bloom_half_length < 384u);
#endif
#if defined(ISAAC_VITA_POOP_FX_PROFILE)
    CHECK(max_poop_fx_length == 281u && max_poop_fx_length < 384u);
#endif
#if defined(ISAAC_VITA_LASER_PROFILE)
    CHECK(max_laser_length == 293u && max_laser_length < 384u);
    CHECK(max_laser_cost_length == 293u && max_laser_cost_length < 384u);
    CHECK(max_laser_gpu_length == 269u && max_laser_gpu_length < 384u);
#endif
#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
    CHECK(max_scheduler_length > 0u && max_scheduler_length <= 512u);
    CHECK(max_scheduler_pointer_length == 276u &&
          max_scheduler_pointer_length < 384u);
    CHECK(max_emission_length == 124u && max_emission_length < 384u);
#endif
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
    CHECK(max_import_kinds_length == 332u && max_import_kinds_length < 384u);
    CHECK(max_import_hot_length == 305u && max_import_hot_length < 384u);
#endif
#if defined(ISAAC_VITA_HEAP_CENSUS)
    CHECK(max_heap_length == ORACLE_MAX_HEAP_LENGTH &&
          max_heap_length <= 512u);
#endif
#if defined(ISAAC_VITA_KAGE_QUAD_FASTPATH)
    CHECK(max_kage_quad_length == ORACLE_MAX_KAGE_QUAD_LENGTH &&
          max_kage_quad_length < 384u);
#endif
#if defined(ISAAC_VITA_LASER_HALO_PROFILE) && ISAAC_VITA_LASER_HALO_PROFILE
    CHECK(log_line_length(log_line(next_line)) == 341u && log_line_length(log_line(next_line)) <= 512u);
    CHECK(strstr(log_line(next_line), "white_on=1 ordinary(p,w)=4294967295,4294967295") != NULL);
    CHECK(strstr(log_line(next_line++), "attempt=4294967295") != NULL);
#endif
#if defined(ISAAC_VITA_ROOM_LOG_MARKERS) && ISAAC_VITA_ROOM_LOG_MARKERS
    CHECK(strstr(log_line(next_line), "epoch=4294967295 n=0 seq=4294967295 drop=4294967295 reset_lost=4294967295 sat=15") != NULL);
    CHECK(log_line_length(log_line(next_line++)) <= 512u);
#endif
    CHECK(next_line ==
          ORACLE_RECORDS_PER_WINDOW - ORACLE_FBO_POLICY_RECORDS);
    CHECK(log_line(ORACLE_RECORDS_PER_WINDOW - ORACLE_FBO_POLICY_RECORDS)
              != NULL &&
          *log_line(ORACLE_RECORDS_PER_WINDOW - ORACLE_FBO_POLICY_RECORDS)
              == '\0');

#if defined(ISAAC_VITA_ROOM_LOG_MARKERS) && ISAAC_VITA_ROOM_LOG_MARKERS
    {
        unsigned i, clocks, threads;
        kage_vita_phase_profile_oracle_room_seed(0u);
        s_room_thread = 7;
        errno = EACCES;
        threads = s_room_thread_reads;
        clocks = s_clock_reads;
        kage_vita_phase_profile_reset();
        CHECK(errno == EACCES && s_room_thread_reads == threads + 1u &&
              s_clock_reads == clocks);
        memset(s_logs, 0, sizeof s_logs); s_log_length = s_log_calls = 0u;
        s_now = UINT64_C(0x123456789abcdef0);
        kage_vita_phase_profile_room_log(UINT32_C(0x80000000), UINT32_MAX);
        CHECK(errno == EACCES && s_clock_reads == clocks + 1u && s_log_calls == 0u);
        s_room_thread = 8;
        kage_vita_phase_profile_room_log(99u, 99u);
        CHECK(errno == EACCES && s_clock_reads == clocks + 1u && s_log_calls == 0u);
        s_room_thread = 7;
        threads = s_room_thread_reads;
        kage_vita_phase_profile_note_loop_head(100u);
        CHECK(errno == EACCES && s_room_thread_reads == threads);
        clocks = s_clock_reads;
        for (i = 0u; i < 9u; ++i)
            kage_vita_phase_profile_room_log(1u, i + 1u);
        CHECK(s_clock_reads == clocks + 9u && s_log_calls == 0u);
        errno = EACCES;
        for (i = 101u; i <= 220u; ++i) {
            s_now += 100u;
            kage_vita_phase_profile_note_loop_head(i);
        }
        CHECK(errno == EACCES);
        CHECK(s_log_calls == ORACLE_RECORDS_PER_WINDOW + 8u);
        CHECK(strstr(s_logs, "epoch=1 n=8 seq=10 drop=2 reset_lost=0 sat=0\n") != NULL);
        CHECK(strstr(s_logs, "epoch=1 seq=1 at=12345678:9abcdef0 assigned=0 win=0 loop=0 pair=-2147483648,-1 sat=0\n") != NULL);
        CHECK(strstr(s_logs, "epoch=1 seq=2 at=12345678:9abcdef0 assigned=1 win=1 loop=100 pair=1,1 sat=0\n") != NULL);
        CHECK(strstr(s_logs, "epoch=1 seq=9 ") == NULL);
        for (i = 0u; i < s_log_calls; ++i) CHECK(log_line_length(log_line(i)) <= 512u);
        memset(s_logs, 0, sizeof s_logs); s_log_length = s_log_calls = 0u;
        kage_vita_phase_profile_room_log(2u, 2u); /* pending tail lost by reset */
        kage_vita_phase_profile_reset();
        kage_vita_phase_profile_room_log(3u, 3u); /* pre-loop in new epoch */
        for (i = 1u; i <= 121u; ++i) {
            s_now += 100u;
            kage_vita_phase_profile_note_loop_head(i);
        }
        CHECK(strstr(s_logs, "epoch=2 n=1 seq=1 drop=2 reset_lost=1 sat=0\n") != NULL);
        CHECK(strstr(s_logs, "assigned=0 win=0 loop=0 pair=3,3 sat=0\n") != NULL);
        CHECK(strstr(s_logs, "pair=2,2") == NULL);
        clocks = s_clock_reads;
        s_room_thread = -1; kage_vita_phase_profile_reset();
        kage_vita_phase_profile_room_log(4u, 4u);
        CHECK(s_clock_reads == clocks);
        s_room_thread = 7;
        printf("Room log phase cases: PASS (owner UID, clocks, pre-loop, queue8/drop, epoch/reset loss, signed pair, saturation, bounded rows)\n");
    }
#endif
    printf("Vita phase profile host oracle: PASS "
           "(records=%u; max t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r=%zu/%zu/%zu",
           (unsigned)ORACLE_RECORDS_PER_WINDOW,
           max_timing_length, max_counts_length, max_location_length);
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
    printf("/%zu", max_guest_cache_length);
#else
    printf("/-");
#endif
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    printf("/%zu", max_gl_redundancy_length);
#else
    printf("/-");
#endif
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    printf("/%zu", max_gl_typed_state_length);
#else
    printf("/-");
#endif
#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
    printf("/%zu/%zu/%zu", max_vitagl_length,
           max_submission_length, max_fusion_length);
#else
    printf("/-/-/-");
#endif
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
    printf("/%zu", max_coloroffset_length);
#else
    printf("/-");
#endif
#if defined(ISAAC_VITA_PILL_BLOOM_BYPASS)
    printf("/%zu", max_pill_bloom_length);
#else
    printf("/-");
#endif
#if defined(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES)
    printf("/%zu", max_bloom_half_length);
#else
    printf("/-");
#endif
#if defined(ISAAC_VITA_POOP_FX_PROFILE)
    printf("/%zu", max_poop_fx_length);
#else
    printf("/-");
#endif
#if defined(ISAAC_VITA_LASER_PROFILE)
    printf("/%zu/%zu/%zu", max_laser_length,
           max_laser_cost_length, max_laser_gpu_length);
#else
    printf("/-/-/-");
#endif
#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
    printf("/%zu/%zu", max_scheduler_length,
           max_scheduler_pointer_length);
#else
    printf("/-/-");
#endif
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    printf("; x=%zu/xd=%zu", max_texture_churn_length,
           max_texture_delete_length);
#endif
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    /* Suffix only under the census define: every other leg's exact PASS
     * string in test_kage_vita_phase_profile.py stays frozen. */
    printf("; fa=%zu/fp=%zu", max_fill_totals_length,
           max_fill_passes_length);
#endif
#if defined(ISAAC_VITA_GL_WRAPPER_TIME)
    /* Suffix only under the wrapper-time define (same rule as fa/fp). */
    printf("; gw=%zu/gd=%zu/gu=%zu", max_gl_wrapper_length,
           max_gl_draw_split_length, max_gl_uniform_split_length);
#endif
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
    printf("; d=%zu/i=%zu", max_dispatch_length, max_imports_length);
#endif
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
    printf("; ik/ih=%zu/%zu", max_import_kinds_length,
           max_import_hot_length);
#endif
#if defined(ISAAC_VITA_COLOROFFSET_FS_PROBE)
    /* Suffix only under the probe define (same rule as mem below). */
    printf("; kp=%zu", max_fs_probe_length);
#endif
#if defined(ISAAC_VITA_HEAP_CENSUS)
    /* Suffix only under the census define: every other leg's exact PASS
     * string in test_kage_vita_phase_profile.py stays frozen. */
    printf("; mem=%zu", max_heap_length);
#endif
#if defined(ISAAC_VITA_KAGE_QUAD_FASTPATH)
    /* Suffix only under the fast-path define (same rule as mem above). */
    printf("; kq=%zu", max_kage_quad_length);
#endif
#if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE) && ISAAC_VITA_GL_TIME_SDK_SPARSE
    printf("; sdk32=%zu/%zu/%zu/%zu", max_gl_time_length,
           max_gl_canonical_length, max_gl_clear_length, max_gl_queue_length);
# if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE_ATTRIB)
    printf("; attrib=%zu", max_gl_attrib_length);
# endif
#endif
    printf("; limiter skips; log cost excluded)\n");
    return 0;
}
