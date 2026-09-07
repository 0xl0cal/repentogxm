/* Low-overhead aggregate timings for the physical Vita game loop.
 *
 * All durations are process-clock microseconds.  Render is inclusive of the
 * nested swap; the separately reported swap phase is therefore a measured
 * subset, not an additional component to add to Render.  The limiter phase is
 * the frozen software-pacer region 0048be3e..0048bf0b.  Whole is the interval
 * between consecutive outer-loop heads.  gap measures consecutive valid CPU
 * Present returns, not scanout/vblank; unlike Whole it includes report cost
 * and skipped-render loops.  It reuses the existing Present-return clock read.
 * Each 120-loop window emits the two
 * bounded baseline records plus the ph120.a location/timeGetTime census;
 * the opt-in guest cache, dispatch table, GL redundancy cache, vitaGL draw
 * policy, fixed-step scheduler, import-kinds census and heap receipt append
 * bounded records in deterministic
 * t,c,a,gt,gx,x,xd,g,d,i,o,y,v,q,f,fa,fp,e,k,kp,b,h,p,l,m,n,s,r,w,ik,ih,mem,ha,rl order
 * (kage_vita_phase_profile.h). */
#include "kage_vita_phase_profile.h"
#include "kage_vita_deep_profile.h"
#include "kage_vita_fullspeed_scheduler.h"
#include "kage_vita_guest_sampler.h"
#include "guest.h"
#include "gl_vita_backend.h"
#if defined(ISAAC_VITA_FRAME_SPIKE_PROFILE)
# include "kage_vita_frame_spike_profile.h"
static KageVitaFrameSpikeWindow s_spikes;
static IsaacVitaGlPhaseProfileCounters s_spike_gl;
static uint32_t s_spike_bad_start, s_spike_clamp_start;
#endif
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE) || \
    defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
/* Readers of guest.h's per-import census array: the dispatch table's ph120.i
 * (top IDs, binding kinds) and the import-kinds ph120.ik/ph120.ih (per-kind
 * totals, fourteen named hot imports, the g(c) identity gate).  Either option
 * compiles guest.c with the array; this owner sees the same define. */
# define KAGE_VITA_PROFILE_IMPORT_CENSUS 1
# include "host_vita_import_id.h"
#endif
#include "host_vita_post_com.h"
#if defined(ISAAC_VITA_KAGE_QUAD_FASTPATH)
/* ph120.kq: the Image::PushQuad host replay's per-window verdict counters
 * (host_vita_kage_quad_fastpath.c owns the take-and-zero). */
# include "host_vita_kage_quad_fastpath.h"
#endif
#if defined(ISAAC_VITA_HEAP_CENSUS)
/* ph120.mem: the heap-lock half comes from host_vita_heap.c in one lock
 * section; mallinfo, the Lua arena and vitaGL are read here, outside it. */
# include "host_vita_heap.h"
# if !defined(ISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE)
#  include <malloc.h>
#  include <vitaGL.h>
# endif
# if defined(ISAAC_VITA_HEAP_CENSUS_LUA)
/* host_vita_lua.c (prototype in host_vita_lua_gc.h, which needs the Lua
 * include path this owner does not have). */
void isaac_vita_lua_arena_stats(uint32_t *live_kb, uint32_t *peak_kb,
                                uint32_t *fallbacks);
# endif
#endif

#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
# if defined(ISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE)
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
void vglGetIsaacGpuDrawStats(vglIsaacGpuDrawStats *stats);
#  if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
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
void vglGetIsaacColorOffsetStats(vglIsaacColorOffsetStats *stats);
#  endif
# else
#  include <vitaGL.h>
# endif
_Static_assert(sizeof(vglIsaacGpuDrawStats) == 31u * sizeof(uint32_t),
               "vitaGL GPU draw statistics ABI drifted");
#endif
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
# if !defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
#  error ColorOffset statistics require the exact vitaGL draw bundle
# endif
_Static_assert(sizeof(vglIsaacColorOffsetStats) == 24u * sizeof(uint32_t),
               "vitaGL ColorOffset statistics ABI drifted");
#endif
#if defined(ISAAC_VITA_COLOROFFSET_FS_PROBE)
# if !defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
#  error ColorOffset FS probe statistics require the exact ColorOffset bundle
# endif
/* ph120.kp (DIAGNOSTIC builds): the 0008 fragment-shader probe.  Struct and
 * endpoint are consumer-declared in gl_vita_coloroffset_fs_probe.h; vitaGL.h
 * is untouched. */
# include "gl_vita_coloroffset_fs_probe.h"
# include <stdio.h>
#endif

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#if defined(ISAAC_VITA_ROOM_LOG_MARKERS) && ISAAC_VITA_ROOM_LOG_MARKERS
# include <errno.h>
#endif

#if defined(ISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE)
int sceClibPrintf(const char *format, ...);
uint64_t sceKernelGetProcessTimeWide(void);
# if defined(ISAAC_VITA_ROOM_LOG_MARKERS) && ISAAC_VITA_ROOM_LOG_MARKERS
int sceKernelGetThreadId(void);
# endif
#else
# include <psp2/kernel/clib.h>
# include <psp2/kernel/processmgr.h>
# if defined(ISAAC_VITA_ROOM_LOG_MARKERS) && ISAAC_VITA_ROOM_LOG_MARKERS
#  include <psp2/kernel/threadmgr.h>
# endif
#endif

/* Every record below is one KVPP_PRINTF call with one newline.  OFF: the
 * kernel debug printf itself (sceClibPrintf; with Cat-A-Log capturing it
 * costs 3-4 ms per call on the game thread, 60-100 ms per window).
 * ISAAC_VITA_LOG_ASYNC: format into a stack buffer and hand the bytes to
 * the logger thread (host_vita_log_async.c); record bytes and order are
 * unchanged. */
#if defined(ISAAC_VITA_LOG_ASYNC)
# include "host_vita_log_async.h"
# define KVPP_PRINTF isaac_vita_log_async_printf
#else
# define KVPP_PRINTF sceClibPrintf
#endif
#if defined(ISAAC_VITA_LASER_HALO_PROFILE) && ISAAC_VITA_LASER_HALO_PROFILE
# include "../vita/vitagl-stock-reference/isaac_laser_halo_profile.h"
# include <stdio.h>
static vglIsaacLaserHaloStats s_window_halo;
static uint32_t s_window_halo_valid, s_window_halo_from;

static void kage_vita_profile_log_halo(const char *bid, uint32_t window,
        uint32_t loops, uint32_t from, const vglIsaacLaserHaloStats *delta)
{
    char white[sizeof " ordinary(p,w)=4294967295,4294967295"] = "";
    if (!delta) {
        /* No healthy zero-valued population when an archive ABI or baseline
         * is unavailable. A following valid snapshot only rebaselines. */
        KVPP_PRINTF("[kage-vita] ph120.ha bid=%.32s win=%u loops=%u abi=0 from=%u\n",
            bid, (unsigned)window, (unsigned)loops, (unsigned)from);
        return;
    }
    if (delta->white_enabled)
        snprintf(white, sizeof white, " ordinary(p,w)=%u,%u",
            (unsigned)delta->ordinary_plain_requests, (unsigned)delta->ordinary_white_requests);
    KVPP_PRINTF("[kage-vita] ph120.ha bid=%.32s win=%u loops=%u abi=2 from=%u "
        "attempt=%u reject(meta,limits,state,plain,uv,output)=%u,%u,%u,%u,%u,%u "
        "staged(light,cap)=%u,%u big=%u white_on=%u%s\n",
        bid, (unsigned)window, (unsigned)loops, (unsigned)from,
        (unsigned)delta->attempts,
        (unsigned)delta->outcome[ISAAC_HALO_META],
        (unsigned)delta->outcome[ISAAC_HALO_LIMITS],
        (unsigned)delta->outcome[ISAAC_HALO_STATE],
        (unsigned)delta->outcome[ISAAC_HALO_PLAIN],
        (unsigned)delta->outcome[ISAAC_HALO_UV],
        (unsigned)delta->outcome[ISAAC_HALO_OUTPUT],
        (unsigned)delta->outcome[ISAAC_HALO_LIGHT],
        (unsigned)delta->outcome[ISAAC_HALO_CAP],
        (unsigned)delta->too_big, (unsigned)delta->white_enabled, white);
}

static void kage_vita_profile_report_halo(const char *bid, uint32_t window, uint32_t loops)
{
    vglIsaacLaserHaloStats current = {0}, delta;
    uint32_t valid = vglGetIsaacLaserHaloStats(&current, ISAAC_LASER_HALO_STATS_WORDS)
        && current.abi_version == ISAAC_LASER_HALO_STATS_ABI && current.white_enabled <= 1u;
    uint32_t i;
    if (valid && s_window_halo_valid) {
        delta.abi_version = ISAAC_LASER_HALO_STATS_ABI;
        delta.attempts = current.attempts - s_window_halo.attempts;
        for (i = 0u; i < ISAAC_HALO_OUTCOMES; ++i)
            delta.outcome[i] = current.outcome[i] - s_window_halo.outcome[i];
        delta.too_big = current.too_big - s_window_halo.too_big;
        /* The legacy population remains valid when the optional census is
         * disabled/changes. White needs a continuously enabled interval. */
        delta.white_enabled = current.white_enabled && s_window_halo.white_enabled;
        delta.ordinary_plain_requests = current.ordinary_plain_requests - s_window_halo.ordinary_plain_requests;
        delta.ordinary_white_requests = current.ordinary_white_requests - s_window_halo.ordinary_white_requests;
    }
    kage_vita_profile_log_halo(bid, window, loops,
        s_window_halo_from, valid && s_window_halo_valid ? &delta : NULL);
    s_window_halo = current;
    s_window_halo_valid = valid;
    s_window_halo_from = window;
}
#endif
#if defined(ISAAC_VITA_NATIVE_RESOURCE_PROFILE) || \
    defined(ISAAC_VITA_IO_WINDOW_PROFILE) || defined(ISAAC_VITA_STATIC_SFX_PROFILE) || \
    defined(ISAAC_VITA_PNG_WINDOW_PROFILE) || defined(ISAAC_VITA_CRT_FILE_LOOKUP_HINT) || \
    defined(ISAAC_VITA_ANM2_WINDOW_PROFILE) || defined(ISAAC_VITA_PNG_TEXEL_INIT_ELISION) || \
    defined(ISAAC_VITA_STOCK_FBO_RT_INTERLUDE_REUSE)
# include "kage_vita_room_profile.h"
# define KVPP_ROOM_DISCARD() kage_vita_room_profile_discard()
# define KVPP_ROOM_REPORT(b, w, l) kage_vita_room_profile_report(b, w, l)
#else
# define KVPP_ROOM_DISCARD() ((void)0)
# define KVPP_ROOM_REPORT(b, w, l) ((void)0)
#endif

#if defined(ISAAC_VITA_IMAGE_RETAIN)
# include "host_vita_image_retain.h"
#endif

#ifndef ISAAC_VITA_PHASE_PROFILE_BUILD_ID
# define ISAAC_VITA_PHASE_PROFILE_BUILD_ID "phase:unstamped"
#endif

#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
extern uint32_t g_guest_lookup_cache_hits;
extern uint32_t g_guest_lookup_cache_misses;
#endif
#if defined(ISAAC_VITA_GL_TIME_PROFILE)
# if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE) && ISAAC_VITA_GL_TIME_SDK_SPARSE
# include "../vita/vitagl-stock-reference/isaac_sdk_sparse_profile.h"
# endif
/* Provided by the 0005 scene-timer and 0006 scene-split patches of the pinned
 * stock vitaGL (gxm.c), deliberately not declared in vitaGL.h so the
 * shader-cache whole-file hash of that header stays valid.  Both take and
 * zero their sums.  vglIsaacSceneTimes: process-clock microseconds inside
 * sceGxmBeginScene and sceGxmEndScene, scenes begun, longest BeginScene.
 * Measured location of the wait on the device: inside sceGxmEndScene, not
 * BeginScene (prof-v12 ph120.gt: c == e, bm <= 0.1 ms; BeginScene is
 * bookkeeping).  Hypothesised cause, unconfirmed until the
 * ISAAC_VITA_STOCK_FBO_RT_SCENES=8 vs 1 A/B: a render target created with
 * scenesPerFrame = 1 has one scene slot, so ending scene k on it would wait
 * until scene k-1 on the same target has finished on the GPU (the ordinal
 * split below is what confirms or refutes it: under that model the 2nd and
 * 3rd offscreen ends carry the wait).  vglIsaacSceneWaits splits that
 * EndScene sum by the framebuffer whose scene ended (offscreen FBO vs
 * display) and by the ordinal of the offscreen end since the last
 * vglSwapBuffers (1st, 2nd, 3rd, 4th and later), gives the longest EndScene
 * and how many exceeded 500 us (display ends included), and counts every
 * offscreen setup_render_target attempt (a refused N-slot target plus its
 * 1-slot retry adds 2), sceGxmCreateRenderTarget failures (any target,
 * display included; silent under NO_DEBUG otherwise) and offscreen
 * depth/stencil buffers created. */
void vglIsaacSceneTimes(
    uint32_t *begin_us, uint32_t *end_us, uint32_t *count,
    uint32_t *begin_max_us);
void vglIsaacSceneWaits(
    uint32_t *fbo_us, uint32_t *display_us, uint32_t *max_us,
    uint32_t *slow, uint32_t *ordinal_us /* [4] */, uint32_t *rt_created,
    uint32_t *rt_failed, uint32_t *depth_created);
# if defined(ISAAC_VITA_STOCK_FBO_VALID_REGION)
/* Provided by the 0009 valid-region patch (gxm.c), take-and-zero, taken with
 * the two hooks above at every take.  counters[8]: s = FBO scenes begun, p =
 * scenes whose logical rect (the guest's last glViewport, recorded with the
 * write framebuffer, its attachment and the attachment size) was known and
 * smaller than the attachment, u = rect unknown, unanchored or oversized
 * (NULL region, fail closed), a = scenes begun with the region (apply mode
 * only), e = region refused by sceGxmBeginScene and begun again with NULL,
 * v = guest glViewport left the rect derived for the open FBO scene (both
 * modes; in apply mode applying stops for the run), c = tile-clipper region
 * clips clamped to the region, o = census overflow (a fifth distinct size
 * tuple).  census[20]: four (texture w, texture h, rect w, rect h, scenes
 * this window) tuples in first-seen order; the tuples persist across takes,
 * the counts are zeroed.  A proven route reads u == 0, e == 0, v == 0 (in
 * observe mode first: apply mode would clip) and, in apply mode, a == p. */
void vglIsaacFboValidRegionStats(uint32_t *counters, uint32_t *census);
# endif

typedef struct kage_vita_scene_times {
    uint32_t count;
    uint32_t begin_us;
    uint32_t end_us;
    uint32_t begin_max_us;
    /* 0006 split of end_us (ph120.gx). */
    uint32_t end_fbo_us;
    uint32_t end_display_us;
    uint32_t end_max_us;
    uint32_t end_slow;
    uint32_t end_ordinal_us[4];
    uint32_t rt_created;
    uint32_t rt_failed;
    uint32_t depth_created;
# if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE) && ISAAC_VITA_GL_TIME_SDK_SPARSE
    IsaacSdkSparseProfile sparse;
#  if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE_ATTRIB)
    /* Shader attribute replay timer (ph120.gt scope=attrib). */
    IsaacVitaAttribReplaySparse attrib;
#  endif
# endif
# if defined(ISAAC_VITA_STOCK_FBO_VALID_REGION)
    /* 0009 valid-region counters (ph120.gx vr) and size census (ph120.vz). */
    uint32_t vr[8];
    uint32_t vr_census[20];
# endif
} kage_vita_scene_times;

static void kage_vita_profile_take_scene_times(kage_vita_scene_times *scene)
{
    vglIsaacSceneTimes(
        &scene->begin_us, &scene->end_us, &scene->count,
        &scene->begin_max_us);
    vglIsaacSceneWaits(
        &scene->end_fbo_us, &scene->end_display_us, &scene->end_max_us,
        &scene->end_slow, scene->end_ordinal_us, &scene->rt_created,
        &scene->rt_failed, &scene->depth_created);
# if defined(ISAAC_VITA_STOCK_FBO_VALID_REGION)
    vglIsaacFboValidRegionStats(scene->vr, scene->vr_census);
# endif
}
#elif defined(ISAAC_VITA_STOCK_FBO_VALID_REGION)
#error "ISAAC_VITA_STOCK_FBO_VALID_REGION requires ISAAC_VITA_GL_TIME_PROFILE"
#endif

enum kage_vita_profile_phase {
    KAGE_VITA_PROFILE_SERVICE = 0,
    KAGE_VITA_PROFILE_UPDATE,
    KAGE_VITA_PROFILE_RENDER,
    KAGE_VITA_PROFILE_PRESENT,
    KAGE_VITA_PROFILE_LIMITER,
    KAGE_VITA_PROFILE_WHOLE,
#if defined(ISAAC_VITA_PHASE_PROFILE_OTHER)
    KAGE_VITA_PROFILE_OTHER,
#endif
    KAGE_VITA_PROFILE_PRESENT_GAP,
    KAGE_VITA_PROFILE_PHASE_COUNT
};

typedef struct kage_vita_profile_samples {
    uint32_t values[KAGE_VITA_PHASE_PROFILE_WINDOW];
    uint32_t count;
    uint32_t dropped;
} kage_vita_profile_samples;

typedef struct kage_vita_profile_active {
    uint64_t started_at;
    uint32_t active;
} kage_vita_profile_active;

typedef struct kage_vita_profile_summary {
    uint32_t count;
    uint32_t minimum;
    uint32_t p50;
    uint32_t p95;
    uint32_t maximum;
} kage_vita_profile_summary;

static kage_vita_profile_samples s_samples[KAGE_VITA_PROFILE_PHASE_COUNT];
static kage_vita_profile_active s_service;
static kage_vita_profile_active s_update;
static kage_vita_profile_active s_render;
static kage_vita_profile_active s_present;
static uint64_t s_last_present_return;
static uint32_t s_have_present_return;
static kage_vita_profile_active s_limiter;
#if defined(ISAAC_VITA_PHASE_PROFILE_OTHER)
/* oth (ISAAC_VITA_PHASE_PROFILE_OTHER): loop time outside svc/upd/rnd/lim,
 * i.e. render return (or skip) -> limiter entry plus limiter exit -> the
 * next service entry, where the sample closes.  It spans the loop head, so
 * a window's emission block (ph120.w) lands in the first sample of the
 * following window. */
static uint64_t s_other_started_at;
static uint64_t s_other_accumulated;
static uint32_t s_other_active;
static uint32_t s_other_pending;
#endif
static uint64_t s_whole_started_at;
static uint32_t s_last_outer_loop;
static uint32_t s_completed_loops;
static uint32_t s_bad_sequence;
static uint32_t s_clamped_duration;
static GuestPhaseProfileCounters s_window_guest;
#if defined(KAGE_VITA_PROFILE_IMPORT_CENSUS)
/* Per-import census: window-start snapshot and the current window's deltas. */
static uint32_t s_window_import_calls[GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS];
static uint32_t s_import_calls_delta[GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS];
# define KAGE_VITA_PROFILE_SNAPSHOT_IMPORTS() \
    memcpy(s_window_import_calls, g_guest_phase_profile_import_calls, \
           sizeof s_window_import_calls)
#else
# define KAGE_VITA_PROFILE_SNAPSHOT_IMPORTS() ((void)0)
#endif
static uint32_t s_window_time_get_time;
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
static uint32_t s_window_lookup_cache_hits;
static uint32_t s_window_lookup_cache_misses;
#endif
static IsaacVitaGlPhaseProfileCounters s_window_gl;
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
static uint32_t s_window_attrib_pending;
#endif
#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
static vglIsaacGpuDrawStats s_window_vitagl;
#endif
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
static vglIsaacColorOffsetStats s_window_coloroffset;
#endif
#if defined(ISAAC_VITA_COLOROFFSET_FS_PROBE)
static vglIsaacColorOffsetFsProbeStats s_window_fs_probe;
#endif
#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
static KageVitaFullspeedSnapshot s_window_scheduler;
/* ph120.w emit(us,max): the previous window's synchronous emission block on
 * the game thread (window deltas, percentile sort, every ph120 record, the
 * window reset) and its process-lifetime maximum.  0 in the first window. */
static uint32_t s_last_emit_us;
static uint32_t s_max_emit_us;
#endif
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
/* Window baselines of the import-kinds records (ph120.ik/ph120.ih): the two
 * aggregate host counters host_vita_first_fault.c keeps and the opt-in
 * body-entry counter.  The per-ID census itself is the delta array above
 * (s_import_calls_delta), taken at the same loop head as g(c,l,i). */
static unsigned s_window_host_import_calls;
static unsigned s_window_host_dynamic_calls;
# if defined(ISAAC_VITA_PROFILE_FUNCTION_ENTRIES)
static uint32_t s_window_function_entries;
# endif

static void kage_vita_profile_import_kinds_baseline(void)
{
    s_window_host_import_calls = g_host_import_calls;
    s_window_host_dynamic_calls = g_host_dynamic_calls;
# if defined(ISAAC_VITA_PROFILE_FUNCTION_ENTRIES)
    s_window_function_entries = g_guest_function_entries;
# endif
}
# define KAGE_VITA_PROFILE_IMPORT_KINDS_BASELINE() \
    kage_vita_profile_import_kinds_baseline()
#else
# define KAGE_VITA_PROFILE_IMPORT_KINDS_BASELINE() ((void)0)
#endif
#if defined(ISAAC_VITA_PILL_BLOOM_BYPASS)
static uint32_t s_pill_bloom_capture_skips;
static uint32_t s_pill_bloom_composite_skips;
#endif
#if defined(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES)
static uint32_t s_bloom_half_create_initial;
static uint32_t s_bloom_half_create_applied;
static uint32_t s_bloom_half_create_rejected;
static uint32_t s_bloom_half_captures;
static uint32_t s_bloom_half_composites;
#endif
#if defined(ISAAC_VITA_POOP_FX_PROFILE)
typedef struct KageVitaPoopFxProfileStats {
    uint32_t add_pill;
    uint32_t add_black;
    uint32_t add_other;
    uint32_t add_unknown;
    uint32_t active;
    uint32_t clouds;
    uint32_t countdown_min;
    uint32_t countdown_max;
    uint32_t countdown_last;
    uint32_t cap_frames;
    uint32_t cap_skipped;
    uint32_t bad;
} KageVitaPoopFxProfileStats;

static KageVitaPoopFxProfileStats s_poop_fx;
#endif
#if defined(ISAAC_VITA_LASER_PROFILE)
enum KageVitaLaserCostClass {
    KAGE_VITA_LASER_COST_SAMPLE_ZERO = 0,
    KAGE_VITA_LASER_COST_SAMPLE_POSITIVE,
    KAGE_VITA_LASER_COST_SHADOW,
    KAGE_VITA_LASER_COST_CLASS_COUNT
};

typedef struct KageVitaLaserCostBucket {
    uint32_t completed;
    uint32_t total_us;
    uint32_t max_us;
    /* Immediate vitaGL submissions only.  KAGE work queued by this owner and
     * flushed later remains in the window-wide ph120.q counters; zeros here
     * must not be read as zero GPU fragment work. */
    uint32_t draw_requests;
    uint32_t index_count;
    uint32_t vertex_count;
} KageVitaLaserCostBucket;

typedef struct KageVitaLaserProfileStats {
    uint32_t render;
    uint32_t shadow;
    uint32_t sample_zero;
    uint32_t sample_positive;
    uint32_t source_1;
    uint32_t source_10_999;
    uint32_t source_other;
    uint32_t variant_0;
    uint32_t variant_1;
    uint32_t variant_2;
    uint32_t variant_other;
    uint32_t subtype_1_3;
    uint32_t subtype_other;
    KageVitaLaserCostBucket cost[KAGE_VITA_LASER_COST_CLASS_COUNT];
    uint32_t cost_other;
    uint32_t active_overflow;
    uint32_t end_mismatch;
    uint32_t dangling;
    uint32_t clock_reset;
    uint32_t submission_samples;
    uint32_t submission_unavailable;
    uint32_t bad;
} KageVitaLaserProfileStats;

# define KAGE_VITA_LASER_ACTIVE_DEPTH 4u
typedef struct KageVitaLaserActive {
    uint32_t token;
    uint32_t cost_class;
    uint64_t started_at;
# if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
    vglIsaacGpuDrawStats vitagl;
# endif
} KageVitaLaserActive;

static KageVitaLaserProfileStats s_laser;
static KageVitaLaserActive
    s_laser_active[2][KAGE_VITA_LASER_ACTIVE_DEPTH];
static uint32_t s_laser_active_depth[2];
#endif
static uint32_t s_running;
#if defined(ISAAC_VITA_HEAP_CENSUS)
/* why= claims of the out-of-cadence ph120.mem records: bit0 badalloc, bit1
 * exit, bit2 any other tag. */
static uint32_t s_heap_census_final_claimed;
#endif

static uint64_t kage_vita_profile_now(void)
{
    return sceKernelGetProcessTimeWide();
}

#if defined(ISAAC_VITA_ROOM_LOG_MARKERS) && ISAAC_VITA_ROOM_LOG_MARKERS
#define KAGE_VITA_ROOM_LOG_CAPACITY 8u
typedef struct kage_vita_room_log_event {
    uint64_t at;
    uint32_t seq, assigned, window, loop, first, second;
} kage_vita_room_log_event;
static kage_vita_room_log_event s_room_log[KAGE_VITA_ROOM_LOG_CAPACITY];
static uint32_t s_room_log_count, s_room_log_epoch, s_room_log_seq;
static uint32_t s_room_log_drop, s_room_log_reset_lost, s_room_log_sat;
/* Current port has one guest-code thread, also owning phase reset/report.
 * UID rejection is not a cross-thread queue protocol: concurrent guest-worker
 * execution/reset must revisit this contract before such workers are enabled. */
static int s_room_log_owner;

/* Sticky bits: epoch=1, seq=2, dropped=4, reset_lost=8. Once an increment
 * cannot be represented, downstream must not infer unique IDs/exact totals. */
static void kage_vita_room_log_add(uint32_t *value, uint32_t amount, uint32_t bit)
{
    if (amount > UINT32_MAX - *value) {
        *value = UINT32_MAX;
        s_room_log_sat |= bit;
    } else {
        *value += amount;
    }
}

void kage_vita_phase_profile_room_log(uint32_t first, uint32_t second)
{
    int saved_errno = errno;
    kage_vita_room_log_event event;

    /* Foreign/unbound calls never touch the owner's queue or phase context.
     * This actual UID check is intentionally only on the rare CRT candidate. */
    if (s_room_log_owner <= 0 || sceKernelGetThreadId() != s_room_log_owner) {
        errno = saved_errno;
        return;
    }
    event.at = kage_vita_profile_now();
    kage_vita_room_log_add(&s_room_log_seq, 1u, 2u);
    event.seq = s_room_log_seq;
    event.assigned = s_running != 0u;
    event.window = s_running ? s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW + 1u : 0u;
    event.loop = s_running ? s_last_outer_loop : 0u;
    event.first = first;
    event.second = second;
    if (s_room_log_count < KAGE_VITA_ROOM_LOG_CAPACITY)
        s_room_log[s_room_log_count++] = event;
    else
        kage_vita_room_log_add(&s_room_log_drop, 1u, 4u);
    errno = saved_errno;
}

static void kage_vita_profile_log_room_events(const char *build_id,
                                             uint32_t window, uint32_t loop)
{
    uint32_t i;
    int saved_errno = errno;
    KVPP_PRINTF("[kage-vita] ph120.rl bid=%.32s win=%u loops=%u "
                "epoch=%u n=%u seq=%u drop=%u reset_lost=%u sat=%u\n",
        build_id, window, loop, s_room_log_epoch, s_room_log_count,
        s_room_log_seq, s_room_log_drop, s_room_log_reset_lost, s_room_log_sat);
    for (i = 0u; i < s_room_log_count; ++i) {
        const kage_vita_room_log_event *e = &s_room_log[i];
        /* A separate event prefix: multiple events are not duplicate ph120
         * windows. hi:lo avoids the SDK logger's unreliable %lli formatting. */
        KVPP_PRINTF("[kage-vita] roomlog bid=%.32s report_win=%u epoch=%u "
                    "seq=%u at=%08x:%08x assigned=%u win=%u loop=%u "
                    "pair=%d,%d sat=%u\n",
            build_id, window, s_room_log_epoch, e->seq,
            (uint32_t)(e->at >> 32), (uint32_t)e->at, e->assigned,
            e->window, e->loop, (int32_t)e->first, (int32_t)e->second,
            s_room_log_sat);
    }
    s_room_log_count = 0u;
    errno = saved_errno;
}
#if defined(ISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE)
/* Existing oracle's integer-boundary injection; absent from production. */
void kage_vita_phase_profile_oracle_room_seed(uint32_t value)
{
    s_room_log_count = 0u;
    s_room_log_epoch = s_room_log_seq = value;
    s_room_log_drop = s_room_log_reset_lost = value;
    s_room_log_sat = 0u;
}
#endif
#endif

#if defined(ISAAC_VITA_FRAME_SPIKE_PROFILE)
static void kage_vita_profile_spike_begin(void)
{
    s_spike_gl = g_isaac_vita_gl_phase_profile_counters;
    s_spike_bad_start = s_bad_sequence;
    s_spike_clamp_start = s_clamped_duration;
}

static void kage_vita_profile_spike_finish(uint64_t now)
{
    KageVitaFrameSpike *p = &s_spikes.current;
    const IsaacVitaGlPhaseProfileCounters *g =
        &g_isaac_vita_gl_phase_profile_counters;
    p->draws = g->draw_elements - s_spike_gl.draw_elements;
    p->clears = g->clear - s_spike_gl.clear;
    p->fbos = g->bind_framebuffer - s_spike_gl.bind_framebuffer;
    p->uploads = g->tex_image - s_spike_gl.tex_image;
    p->bad = s_bad_sequence - s_spike_bad_start;
    p->clamped |= s_clamped_duration != s_spike_clamp_start;
    kage_vita_spike_finish(&s_spikes, s_last_outer_loop, now,
        now >= s_whole_started_at ? now - s_whole_started_at : 0u);
}

static void kage_vita_profile_log_spikes(uint32_t last_outer_loop)
{
    static const char *const kind[] = {"all", "upd", "rnd"};
    uint32_t i;
    for (i = 0u; i < KAGE_VITA_SPIKE_WINNERS; ++i) {
        const KageVitaFrameSpike *p = &s_spikes.worst[i];
        KVPP_PRINTF(
            "[kage-vita] ph120.sp bid=%.32s win=%u loops=%u kind=%.3s "
            "loop=%u at_ms=%u all=%u svc=%u upd=%u rnd=%u swp=%u lim=%u "
            "mask=%u draw=%u clear=%u fbo=%u tex=%u bad=%u clamp=%u\n",
            ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
            s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
            last_outer_loop, kind[i], p->loop, p->at_ms, p->whole_us,
            p->phase_us[0], p->phase_us[1], p->phase_us[2],
            p->phase_us[3], p->phase_us[4], p->phase_mask,
            p->draws, p->clears, p->fbos, p->uploads, p->bad, p->clamped);
    }
}
#endif

static void kage_vita_profile_reset_window(void)
{
    memset(s_samples, 0, sizeof s_samples);
#if defined(ISAAC_VITA_FRAME_SPIKE_PROFILE)
    memset(&s_spikes, 0, sizeof s_spikes);
#endif
    s_bad_sequence = 0u;
    s_clamped_duration = 0u;
#if defined(ISAAC_VITA_PILL_BLOOM_BYPASS)
    s_pill_bloom_capture_skips = 0u;
    s_pill_bloom_composite_skips = 0u;
#endif
#if defined(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES)
    s_bloom_half_captures = 0u;
    s_bloom_half_composites = 0u;
#endif
#if defined(ISAAC_VITA_POOP_FX_PROFILE)
    memset(&s_poop_fx, 0, sizeof s_poop_fx);
    s_poop_fx.countdown_min = UINT32_MAX;
#endif
#if defined(ISAAC_VITA_LASER_PROFILE)
    memset(&s_laser, 0, sizeof s_laser);
    memset(s_laser_active, 0, sizeof s_laser_active);
    memset(s_laser_active_depth, 0, sizeof s_laser_active_depth);
#endif
}

void kage_vita_phase_profile_reset(void)
{
#if defined(ISAAC_VITA_ROOM_LOG_MARKERS) && ISAAC_VITA_ROOM_LOG_MARKERS
    {
        int saved_errno = errno;
        /* Reset also runs at shutdown/reinitialization. Account for pending
         * events instead of silently reusing their epoch/sequence identity. */
        kage_vita_room_log_add(&s_room_log_reset_lost, s_room_log_count, 8u);
        kage_vita_room_log_add(&s_room_log_epoch, 1u, 1u);
        s_room_log_count = s_room_log_seq = 0u;
        s_room_log_owner = sceKernelGetThreadId();
        errno = saved_errno;
    }
#endif
#if defined(ISAAC_VITA_LASER_HALO_PROFILE) && ISAAC_VITA_LASER_HALO_PROFILE
    s_window_halo_valid = s_window_halo_from = 0u;
#endif
    kage_vita_profile_reset_window();
#if defined(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES)
    s_bloom_half_create_initial = 0u;
    s_bloom_half_create_applied = 0u;
    s_bloom_half_create_rejected = 0u;
#endif
    memset(&s_service, 0, sizeof s_service);
    memset(&s_update, 0, sizeof s_update);
    memset(&s_render, 0, sizeof s_render);
    memset(&s_present, 0, sizeof s_present);
    s_last_present_return = 0u;
    s_have_present_return = 0u;
    memset(&s_limiter, 0, sizeof s_limiter);
#if defined(ISAAC_VITA_PHASE_PROFILE_OTHER)
    s_other_started_at = 0u;
    s_other_accumulated = 0u;
    s_other_active = 0u;
    s_other_pending = 0u;
#endif
    s_whole_started_at = 0u;
    s_last_outer_loop = 0u;
    s_completed_loops = 0u;
    memset(&s_window_guest, 0, sizeof s_window_guest);
    memset(&g_guest_phase_profile_counters, 0,
           sizeof g_guest_phase_profile_counters);
#if defined(KAGE_VITA_PROFILE_IMPORT_CENSUS)
    memset(s_window_import_calls, 0, sizeof s_window_import_calls);
    memset(s_import_calls_delta, 0, sizeof s_import_calls_delta);
    memset(g_guest_phase_profile_import_calls, 0,
           sizeof g_guest_phase_profile_import_calls);
#endif
    KAGE_VITA_PROFILE_IMPORT_KINDS_BASELINE();
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
    s_window_lookup_cache_hits = 0U;
    s_window_lookup_cache_misses = 0U;
    g_guest_lookup_cache_hits = 0U;
    g_guest_lookup_cache_misses = 0U;
#endif
    memset(&s_window_gl, 0, sizeof s_window_gl);
    memset(&g_isaac_vita_gl_phase_profile_counters, 0,
           sizeof g_isaac_vita_gl_phase_profile_counters);
    gl_vita_backend_phase_profile_window_boundary();
#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
    memset(&s_window_vitagl, 0, sizeof s_window_vitagl);
#endif
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
    memset(&s_window_coloroffset, 0, sizeof s_window_coloroffset);
#endif
#if defined(ISAAC_VITA_COLOROFFSET_FS_PROBE)
    memset(&s_window_fs_probe, 0, sizeof s_window_fs_probe);
#endif
#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
    memset(&s_window_scheduler, 0, sizeof s_window_scheduler);
    s_last_emit_us = 0u;
    s_max_emit_us = 0u;
#endif
#if defined(ISAAC_VITA_HEAP_CENSUS)
    s_heap_census_final_claimed = 0u;
#endif
    s_running = 0u;
}

static void kage_vita_profile_record(
    enum kage_vita_profile_phase phase, uint64_t begin, uint64_t end)
{
    kage_vita_profile_samples *samples = &s_samples[phase];
    uint64_t elapsed;
    uint32_t value;

    if (end < begin) {
        ++s_bad_sequence;
        return;
    }
    elapsed = end - begin;
    if (elapsed > UINT32_MAX) {
        value = UINT32_MAX;
        ++s_clamped_duration;
    } else {
        value = (uint32_t)elapsed;
    }
    if (samples->count < KAGE_VITA_PHASE_PROFILE_WINDOW)
        samples->values[samples->count++] = value;
    else
        ++samples->dropped;
#if defined(ISAAC_VITA_FRAME_SPIKE_PROFILE)
    /* First five phase IDs above are deliberately the spike ABI order. */
    kage_vita_spike_record(&s_spikes, (uint32_t)phase, value);
#endif
}

static void kage_vita_profile_begin(
    kage_vita_profile_active *active, uint64_t now)
{
    if (active->active)
        ++s_bad_sequence;
    active->started_at = now;
    active->active = 1u;
}

static void kage_vita_profile_end(
    enum kage_vita_profile_phase phase,
    kage_vita_profile_active *active, uint64_t now)
{
    if (!active->active) {
        ++s_bad_sequence;
        return;
    }
    active->active = 0u;
    kage_vita_profile_record(phase, active->started_at, now);
}

#if defined(ISAAC_VITA_PHASE_PROFILE_OTHER)
static void kage_vita_profile_other_begin(uint64_t now)
{
    if (s_other_active)
        return;
    s_other_started_at = now;
    s_other_active = 1u;
}

static void kage_vita_profile_other_pause(uint64_t now)
{
    if (!s_other_active)
        return;
    s_other_active = 0u;
    s_other_pending = 1u;
    if (now >= s_other_started_at)
        s_other_accumulated += now - s_other_started_at;
    else
        ++s_bad_sequence;
}

/* Service entry: close the loop's oth sample (both segments summed). */
static void kage_vita_profile_other_close(uint64_t now)
{
    kage_vita_profile_other_pause(now);
    if (s_other_pending)
        kage_vita_profile_record(KAGE_VITA_PROFILE_OTHER, 0u,
                                 s_other_accumulated);
    s_other_accumulated = 0u;
    s_other_pending = 0u;
}
#endif

static void kage_vita_profile_sort(uint32_t *values, uint32_t count)
{
    uint32_t i;

    for (i = 1u; i < count; ++i) {
        uint32_t value = values[i];
        uint32_t j = i;

        while (j != 0u && values[j - 1u] > value) {
            values[j] = values[j - 1u];
            --j;
        }
        values[j] = value;
    }
}

static kage_vita_profile_summary kage_vita_profile_summarize(
    const kage_vita_profile_samples *samples)
{
    kage_vita_profile_summary result;
    uint32_t ordered[KAGE_VITA_PHASE_PROFILE_WINDOW];
    uint32_t p50_index;
    uint32_t p95_index;

    memset(&result, 0, sizeof result);
    result.count = samples->count + samples->dropped;
    if (!samples->count)
        return result;
    memcpy(ordered, samples->values,
           samples->count * sizeof samples->values[0]);
    kage_vita_profile_sort(ordered, samples->count);
    /* Nearest-rank percentiles: ceil(P*N)-1. */
    p50_index = (50u * samples->count + 99u) / 100u - 1u;
    p95_index = (95u * samples->count + 99u) / 100u - 1u;
    result.minimum = ordered[0];
    result.p50 = ordered[p50_index];
    result.p95 = ordered[p95_index];
    result.maximum = ordered[samples->count - 1u];
    return result;
}

static uint32_t kage_vita_profile_dropped_total(void)
{
    uint32_t total = 0u;
    uint32_t phase;

    for (phase = 0u; phase < KAGE_VITA_PROFILE_PHASE_COUNT; ++phase)
        total += s_samples[phase].dropped;
    return total;
}

static void kage_vita_profile_log_timings(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const kage_vita_profile_summary *service,
    const kage_vita_profile_summary *update,
    const kage_vita_profile_summary *render,
    const kage_vita_profile_summary *present,
    const kage_vita_profile_summary *limiter,
#if defined(ISAAC_VITA_PHASE_PROFILE_OTHER)
    const kage_vita_profile_summary *other,
#endif
    const kage_vita_profile_summary *whole,
    const kage_vita_profile_summary *present_gap,
    uint32_t bad_sequence, uint32_t clamped_duration)
{
#if defined(ISAAC_VITA_PHASE_PROFILE_OTHER)
    /* oth and gap still fit the 512-byte durable-record bound with every
     * integer at its maximum width; the existing host fixture checks it. */
    KVPP_PRINTF(
        "[kage-vita] ph120.t bid=%.32s win=%u loops=%u "
        "us(50,95,max) "
        "svc=%u/%u/%u upd=%u/%u/%u rnd=%u/%u/%u "
        "swp=%u/%u/%u lim=%u/%u/%u oth=%u/%u/%u all=%u/%u/%u "
        "gap(n,min,50,95,max)=%u/%u/%u/%u/%u "
        "bad=%u clamp=%u\n",
        build_id, window, last_outer_loop,
        service->p50, service->p95, service->maximum,
        update->p50, update->p95, update->maximum,
        render->p50, render->p95, render->maximum,
        present->p50, present->p95, present->maximum,
        limiter->p50, limiter->p95, limiter->maximum,
        other->p50, other->p95, other->maximum,
        whole->p50, whole->p95, whole->maximum,
        present_gap->count, present_gap->minimum,
        present_gap->p50, present_gap->p95, present_gap->maximum,
        bad_sequence, clamped_duration);
#else
    /* Keep this one physical record below the established 512-byte durable
     * log bound even when every printed uint32_t is ten decimal digits. */
    KVPP_PRINTF(
        "[kage-vita] ph120.t bid=%.32s win=%u loops=%u "
        "us(50,95,max) "
        "svc=%u/%u/%u upd=%u/%u/%u rnd=%u/%u/%u "
        "swp=%u/%u/%u lim=%u/%u/%u all=%u/%u/%u "
        "gap(n,min,50,95,max)=%u/%u/%u/%u/%u "
        "bad=%u clamp=%u\n",
        build_id, window, last_outer_loop,
        service->p50, service->p95, service->maximum,
        update->p50, update->p95, update->maximum,
        render->p50, render->p95, render->maximum,
        present->p50, present->p95, present->maximum,
        limiter->p50, limiter->p95, limiter->maximum,
        whole->p50, whole->p95, whole->maximum,
        present_gap->count, present_gap->minimum,
        present_gap->p50, present_gap->p95, present_gap->maximum,
        bad_sequence, clamped_duration);
#endif
}

#if defined(ISAAC_VITA_GL_TIME_PROFILE)
static void kage_vita_profile_log_gl_time(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const IsaacVitaGlTimeProfile *time,
    const kage_vita_scene_times *scene)
{
# if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE) && ISAAC_VITA_GL_TIME_SDK_SPARSE
    const IsaacSdkSparseProfile *s = &scene->sparse;
    const IsaacSdkSparseBucket *b = s->bucket;
    (void)time;
    /* API caller wall time only. No broad bucket fields are emitted in this
     * mode; absent is not zero. Canonical outer/draw exclude every unpaired
     * selected span together; clear/queue cover all actual calls. */
    KVPP_PRINTF(
        "[kage-vita] ph120.gt bid=%.32s win=%u loops=%u mode=sdk32 scope=scene "
        "scene(n,b,e,bm)=%u,%u,%u,%u sdk(b,e)=%u,%u "
        "clocks=%u bucket_bad=%u bucket_sat=%u scene_check=legacy ctl(n,us,max)=%u,%u,%u\n",
        build_id, window, last_outer_loop,
        scene->count, scene->begin_us, scene->end_us, scene->begin_max_us,
        s->begin_calls, s->end_calls, s->clock_reads, s->bad_clock, s->saturation,
        b[ISAAC_SDK_CONTROL].calls, b[ISAAC_SDK_CONTROL].us, b[ISAAC_SDK_CONTROL].max_us);
    KVPP_PRINTF(
        "[kage-vita] ph120.gt bid=%.32s win=%u loops=%u mode=sdk32 scope=canonical "
        "res=%u seen=%u selected=%u issued=%u unpaired=%u draw_errors=%u other=%u "
        "outer(n,us,max)=%u,%u,%u draw(n,us,max)=%u,%u,%u\n",
        build_id, window, last_outer_loop,
        s->residue, s->canonical_seen, s->canonical_selected, s->canonical_issued,
        s->canonical_unpaired, s->canonical_draw_errors, s->other_draw_calls,
        b[ISAAC_SDK_CANONICAL].calls, b[ISAAC_SDK_CANONICAL].us, b[ISAAC_SDK_CANONICAL].max_us,
        b[ISAAC_SDK_DRAW].calls, b[ISAAC_SDK_DRAW].us, b[ISAAC_SDK_DRAW].max_us);
    KVPP_PRINTF(
        "[kage-vita] ph120.gt bid=%.32s win=%u loops=%u mode=sdk32 scope=clear "
        "outer(n,us,max)=%u,%u,%u vertex(n,us,max)=%u,%u,%u "
        "fragment(n,us,max)=%u,%u,%u draw(n,us,max)=%u,%u,%u\n",
        build_id, window, last_outer_loop,
        b[ISAAC_SDK_CLEAR].calls, b[ISAAC_SDK_CLEAR].us, b[ISAAC_SDK_CLEAR].max_us,
        b[ISAAC_SDK_CLEAR_VERTEX].calls, b[ISAAC_SDK_CLEAR_VERTEX].us, b[ISAAC_SDK_CLEAR_VERTEX].max_us,
        b[ISAAC_SDK_CLEAR_FRAGMENT].calls, b[ISAAC_SDK_CLEAR_FRAGMENT].us, b[ISAAC_SDK_CLEAR_FRAGMENT].max_us,
        b[ISAAC_SDK_CLEAR_DRAW].calls, b[ISAAC_SDK_CLEAR_DRAW].us, b[ISAAC_SDK_CLEAR_DRAW].max_us);
    KVPP_PRINTF(
        "[kage-vita] ph120.gt bid=%.32s win=%u loops=%u mode=sdk32 scope=queue "
        "queue(n,us,max)=%u,%u,%u\n",
        build_id, window, last_outer_loop,
        b[ISAAC_SDK_QUEUE].calls, b[ISAAC_SDK_QUEUE].us, b[ISAAC_SDK_QUEUE].max_us);
#  if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE_ATTRIB)
    /* Shader::EnableAttribs/DisableAttribs host replays: every 32nd HANDLED
     * replay per kind timed entry to `ret 0xc` (gl_vita_backend.h legend);
     * replays(en,di) are all HANDLED replays of the window, n ~ replays/32. */
    KVPP_PRINTF(
        "[kage-vita] ph120.gt bid=%.32s win=%u loops=%u mode=sdk32 scope=attrib "
        "res=%u replays(en,di)=%u,%u en(n,us,max)=%u,%u,%u di(n,us,max)=%u,%u,%u "
        "bad=%u\n",
        build_id, window, last_outer_loop,
        scene->attrib.residue, scene->attrib.enable_replays,
        scene->attrib.disable_replays,
        scene->attrib.enable.calls, scene->attrib.enable.us,
        scene->attrib.enable.max_us,
        scene->attrib.disable.calls, scene->attrib.disable.us,
        scene->attrib.disable.max_us,
        scene->attrib.bad_clock);
#  endif
# else
    /* Window sums of process-clock microseconds inside native vitaGL calls:
     * d=glDrawElements/canonical quads, c=glClear, b=glBindFramebuffer +
     * glFramebufferTexture2D, t=glTexImage2D/glTexSubImage2D, s=program/
     * uniform/attrib/texture/blend/depth/viewport/enable state, p=present.
     * max is the longest single call of d, c and b (the three that can open a
     * GXM scene).  n counts wrapped calls per bucket.  scene comes from vitaGL:
     * scenes begun, total BeginScene us, total EndScene us, longest BeginScene.
     * e is where the CPU waits for the GPU on the device (sceGxmEndScene;
     * BeginScene never does, bm stays under 0.1 ms) and is split further in
     * ph120.gx; whether the wait is the one-scene-slot render target is the
     * FBO_RT_SCENES A/B question, not a fact of this legend.  rnd (ph120.t,
     * summed over the window) minus d+c+b+t+s is translated guest code plus
     * our boundary bookkeeping. */
    KVPP_PRINTF(
        "[kage-vita] ph120.gt bid=%.32s win=%u loops=%u "
        "us(d,c,b,t,s,p)=%u,%u,%u,%u,%u,%u max(d,c,b)=%u,%u,%u "
        "n(d,c,b,t,s,p)=%u,%u,%u,%u,%u,%u scene(n,b,e,bm)=%u,%u,%u,%u "
        "bad=%u\n",
        build_id, window, last_outer_loop,
        time->draw.total_us, time->clear.total_us, time->bind_fb.total_us,
        time->tex_upload.total_us, time->state.total_us,
        time->present.total_us,
        time->draw.max_us, time->clear.max_us, time->bind_fb.max_us,
        time->draw.calls, time->clear.calls, time->bind_fb.calls,
        time->tex_upload.calls, time->state.calls, time->present.calls,
        scene->count, scene->begin_us, scene->end_us, scene->begin_max_us,
        time->bad_clock);
# endif
}

# if defined(ISAAC_VITA_GL_WRAPPER_TIME)
/* ph120.gw, directly after ph120.gt (gl_vita_backend.h legend): w = wall
 * microseconds from GL wrapper entry to return per bucket d,c,b,t,s,p,a,u,o
 * (same clock and bucket assignment as gt, so w - gt.us is the shim's own
 * cost; a and u have their gt bodies inside gt s), n = wrapper calls in the
 * same order, k(en,di) = us,calls of the handled Shader::EnableAttribs /
 * DisableAttribs host replays (ISAAC_VITA_SHADER_ATTRIB_FASTPATH; zero
 * without it), bad = reversed clock reads.  All-ones: 379 bytes. */
static void kage_vita_profile_log_gl_wrapper_time(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const IsaacVitaGlWrapperTimeProfile *wrapper)
{
    const IsaacVitaGlTimeBucket *w = wrapper->wrapper;
    const IsaacVitaGlTimeBucket *k = wrapper->slot;

    KVPP_PRINTF(
        "[kage-vita] ph120.gw bid=%.32s win=%u loops=%u "
        "w(d,c,b,t,s,p,a,u,o)=%u,%u,%u,%u,%u,%u,%u,%u,%u "
        "n=%u,%u,%u,%u,%u,%u,%u,%u,%u k(en,di)=%u,%u/%u,%u bad=%u\n",
        build_id, window, last_outer_loop,
        w[0].total_us, w[1].total_us, w[2].total_us, w[3].total_us,
        w[4].total_us, w[5].total_us, w[6].total_us, w[7].total_us,
        w[8].total_us,
        w[0].calls, w[1].calls, w[2].calls, w[3].calls, w[4].calls,
        w[5].calls, w[6].calls, w[7].calls, w[8].calls,
        k[0].total_us, k[0].calls, k[1].total_us, k[1].calls,
        wrapper->bad_clock);
}

/* ph120.gd, directly after ph120.gw (gl_vita_backend.h legend): the draw
 * wrapper's w(d) split along vita_glDrawElements, d(disp,sync,cls,cen,fbo,
 * gl,tail) in us with gl == gt d by construction (same reads); n(dr,cq,ea,
 * da) = draws, canonical zero-copy quad hits, attribs replayed by the handled
 * EnableAttribs / DisableAttribs replays; en/di(own,loc,gl) = the k(en,di)
 * us split (own + loc + gl == k us).  All-ones: 370 bytes. */
static void kage_vita_profile_log_gl_draw_split(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const IsaacVitaGlWrapperTimeProfile *wrapper)
{
    const IsaacVitaGlTimeBucket *d = wrapper->draw;
    const IsaacVitaGlTimeBucket *en =
        wrapper->replay[ISAAC_VITA_GL_WRAPPER_K_ATTRIB_ENABLE];
    const IsaacVitaGlTimeBucket *di =
        wrapper->replay[ISAAC_VITA_GL_WRAPPER_K_ATTRIB_DISABLE];

    KVPP_PRINTF(
        "[kage-vita] ph120.gd bid=%.32s win=%u loops=%u "
        "d(disp,sync,cls,cen,fbo,gl,tail)=%u,%u,%u,%u,%u,%u,%u "
        "n(dr,cq,ea,da)=%u,%u,%u,%u en(own,loc,gl)=%u,%u,%u "
        "di(own,loc,gl)=%u,%u,%u bad=%u\n",
        build_id, window, last_outer_loop,
        d[0].total_us, d[1].total_us, d[2].total_us, d[3].total_us,
        d[4].total_us, d[5].total_us, d[6].total_us,
        d[ISAAC_VITA_GL_DRAW_SPLIT_DISPATCH].calls, wrapper->draw_canonical,
        en[ISAAC_VITA_GL_REPLAY_LOC].calls, di[ISAAC_VITA_GL_REPLAY_LOC].calls,
        en[0].total_us, en[1].total_us, en[2].total_us,
        di[0].total_us, di[1].total_us, di[2].total_us,
        wrapper->bad_clock);
}

/* ph120.gu, directly after ph120.gd: the u wrapper bucket by frozen entry
 * point, us,calls each (glUniformMatrix4fv, glUniform4fv, glUniform2fv,
 * glUniform1i, other glUniform*).  All-ones: 232 bytes. */
static void kage_vita_profile_log_gl_uniform_split(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const IsaacVitaGlWrapperTimeProfile *wrapper)
{
    const IsaacVitaGlTimeBucket *u = wrapper->uniform;

    KVPP_PRINTF(
        "[kage-vita] ph120.gu bid=%.32s win=%u loops=%u "
        "u(m4,4f,2f,1i,o)=%u,%u/%u,%u/%u,%u/%u,%u/%u,%u bad=%u\n",
        build_id, window, last_outer_loop,
        u[0].total_us, u[0].calls, u[1].total_us, u[1].calls,
        u[2].total_us, u[2].calls, u[3].total_us, u[3].calls,
        u[4].total_us, u[4].calls, wrapper->bad_clock);
}
# endif

# if defined(ISAAC_VITA_STOCK_FBO_VALID_REGION)
#  define KAGE_VITA_PROFILE_GX_VR_FORMAT " vr(s,p,u,a,e,v,c)=%u,%u,%u,%u,%u,%u,%u"
#  define KAGE_VITA_PROFILE_GX_VR_ARGS(scene) \
    , (scene)->vr[0], (scene)->vr[1], (scene)->vr[2], (scene)->vr[3], \
      (scene)->vr[4], (scene)->vr[5], (scene)->vr[6]
# else
#  define KAGE_VITA_PROFILE_GX_VR_FORMAT ""
#  define KAGE_VITA_PROFILE_GX_VR_ARGS(scene)
# endif

static void kage_vita_profile_log_gl_scene_split(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const kage_vita_scene_times *scene)
{
    /* 0006 split of scene(e) from ph120.gt, same window.  end: EndScene us
     * of offscreen (f) and display (d) scenes (f + d == e), the longest single
     * EndScene, and how many exceeded 500 us (slow).  ord: offscreen EndScene
     * us by ordinal since the last vglSwapBuffers (1st, 2nd, 3rd, 4th and
     * later; the display end at the swap is d).  Under the per-target
     * scene-slot model the 2nd and 3rd offscreen ends carry the wait and d
     * stays near zero; under a global GPU-behind model the waits spread over
     * every ordinal and d.  rt: offscreen render targets created (c; the
     * FBO's target is recreated when its attachment size changes, so a
     * steady window must show 0), sceGxmCreateRenderTarget failures (x; must
     * stay 0, the 0007 creation site falls back to one scene slot) and
     * offscreen depth buffers created (z).  Worst case 253 bytes (every
     * field ten digits), inside the 384-byte durable log bound.  With
     * ISAAC_VITA_STOCK_FBO_VALID_REGION the 0009 counters follow as
     * vr(s,p,u,a,e,v,c) (see vglIsaacFboValidRegionStats; the census
     * overflow o and the size tuples go to ph120.vz): worst case 348 bytes. */
    KVPP_PRINTF(
        "[kage-vita] ph120.gx bid=%.32s win=%u loops=%u "
        "end(f,d,max,slow)=%u,%u,%u,%u ord(1,2,3,4+)=%u,%u,%u,%u "
        "rt(c,x,z)=%u,%u,%u" KAGE_VITA_PROFILE_GX_VR_FORMAT "\n",
        build_id, window, last_outer_loop,
        scene->end_fbo_us, scene->end_display_us, scene->end_max_us,
        scene->end_slow,
        scene->end_ordinal_us[0], scene->end_ordinal_us[1],
        scene->end_ordinal_us[2], scene->end_ordinal_us[3],
        scene->rt_created, scene->rt_failed, scene->depth_created
        KAGE_VITA_PROFILE_GX_VR_ARGS(scene));
}

# if defined(ISAAC_VITA_STOCK_FBO_VALID_REGION)
static void kage_vita_profile_log_fbo_valid_region_census(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const kage_vita_scene_times *scene)
{
    /* 0009 size census, same take as ph120.gx.  o: distinct (texture,
     * rect) size tuples beyond the four remembered (must stay 0; the game
     * has two offscreen sizes).  sz: up to four TWxTH/RWxRH/N tuples in
     * first-seen order (texture size / logical rect / FBO scenes begun on
     * that pair this window, only those with a known rect, so the N sum
     * equals p + the full-size scenes).  Unused tuples print 0x0/0x0/0.
     * Worst case 326 bytes. */
    KVPP_PRINTF(
        "[kage-vita] ph120.vz bid=%.32s win=%u loops=%u o=%u "
        "sz=%ux%u/%ux%u/%u,%ux%u/%ux%u/%u,%ux%u/%ux%u/%u,%ux%u/%ux%u/%u\n",
        build_id, window, last_outer_loop, scene->vr[7],
        scene->vr_census[0], scene->vr_census[1], scene->vr_census[2],
        scene->vr_census[3], scene->vr_census[4],
        scene->vr_census[5], scene->vr_census[6], scene->vr_census[7],
        scene->vr_census[8], scene->vr_census[9],
        scene->vr_census[10], scene->vr_census[11], scene->vr_census[12],
        scene->vr_census[13], scene->vr_census[14],
        scene->vr_census[15], scene->vr_census[16], scene->vr_census[17],
        scene->vr_census[18], scene->vr_census[19]);
}
# endif
#endif

static void kage_vita_profile_log_counts(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const kage_vita_profile_summary *service,
    const kage_vita_profile_summary *update,
    const kage_vita_profile_summary *render,
    const kage_vita_profile_summary *present,
    const kage_vita_profile_summary *limiter,
    const kage_vita_profile_summary *whole,
    const GuestPhaseProfileCounters *guest,
    const IsaacVitaGlPhaseProfileCounters *gl,
    uint32_t dropped)
{
    uint32_t guest_calls = guest->guest_calls;
    uint32_t guest_lookups = guest->guest_lookups;

#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
    /* guest_call counts its entries in dispatch_calls only; a table hit is
     * the successor of a confirmed dispatch-cache hit (one call, one lookup),
     * so the record keeps its established meaning.  ph120.d has the raw split. */
    guest_calls += guest->dispatch_calls;
    guest_lookups += guest->dispatch_calls - guest->dispatch_slow;
# if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
    /* ISAAC_VITA_GL_SHIM_TABLE_TOKENS: a typed-GL token that hit the table
     * is a dynamic call, not a lookup (guest_call_slow used to count it in
     * dispatch_slow), so the derived g(l) excludes it and keeps its value. */
    guest_lookups -= guest->dispatch_gl;
# endif
#endif
    /* n order: service, update, render, present/swap, limiter, whole.
     * g order: guest_call, guest_lookup, binary-search iterations.
     * gl order: drawElements, clear, bindTexture, useProgram, texImage,
     * texSubImage, uniform*, attribPointer, attrib enable+disable,
     * bindFramebuffer, and aggregate blend/cull/depth/viewport state. */
    KVPP_PRINTF(
        "[kage-vita] ph120.c bid=%.32s win=%u loops=%u "
        "n(s,u,r,p,l,w)=%u,%u,%u,%u,%u,%u "
        "g(c,l,i)=%u,%u,%u "
        "gl(d,c,t,p,i,s,u,a,v,f,x)="
        "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u drop=%u\n",
        build_id, window, last_outer_loop,
        service->count, update->count, render->count,
        present->count, limiter->count, whole->count,
        guest_calls, guest_lookups,
        guest->lookup_iterations,
        gl->draw_elements, gl->clear, gl->bind_texture,
        gl->use_program, gl->tex_image, gl->tex_sub_image,
        gl->uniform, gl->attrib_pointer, gl->attrib_toggle,
        gl->bind_framebuffer, gl->state, dropped);
}

static void kage_vita_profile_log_locations(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const IsaacVitaGlPhaseProfileCounters *gl,
    uint32_t time_get_time_calls)
{
    /* Separate from ph120.c so its established format and sub-384-byte bound
     * remain unchanged.  loc order: guest glGetAttribLocation requests, guest
     * glGetUniformLocation requests, shim location-cache hits over both
     * queries (zero without ISAAC_VITA_GL_LOCATION_CACHE) and VERIFY-variant
     * hit/native mismatches (must stay zero; the replay memo's
     * ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_VERIFY counts into the same
     * field).  tgt: guest timeGetTime import
     * calls (the sub_00564f50 pending-release loop; research-gl-shim-path
     * finding 6 named both streams as uncounted). */
    KVPP_PRINTF(
        "[kage-vita] ph120.a bid=%.32s win=%u loops=%u "
        "loc(a,u,h,m)=%u,%u,%u,%u tgt=%u\n",
        build_id, window, last_outer_loop,
        gl->attrib_location, gl->uniform_location,
        gl->location_cache_hit, gl->location_cache_mismatch,
        time_get_time_calls);
}

#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
/* ph120.w: the previous window's synchronous emission block on the game
 * thread -- the loop-head deltas, percentile sort, every ph120 record and the
 * window reset, clock-paired like the phases -- and its process-lifetime
 * maximum.  The at= gap between consecutive ph120.s records minus the 120
 * loops is this stall; the first window prints 0,0.  Its own record because
 * ph120.s already sits at the 512-byte worst case.  Worst case 124 bytes. */
static void kage_vita_profile_log_emission(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    uint32_t emit_us, uint32_t emit_max_us)
{
    KVPP_PRINTF(
        "[kage-vita] ph120.w bid=%.32s win=%u loops=%u emit(us,max)=%u,%u\n",
        build_id, window, last_outer_loop, emit_us, emit_max_us);
}

static void kage_vita_profile_log_scheduler(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    uint64_t report_at_us,
    const KageVitaFullspeedSnapshot *value,
    const KageVitaFullspeedSnapshot *prior)
{
    /* Counts are per 120-loop profile window.  `cad` proves the unchanged
     * wrapper/service/Manager/Game/Render/Present ownership chain; `ctr`
     * separates Continue's exact counter-normalization seam, accepted rebases
     * and rejects; `gptr` does the same for Game publications; `frame` keeps
     * the latest and fail-open Game return-site/begin/end evidence; `viol`
     * keeps the remaining fail-open reasons visible.
     * Debt and its maximum are the scheduler's current and process-lifetime
     * values, respectively. */
    KVPP_PRINTF(
        "[kage-vita] ph120.s bid=%.32s win=%u loops=%u "
        "at=%08x%08x cad(w,s,d,m,g,r,b,p)="
        "%u,%u,%u,%u,%u,%u,%u,%u "
        "phase(f,n,i)=%u,%u,%u skip(a,l,n,f)=%u,%u,%u,%u "
        "viol(d,p,g,s,o)=%u,%u,%u,%u,%u reset=%u "
        "drop(t,us)=%u,%u wait(c,us)=%u,%u debt=%u/%u\n",
        build_id, window, last_outer_loop,
        (unsigned)(uint32_t)(report_at_us >> 32),
        (unsigned)(uint32_t)report_at_us,
        value->wrapper_ticks - prior->wrapper_ticks,
        value->service_calls - prior->service_calls,
        value->manager_dispatch_calls - prior->manager_dispatch_calls,
        value->manager_entries - prior->manager_entries,
        value->game_updates - prior->game_updates,
        value->render_calls - prior->render_calls,
        value->render_bodies - prior->render_bodies,
        value->presents - prior->presents,
        value->full_phases - prior->full_phases,
        value->nonfull_phases - prior->nonfull_phases,
        value->interpolation_phases - prior->interpolation_phases,
        value->render_skips - prior->render_skips,
        value->late_full_skips - prior->late_full_skips,
        value->nonfull_skips - prior->nonfull_skips,
        value->forced_renders - prior->forced_renders,
        value->duplicate_tick_violations -
            prior->duplicate_tick_violations,
        value->parity_violations - prior->parity_violations,
        value->game_frame_violations - prior->game_frame_violations,
        value->sequence_violations - prior->sequence_violations,
        value->runtime_disables - prior->runtime_disables,
        value->clock_resets - prior->clock_resets,
        value->dropped_ticks - prior->dropped_ticks,
        value->dropped_us - prior->dropped_us,
        value->wait_calls - prior->wait_calls,
        value->waited_us - prior->waited_us,
        value->debt_us, value->maximum_debt_us);
    /* Keep the lifecycle/reason record below the 384-byte logger ceiling. */
    KVPP_PRINTF(
        "[kage-vita] ph120.r bid=%.32s win=%u loops=%u "
        "ctr(p,a,v)=%u,%u,%u gptr(p,a,v)=%u,%u,%u "
        "frame(last:s,b,e)=%08x,%u,%u "
        "frame(fail:s,b,e)=%08x,%u,%u\n",
        build_id, window, last_outer_loop,
        value->manager_counter_rebase_events -
            prior->manager_counter_rebase_events,
        value->manager_counter_rebases - prior->manager_counter_rebases,
        value->manager_counter_rebase_violations -
            prior->manager_counter_rebase_violations,
        value->game_pointer_publish_events -
            prior->game_pointer_publish_events,
        value->game_pointer_rebinds - prior->game_pointer_rebinds,
        value->game_pointer_violations - prior->game_pointer_violations,
        value->game_update_last_return_site,
        value->game_update_last_frame_before,
        value->game_update_last_frame_after,
        value->game_frame_violation_return_site,
        value->game_frame_violation_before,
        value->game_frame_violation_after);
}
#endif

#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
/* ph120.ik: the import mix behind g(c)-g(l), and the census identity.  (The
 * dispatch table's ph120.i record lists the same census by top ID; this
 * record sums it by binding kind.)
 *   imp(t,...)   t = host imports this window (g_host_import_calls delta,
 *                the same counter every family endpoint increments), then
 *                the per-ID census summed by binding kind: sync, heap,
 *                mem (VCRUNTIME memcpy/memset/memmove), crt, pcom (WINMM
 *                timeGetTime), math, lua, oth (all remaining kinds).
 *   dyn          GL/WGL/XInput dynamic-token dispatches (g_host_dynamic_calls).
 *   ent          translated body entries (0 unless
 *                ISAAC_VITA_PROFILE_FUNCTION_ENTRIES); direct+tail = ent-g(l).
 *   pace(c,us)   Game::Update pacing sleeps inside the upd phase (fullspeed
 *                scheduler); upd CPU ~= upd wall - us/loops.
 *   id(g,s)      identity residuals, both must be 0: g = g(c)-g(l)-t-dyn
 *                (every guest_call is a translated lookup, a host import or
 *                a dynamic token; with the dispatch table g(c) and g(l) are
 *                the derived ph120.c values), s = t - sum(per-ID census) (a
 *                nonzero s means a native seam reached a family endpoint
 *                without the per-ID note).  ok=1 when both are 0 and the hot
 *                pins hold.
 * ph120.ih: the fourteen pinned hot imports (host_vita_import_id.c
 * s_import_hot_pins): en/lv/te/sl = Enter/Leave/TryEnter CS, Sleep;
 * ma/fr/ca/re = malloc/free/calloc/realloc; mc/ms/mm = memcpy/memset/memmove;
 * tg = timeGetTime, qp = QueryPerformanceCounter, fl = floor.
 * Worst case 332 and 305 bytes with every counter at ten digits and both
 * residuals at INT32_MIN (kage_vita_phase_profile_oracle.c pins both). */
static void kage_vita_profile_log_import_kinds_records(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const IsaacVitaImportKindSnapshot *imports, uint32_t host_imports,
    uint32_t other, uint32_t dynamic, uint32_t entries,
    uint32_t pace_wait_calls, uint32_t pace_waited_us,
    int32_t residual, int32_t split, uint32_t ok)
{
    KVPP_PRINTF(
        "[kage-vita] ph120.ik bid=%.32s win=%u loops=%u "
        "imp(t,sync,heap,mem,crt,pcom,math,lua,oth)=%u,%u,%u,%u,%u,%u,%u,%u,%u "
        "dyn=%u ent=%u pace(c,us)=%u,%u id(g,s)=%d,%d ok=%u\n",
        build_id, window, last_outer_loop,
        host_imports,
        imports->kind[ISAAC_VITA_IMPORT_SYNC],
        imports->kind[ISAAC_VITA_IMPORT_HEAP],
        imports->kind[ISAAC_VITA_IMPORT_MEMORY],
        imports->kind[ISAAC_VITA_IMPORT_CRT],
        imports->kind[ISAAC_VITA_IMPORT_POST_COM],
        imports->kind[ISAAC_VITA_IMPORT_MATH],
        imports->kind[ISAAC_VITA_IMPORT_LUA],
        other, dynamic, entries, pace_wait_calls, pace_waited_us,
        (int)residual, (int)split, ok);
    KVPP_PRINTF(
        "[kage-vita] ph120.ih bid=%.32s win=%u loops=%u "
        "hot(en,lv,te,sl,ma,fr,ca,re,mc,ms,mm,tg,qp,fl)="
        "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u ok=%u\n",
        build_id, window, last_outer_loop,
        imports->hot[ISAAC_VITA_IMPORT_HOT_ENTER_CS],
        imports->hot[ISAAC_VITA_IMPORT_HOT_LEAVE_CS],
        imports->hot[ISAAC_VITA_IMPORT_HOT_TRY_ENTER_CS],
        imports->hot[ISAAC_VITA_IMPORT_HOT_SLEEP],
        imports->hot[ISAAC_VITA_IMPORT_HOT_MALLOC],
        imports->hot[ISAAC_VITA_IMPORT_HOT_FREE],
        imports->hot[ISAAC_VITA_IMPORT_HOT_CALLOC],
        imports->hot[ISAAC_VITA_IMPORT_HOT_REALLOC],
        imports->hot[ISAAC_VITA_IMPORT_HOT_MEMCPY],
        imports->hot[ISAAC_VITA_IMPORT_HOT_MEMSET],
        imports->hot[ISAAC_VITA_IMPORT_HOT_MEMMOVE],
        imports->hot[ISAAC_VITA_IMPORT_HOT_TIME_GET_TIME],
        imports->hot[ISAAC_VITA_IMPORT_HOT_QPC],
        imports->hot[ISAAC_VITA_IMPORT_HOT_FLOOR],
        imports->hot_ok);
}

static void kage_vita_profile_log_import_kinds(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const GuestPhaseProfileCounters *guest,
    uint32_t pace_wait_calls, uint32_t pace_waited_us)
{
    IsaacVitaImportKindSnapshot delta;
    uint32_t host_imports = g_host_import_calls - s_window_host_import_calls;
    uint32_t dynamic = g_host_dynamic_calls - s_window_host_dynamic_calls;
    uint32_t guest_calls = guest->guest_calls;
    uint32_t guest_lookups = guest->guest_lookups;
    uint32_t entries = 0u;
    uint32_t listed;
    int32_t residual;
    int32_t split;

    /* The window's per-ID deltas (kage_vita_profile_take_import_window) summed
     * by the frozen table's binding kinds; sums are linear, so the delta of
     * the sums is the sum of the deltas. */
    isaac_vita_import_id_calls_snapshot(
        s_import_calls_delta, GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS, &delta);
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
    /* The same derivation as ph120.c: table entries are calls, table hits
     * are lookups (kage_vita_profile_log_counts). */
    guest_calls += guest->dispatch_calls;
    guest_lookups += guest->dispatch_calls - guest->dispatch_slow;
# if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
    /* ISAAC_VITA_GL_SHIM_TABLE_TOKENS: a typed-GL token that hit the table
     * is a dynamic call, not a lookup (guest_call_slow used to count it in
     * dispatch_slow), so the derived g(l) excludes it and keeps its value. */
    guest_lookups -= guest->dispatch_gl;
# endif
#endif
#if defined(ISAAC_VITA_PROFILE_FUNCTION_ENTRIES)
    entries = g_guest_function_entries - s_window_function_entries;
#endif
    listed = delta.kind[ISAAC_VITA_IMPORT_SYNC] +
        delta.kind[ISAAC_VITA_IMPORT_HEAP] +
        delta.kind[ISAAC_VITA_IMPORT_MEMORY] +
        delta.kind[ISAAC_VITA_IMPORT_CRT] +
        delta.kind[ISAAC_VITA_IMPORT_POST_COM] +
        delta.kind[ISAAC_VITA_IMPORT_MATH] +
        delta.kind[ISAAC_VITA_IMPORT_LUA];
    /* Debug gate: guest_call = translated lookup | host import | dynamic
     * token, and the per-ID census must account for every host import. */
    residual = (int32_t)(guest_calls - guest_lookups -
                         host_imports - dynamic);
    split = (int32_t)(host_imports - delta.total);
    kage_vita_profile_log_import_kinds_records(
        build_id, window, last_outer_loop, &delta, host_imports,
        delta.total >= listed ? delta.total - listed : 0u,
        dynamic, entries, pace_wait_calls, pace_waited_us,
        residual, split,
        (residual == 0 && split == 0 && delta.hot_ok) ? 1u : 0u);
}
#endif

#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
static void kage_vita_profile_log_guest_cache(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    uint32_t lookup_cache_hits, uint32_t lookup_cache_misses)
{
    /* Separate from ph120.c so its established format and sub-384-byte bound
     * remain unchanged even when every counter reaches UINT32_MAX. */
    KVPP_PRINTF(
        "[kage-vita] ph120.g bid=%.32s win=%u loops=%u "
        "gc(h,m)=%u,%u\n",
        build_id, window, last_outer_loop,
        lookup_cache_hits, lookup_cache_misses);
}
#endif

#if defined(KAGE_VITA_PROFILE_IMPORT_CENSUS)
static void kage_vita_profile_take_import_window(void)
{
    uint32_t i;

    for (i = 0u; i < GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS; ++i)
        s_import_calls_delta[i] =
            g_guest_phase_profile_import_calls[i] - s_window_import_calls[i];
}
#endif

#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
static void kage_vita_profile_log_dispatch(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const GuestPhaseProfileCounters *guest)
{
    /* Raw guest_call census behind the derived ph120.c values: entries,
     * entries that left the inline probe (imports, dynamic tokens, RVA-form
     * keys, faults) and sync fast-path calls.  Table hits are c - s. */
#if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
    /* ISAAC_VITA_GL_SHIM_TABLE_TOKENS: g = typed-GL tokens that hit the
     * table (their dynamic calls are still in ph120.ik dyn); s no longer
     * includes them, translated hits are c - s - g. */
    KVPP_PRINTF(
        "[kage-vita] ph120.d bid=%.32s win=%u loops=%u "
        "d(c,s,sf,g)=%u,%u,%u,%u\n",
        build_id, window, last_outer_loop,
        guest->dispatch_calls, guest->dispatch_slow, guest->sync_fastpath,
        guest->dispatch_gl);
#else
    KVPP_PRINTF(
        "[kage-vita] ph120.d bid=%.32s win=%u loops=%u "
        "d(c,s,sf)=%u,%u,%u\n",
        build_id, window, last_outer_loop,
        guest->dispatch_calls, guest->dispatch_slow, guest->sync_fastpath);
#endif
}

#define KAGE_VITA_PROFILE_IMPORT_TOP 8U
#define KAGE_VITA_PROFILE_IMPORT_KINDS 32U
#define KAGE_VITA_PROFILE_IMPORT_KIND_TOP 4U

/* Top-N by count with ties broken toward the lowest index; zero-count
 * fillers keep the record shape fixed.  Indices already emitted (ids[0..n))
 * are skipped, so a count is never listed twice. */
static void kage_vita_profile_top(
    const uint32_t *counts, uint32_t count, uint32_t *ids, uint32_t *values,
    uint32_t top)
{
    uint32_t n, i, k;

    for (n = 0u; n < top; ++n) {
        uint32_t best = UINT32_MAX;

        for (i = 0u; i < count; ++i) {
            for (k = 0u; k < n && ids[k] != i; ++k) {
            }
            if (k != n)
                continue;
            if (best == UINT32_MAX || counts[i] > counts[best])
                best = i;
        }
        ids[n] = best == UINT32_MAX ? 0u : best;
        values[n] = best == UINT32_MAX ? 0u : counts[best];
    }
}

static void kage_vita_profile_log_imports(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const uint32_t *by_id, uint32_t sync_fastpath)
{
    uint32_t by_kind[KAGE_VITA_PROFILE_IMPORT_KINDS];
    uint32_t top_id[KAGE_VITA_PROFILE_IMPORT_TOP];
    uint32_t top_count[KAGE_VITA_PROFILE_IMPORT_TOP];
    uint32_t top_kind[KAGE_VITA_PROFILE_IMPORT_KIND_TOP];
    uint32_t top_kind_count[KAGE_VITA_PROFILE_IMPORT_KIND_TOP];
    uint32_t total = 0u, i;

    memset(by_kind, 0, sizeof by_kind);
    for (i = 0u; i < GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS; ++i) {
        int kind = guest_host_import_id_kind(i);

        total += by_id[i];
        if (kind >= 0 && (uint32_t)kind < KAGE_VITA_PROFILE_IMPORT_KINDS)
            by_kind[kind] += by_id[i];
    }
    kage_vita_profile_top(by_id, GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS,
                          top_id, top_count, KAGE_VITA_PROFILE_IMPORT_TOP);
    kage_vita_profile_top(by_kind, KAGE_VITA_PROFILE_IMPORT_KINDS,
                          top_kind, top_kind_count,
                          KAGE_VITA_PROFILE_IMPORT_KIND_TOP);
    /* imp: host import calls in the window (all routes) and the subset that
     * took a sync fast path; top: dense import ID:count; kind: binding
     * family (enum isaac_vita_import_binding_kind):count.  Below 384 bytes
     * with every field at UINT32_MAX. */
    KVPP_PRINTF(
        "[kage-vita] ph120.i bid=%.32s win=%u loops=%u "
        "imp(n,sf)=%u,%u "
        "top=%u:%u,%u:%u,%u:%u,%u:%u,%u:%u,%u:%u,%u:%u,%u:%u "
        "kind=%u:%u,%u:%u,%u:%u,%u:%u\n",
        build_id, window, last_outer_loop,
        total, sync_fastpath,
        top_id[0], top_count[0], top_id[1], top_count[1],
        top_id[2], top_count[2], top_id[3], top_count[3],
        top_id[4], top_count[4], top_id[5], top_count[5],
        top_id[6], top_count[6], top_id[7], top_count[7],
        top_kind[0], top_kind_count[0], top_kind[1], top_kind_count[1],
        top_kind[2], top_kind_count[2], top_kind[3], top_kind_count[3]);
}
#endif

#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
static void kage_vita_profile_log_gl_redundancy(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const IsaacVitaGlPhaseProfileCounters *gl)
{
    /* ph120.c counts calls which reached vitaGL.  Adding these suppressed
     * counts reconstructs the corresponding guest request totals. */
    KVPP_PRINTF(
        "[kage-vita] ph120.o bid=%.32s win=%u loops=%u "
        "glskip(p,u)=%u,%u\n",
        build_id, window, last_outer_loop,
        gl->use_program_suppressed, gl->uniform_suppressed);
}
#endif

#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
static void kage_vita_profile_log_gl_typed_state(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const IsaacVitaGlPhaseProfileCounters *gl)
{
    /* One aggregate record: no timer or logger enters a typed GL wrapper.
     * `miss` is a valid first/changed tuple which reached vitaGL; rejects are
     * invalid or deliberately unmodelled requests which also passed through. */
    KVPP_PRINTF(
        "[kage-vita] ph120.y bid=%.32s win=%u loops=%u "
        "hit(a,t,b,d,v,e,p)=%u,%u,%u,%u,%u,%u,%u "
        "miss=%u reject(i,u)=%u,%u"
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
        " coal(d,c,p0,p1)=%u,%u,%u,%u"
#endif
        "\n",
        build_id, window, last_outer_loop,
        gl->typed_state_hit_active_texture,
        gl->typed_state_hit_bind_texture,
        gl->typed_state_hit_blend,
        gl->typed_state_hit_depth,
        gl->typed_state_hit_viewport,
        gl->typed_state_hit_attrib_toggle,
        gl->typed_state_hit_attrib_pointer,
        gl->typed_state_miss,
        gl->typed_state_reject_invalid,
        gl->typed_state_reject_unknown
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
        , gl->attrib_deferred, gl->attrib_cancelled,
        s_window_attrib_pending, gl_vita_backend_attrib_pending()
#endif
        );
}
#endif

#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
static void kage_vita_profile_vitagl_delta(
    vglIsaacGpuDrawStats *delta, const vglIsaacGpuDrawStats *current,
    const vglIsaacGpuDrawStats *prior)
{
    memset(delta, 0, sizeof *delta);
    delta->abi_version = current->abi_version;
    delta->enabled = current->enabled;
#define ISAAC_VITAGL_DELTA(field) \
    delta->field = current->field - prior->field
    ISAAC_VITAGL_DELTA(draw_elements_requests);
    ISAAC_VITAGL_DELTA(coalesce_attempts);
    ISAAC_VITAGL_DELTA(coalesce_hits);
    ISAAC_VITAGL_DELTA(coalesce_reject_count);
    ISAAC_VITAGL_DELTA(coalesce_reject_stream_index);
    ISAAC_VITAGL_DELTA(coalesce_reject_base);
    ISAAC_VITAGL_DELTA(coalesce_reject_descriptor);
    ISAAC_VITAGL_DELTA(stream_slots_before);
    ISAAC_VITAGL_DELTA(stream_slots_after);
    ISAAC_VITAGL_DELTA(vertex_program_requests);
    ISAAC_VITAGL_DELTA(vertex_program_hits);
    ISAAC_VITAGL_DELTA(vertex_program_misses);
    ISAAC_VITAGL_DELTA(vertex_program_stores);
    ISAAC_VITAGL_DELTA(vertex_program_full_fallbacks);
    ISAAC_VITAGL_DELTA(vertex_program_store_failures);
    ISAAC_VITAGL_DELTA(vertex_program_create_failures);
    ISAAC_VITAGL_DELTA(vertex_program_releases);
    ISAAC_VITAGL_DELTA(index_bytes);
    ISAAC_VITAGL_DELTA(vertex_bytes);
    ISAAC_VITAGL_DELTA(index_count);
    ISAAC_VITAGL_DELTA(vertex_count);
    /* A high-water mark is a lifetime snapshot; subtracting it would turn a
     * quiet window into a false zero/underflow rather than preserve its bound. */
    delta->top_idx_high_water = current->top_idx_high_water;
    ISAAC_VITAGL_DELTA(top_idx_unknown);
    ISAAC_VITAGL_DELTA(vertex_set_hits);
    ISAAC_VITAGL_DELTA(vertex_set_misses);
    ISAAC_VITAGL_DELTA(fragment_set_hits);
    ISAAC_VITAGL_DELTA(fragment_set_misses);
    ISAAC_VITAGL_DELTA(texture_set_hits);
    ISAAC_VITAGL_DELTA(texture_set_misses);
#undef ISAAC_VITAGL_DELTA
}

static void kage_vita_profile_log_vitagl_draw(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const vglIsaacGpuDrawStats *stats)
{
    KVPP_PRINTF(
        "[kage-vita] ph120.v bid=%.32s win=%u loops=%u abi=%u en=%u "
        "draw=%u coal=%u/%u rej=%u/%u/%u/%u streams=%u/%u "
        "vp=%u/%u/%u/%u/%u/%u/%u rel=%u\n",
        build_id, window, last_outer_loop,
        stats->abi_version, stats->enabled,
        stats->draw_elements_requests,
        stats->coalesce_hits, stats->coalesce_attempts,
        stats->coalesce_reject_count,
        stats->coalesce_reject_stream_index,
        stats->coalesce_reject_base,
        stats->coalesce_reject_descriptor,
        stats->stream_slots_before, stats->stream_slots_after,
        stats->vertex_program_requests,
        stats->vertex_program_hits, stats->vertex_program_misses,
        stats->vertex_program_stores,
        stats->vertex_program_full_fallbacks,
        stats->vertex_program_store_failures,
        stats->vertex_program_create_failures,
        stats->vertex_program_releases);
}

static void kage_vita_profile_log_draw_submission(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const IsaacVitaGlPhaseProfileCounters *gl,
    const vglIsaacGpuDrawStats *stats)
{
    KVPP_PRINTF(
        "[kage-vita] ph120.q bid=%.32s win=%u loops=%u "
        "q(h,s,x,b,p,d,save)=%u,%u,%u,%u,%u,%u,%u "
        "io(i,v,n,vc,top,u)=%u,%u,%u,%u,%u,%u "
        "set(v,f,t)=%u/%u,%u/%u,%u/%u\n",
        build_id, window, last_outer_loop,
        gl->canonical_quad_hits,
        gl->canonical_quad_reject_callsite,
        gl->canonical_quad_reject_shape,
        gl->canonical_quad_reject_bounds,
        gl->canonical_quad_reject_pointer,
        gl->canonical_quad_driver_fallback,
        gl->canonical_quad_index_bytes_saved,
        stats->index_bytes, stats->vertex_bytes, stats->index_count,
        stats->vertex_count, stats->top_idx_high_water,
        stats->top_idx_unknown,
        stats->vertex_set_hits, stats->vertex_set_misses,
        stats->fragment_set_hits, stats->fragment_set_misses,
        stats->texture_set_hits, stats->texture_set_misses);
}

static void kage_vita_profile_log_fusion_ceiling(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const IsaacVitaGlPhaseProfileCounters *gl)
{
    KVPP_PRINTF(
        "[kage-vita] ph120.f bid=%.32s win=%u loops=%u "
        "idx=%u blend(a,n,o)=%u,%u,%u shader(s,c)=%u,%u "
        "fbo(d,o)=%u,%u atlas(s,c)=%u,%u add(d,r)=%u,%u\n",
        build_id, window, last_outer_loop, gl->fusion_index_bytes,
        gl->fusion_blend_additive, gl->fusion_blend_alpha,
        gl->fusion_blend_other, gl->fusion_shader_same,
        gl->fusion_shader_change, gl->fusion_fbo_default,
        gl->fusion_fbo_offscreen, gl->fusion_atlas_same,
        gl->fusion_atlas_change, gl->fusion_additive_draws,
        gl->fusion_additive_runs);
}
#endif

#if defined(ISAAC_VITA_GL_FILL_CENSUS)
/* GL fill census (gl_vita_backend.h legend).  Two records so each stays
 * under the 384-byte isaac_vita_log body at all-ones: fa = window totals and
 * classes (kpx = kilo-pixels of projected triangle area: u unclipped, c
 * clipped to the viewport, clr native clears; prog e/o = coloroffset
 * layout / other; blend a/d/o as ph120.f; vp = current viewport, n changes,
 * miss unmeasured draws), fp = per pass ordinal p0..p3 (3 = 3+) and the
 * display pass (draws, clipped kpx, clears), the last offscreen attachment
 * size, the 32x32 tile sum and cunk = native clears of an unknown-size
 * target (0 kpx in clr).  All-ones: fa 381, fp 381 bytes. */
static void kage_vita_profile_log_fill_census(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const IsaacVitaGlFillCensus *fill)
{
    KVPP_PRINTF(
        "[kage-vita] ph120.fa bid=%.32s win=%u loops=%u "
        "draws=%u tri=%u kpx(u,c,clr)=%u,%u,%u max=%u "
        "big=%u bad=%u syn=%u proj=%u prog(e,o)=%u,%u "
        "blend(a,d,o)=%u,%u,%u vp(w,h,n,miss)=%u,%u,%u,%u\n",
        build_id, window, last_outer_loop, fill->draws, fill->triangles,
        fill->kpx_unclipped, fill->kpx_clipped, fill->kpx_clear,
        fill->max_draw_kpx, fill->big, fill->bad, fill->synthesized,
        fill->projective, fill->prog_kpx[0], fill->prog_kpx[1],
        fill->blend_kpx[0], fill->blend_kpx[1], fill->blend_kpx[2],
        fill->viewport_width, fill->viewport_height,
        fill->viewport_changes, fill->miss);
    KVPP_PRINTF(
        "[kage-vita] ph120.fp bid=%.32s win=%u loops=%u "
        "p0(d,kpx,c)=%u,%u,%u p1(d,kpx,c)=%u,%u,%u p2(d,kpx,c)=%u,%u,%u "
        "p3(d,kpx,c)=%u,%u,%u disp(d,kpx,c)=%u,%u,%u att(w,h)=%u,%u "
        "tiles=%u cunk=%u\n",
        build_id, window, last_outer_loop,
        fill->pass_draws[0], fill->pass_kpx[0], fill->pass_clears[0],
        fill->pass_draws[1], fill->pass_kpx[1], fill->pass_clears[1],
        fill->pass_draws[2], fill->pass_kpx[2], fill->pass_clears[2],
        fill->pass_draws[3], fill->pass_kpx[3], fill->pass_clears[3],
        fill->pass_draws[4], fill->pass_kpx[4], fill->pass_clears[4],
        fill->attachment_width, fill->attachment_height, fill->tiles,
        fill->clear_unknown);
}
#endif

#if defined(ISAAC_VITA_FBO_CLEAR_ELISION) ||         defined(ISAAC_VITA_FBO_RASTER_SCALE)
/* Offscreen render-target policies: clear(x,r,p) = guest glClear requests
 * absorbed at the call, owed depth/stencil clears issued as their own native
 * glClear, and permanent scissor poisons; raster(t,v,s) = colour targets
 * allocated at the reduced raster, native viewports rewritten for a scaled
 * draw target, and scaled textures re-specified at full size by a guest
 * sub-image.  Native clears per window = gl(c) in ph120.c = guest clears - x
 * + r; every absorbed clear that emptied an offscreen pass also removes one
 * GXM scene switch. */
static void kage_vita_profile_log_fbo_policy(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const IsaacVitaGlPhaseProfileCounters *gl)
{
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION_DEPTH_DROP)
    /* Depth-drop sub-mode: colour clears are always native, and the owed
     * depth/stencil clear is settled one of three ways: replayed before a
     * draw or an unmodelled call (r), dropped at a tracked colour attach of
     * the owing framebuffer (a; stock vitaGL ends that scene at the next
     * clear or draw, FBO depth is never stored) or dropped because stock
     * vitaGL ends the owing scene (d: clear/draw on another framebuffer,
     * present, framebuffer deletion).  x - (r + a + d) is owed clears folded
     * into a later native clear of the same target or re-requested before
     * settlement, so on the game's fixed frame x == r + a + d. */
    KVPP_PRINTF(
        "[kage-vita] ph120.e bid=%.32s win=%u loops=%u "
        "clear(x,r,p,a,d)=%u,%u,%u,%u,%u raster(t,v,s)=%u,%u,%u\n",
        build_id, window, last_outer_loop,
        gl->clear_suppressed, gl->clear_replayed, gl->fbo_elision_poison,
        gl->clear_dropped_attach, gl->clear_dropped_scene,
        gl->fbo_raster_textures, gl->fbo_raster_viewports,
        gl->fbo_raster_respecified);
#else
    KVPP_PRINTF(
        "[kage-vita] ph120.e bid=%.32s win=%u loops=%u "
        "clear(x,r,p)=%u,%u,%u raster(t,v,s)=%u,%u,%u\n",
        build_id, window, last_outer_loop,
        gl->clear_suppressed, gl->clear_replayed, gl->fbo_elision_poison,
        gl->fbo_raster_textures, gl->fbo_raster_viewports,
        gl->fbo_raster_respecified);
#endif
}
#endif

#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
static void kage_vita_profile_coloroffset_delta(
    vglIsaacColorOffsetStats *delta,
    const vglIsaacColorOffsetStats *current,
    const vglIsaacColorOffsetStats *prior)
{
    memset(delta, 0, sizeof *delta);
    delta->abi_version = current->abi_version;
    delta->enabled = current->enabled;
#define ISAAC_COLOROFFSET_DELTA(field) \
    delta->field = current->field - prior->field
    ISAAC_COLOROFFSET_DELTA(source_selected);
    ISAAC_COLOROFFSET_DELTA(source_mark_rejected);
    ISAAC_COLOROFFSET_DELTA(compile_successes);
    ISAAC_COLOROFFSET_DELTA(compile_failures);
    ISAAC_COLOROFFSET_DELTA(exact_draw_requests);
    ISAAC_COLOROFFSET_DELTA(opaque_eligible);
    ISAAC_COLOROFFSET_DELTA(opaque_hits);
    ISAAC_COLOROFFSET_DELTA(opaque_viewport_pixels);
    ISAAC_COLOROFFSET_DELTA(fail_program);
    ISAAC_COLOROFFSET_DELTA(fail_blend);
    ISAAC_COLOROFFSET_DELTA(fail_sampler);
    ISAAC_COLOROFFSET_DELTA(fail_layout);
    ISAAC_COLOROFFSET_DELTA(fail_vertex_range);
    ISAAC_COLOROFFSET_DELTA(fail_vertex_value);
    ISAAC_COLOROFFSET_DELTA(fail_output);
    ISAAC_COLOROFFSET_DELTA(fail_cache);
    ISAAC_COLOROFFSET_DELTA(cache_hits);
    ISAAC_COLOROFFSET_DELTA(cache_creates);
    ISAAC_COLOROFFSET_DELTA(cache_releases);
    ISAAC_COLOROFFSET_DELTA(static_eligible);
    ISAAC_COLOROFFSET_DELTA(static_hits);
    ISAAC_COLOROFFSET_DELTA(static_pixels);
#undef ISAAC_COLOROFFSET_DELTA
}

static void kage_vita_profile_log_coloroffset(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const vglIsaacColorOffsetStats *stats)
{
    KVPP_PRINTF(
        "[kage-vita] ph120.k bid=%.32s win=%u loops=%u abi=%u en=%u "
        "src=%u/%u/%u/%u opq=%u/%u/%u/%u "
        "fail=%u/%u/%u/%u/%u/%u/%u/%u cache=%u/%u/%u "
        "sta=%u/%u/%u\n",
        build_id, window, last_outer_loop,
        stats->abi_version, stats->enabled,
        stats->source_selected, stats->source_mark_rejected,
        stats->compile_successes, stats->compile_failures,
        stats->exact_draw_requests, stats->opaque_eligible,
        stats->opaque_hits, stats->opaque_viewport_pixels,
        stats->fail_program, stats->fail_blend, stats->fail_sampler,
        stats->fail_layout, stats->fail_vertex_range,
        stats->fail_vertex_value, stats->fail_output, stats->fail_cache,
        stats->cache_hits, stats->cache_creates, stats->cache_releases,
        stats->static_eligible, stats->static_hits, stats->static_pixels);
}
#endif

#if defined(ISAAC_VITA_COLOROFFSET_FS_PROBE)
static void kage_vita_profile_coloroffset_fs_probe_delta(
    vglIsaacColorOffsetFsProbeStats *delta,
    const vglIsaacColorOffsetFsProbeStats *current,
    const vglIsaacColorOffsetFsProbeStats *prior)
{
    /* The link fields are process state (one link per exact program); only
     * the per-draw and cache counters become this window's deltas. */
    *delta = *current;
#define ISAAC_FS_PROBE_DELTA(field) \
    delta->field = current->field - prior->field
    ISAAC_FS_PROBE_DELTA(draw_requests);
    ISAAC_FS_PROBE_DELTA(draws_bound);
    ISAAC_FS_PROBE_DELTA(draws_opaque_overridden);
    ISAAC_FS_PROBE_DELTA(fail_cache);
    ISAAC_FS_PROBE_DELTA(fail_create);
    ISAAC_FS_PROBE_DELTA(cache_creates);
    ISAAC_FS_PROBE_DELTA(cache_releases);
#undef ISAAC_FS_PROBE_DELTA
}

/* ph120.kp (DIAGNOSTIC builds only): probe mode, link state (attempts, ready;
 * failures by reason: source, compiler, compile, check, register), the GL
 * program handle of the last link with its receipts (GXP size/FNV-1a, variant
 * source size/FNV-1a), then this window's exact draws that consulted the
 * probe, bound it and would otherwise have used the 0003 opaque variant, and
 * the probe fragment-program cache (creates, releases, full, create failed).
 * A session proves which fragment shader ran: mode plus link(a,r)=n,n and
 * draw(q,b,..)=m,m with m > 0. Optional plain(q,b,f) is its subset; main
 * gxp/src still identify neutral, not plain. Optional pair(q,b,f) proves
 * private VS+FS selection versus P2 fallback. Optional half(q,b) proves
 * half-fragment selection; q-b is its fallback count. Even both optional
 * suffixes fit: worst case 501 <512 bytes (the durable formatter); no new
 * physical log record is emitted. */
static void kage_vita_profile_take_plain(uint32_t plain[3])
{
    memset(plain, 0, 3u * sizeof(uint32_t));
#if defined(__GNUC__)
    if (vglTakeIsaacColorOffsetPlainStats &&
        vglTakeIsaacColorOffsetPlainStats(plain, 3u) != 1u)
        memset(plain, 0, 3u * sizeof(uint32_t));
#elif defined(ISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE)
    if (vglTakeIsaacColorOffsetPlainStats(plain, 3u) != 1u)
        memset(plain, 0, 3u * sizeof(uint32_t));
#endif
}

static int kage_vita_profile_take_plain_vertex_pair(uint32_t pair[3])
{
    memset(pair, 0, 3u * sizeof(uint32_t));
#if defined(__GNUC__)
    if (vglTakeIsaacColorOffsetPlainVertexPairStats)
        return vglTakeIsaacColorOffsetPlainVertexPairStats(pair, 3u) == 1u;
#elif defined(ISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE)
    return vglTakeIsaacColorOffsetPlainVertexPairStats(pair, 3u) == 1u;
#endif
    return 0;
}

static int kage_vita_profile_take_plain_fp16(uint32_t half[2])
{
    memset(half, 0, 2u * sizeof(uint32_t));
#if defined(__GNUC__)
    if (vglTakeIsaacColorOffsetPlainFp16Stats)
        return vglTakeIsaacColorOffsetPlainFp16Stats(half, 2u) == 1u;
#elif defined(ISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE)
    return vglTakeIsaacColorOffsetPlainFp16Stats(half, 2u) == 1u;
#endif
    return 0;
}

static void kage_vita_profile_log_coloroffset_fs_probe(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const vglIsaacColorOffsetFsProbeStats *stats, const uint32_t plain[3],
    const uint32_t pair[3], const uint32_t half[2])
{
    char pair_text[sizeof " pair(q,b,f)=4294967295,4294967295,4294967295"] = "";
    char half_text[sizeof " half(q,b)=4294967295,4294967295"] = "";
    if (pair)
        snprintf(pair_text, sizeof pair_text, " pair(q,b,f)=%u,%u,%u",
                 pair[0], pair[1], pair[2]);
    if (half)
        snprintf(half_text, sizeof half_text, " half(q,b)=%u,%u",
                 half[0], half[1]);
    KVPP_PRINTF(
        "[kage-vita] ph120.kp bid=%.32s win=%u loops=%u mode=%u "
        "link(a,r)=%u,%u lfail(s,c,x,k,r)=%u,%u,%u,%u,%u prog=%u "
        "gxp=%u/%08x src=%u/%08x draw(q,b,o)=%u,%u,%u "
        "cache(c,r,f,x)=%u,%u,%u,%u plain(q,b,f)=%u,%u,%u%s%s\n",
        build_id, window, last_outer_loop, stats->mode,
        stats->link_attempts, stats->link_ready,
        stats->fail_source, stats->fail_compiler, stats->fail_compile,
        stats->fail_check, stats->fail_register, stats->last_program,
        stats->gxp_size, stats->gxp_fnv1a, stats->source_size,
        stats->source_fnv1a, stats->draw_requests, stats->draws_bound,
        stats->draws_opaque_overridden, stats->cache_creates,
        stats->cache_releases, stats->fail_cache, stats->fail_create,
        plain[0], plain[1], plain[2], pair_text, half_text);
}
#endif

#if defined(ISAAC_VITA_PILL_BLOOM_BYPASS)
static void kage_vita_profile_log_pill_bloom(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    uint32_t capture_skips, uint32_t composite_skips)
{
    /* `bad` is an oracle, not a recovery path.  The generated function-local
     * token prevents an unmatched target pop; a non-zero value means control
     * left Game::Render between the two exact seams and needs investigation. */
    KVPP_PRINTF(
        "[kage-vita] ph120.b bid=%.32s win=%u loops=%u "
        "bloom(skip c,o)=%u,%u bad=%u\n",
        build_id, window, last_outer_loop,
        capture_skips, composite_skips,
        capture_skips != composite_skips ? 1u : 0u);
}
#endif

#if defined(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES)
static void kage_vita_profile_log_bloom_half(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    uint32_t create_initial, uint32_t create_applied,
    uint32_t create_rejected,
    uint32_t captures, uint32_t composites)
{
    /* Before the resize request reaches Game::Render, i=1/a=0 is an
     * intentionally incomplete lifecycle and therefore reports bad=1. */
    uint32_t bad = create_initial != 1u || create_applied != 1u ||
        create_rejected != 0u || captures != composites;

    KVPP_PRINTF(
        "[kage-vita] ph120.h bid=%.32s win=%u loops=%u "
        "half(create i,a,r)=%u,%u,%u bloom(c,o)=%u,%u bad=%u\n",
        build_id, window, last_outer_loop,
        create_initial, create_applied, create_rejected,
        captures, composites,
        bad != 0u ? 1u : 0u);
}
#endif

#if defined(ISAAC_VITA_POOP_FX_PROFILE)
static void kage_vita_profile_log_poop_fx(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const KageVitaPoopFxProfileStats *poop_fx)
{
    uint32_t countdown_min = poop_fx->active != 0u ?
        poop_fx->countdown_min : 0u;
    uint32_t bad = poop_fx->bad;

    bad |= poop_fx->clouds != poop_fx->active * 3u;
    bad |= poop_fx->cap_frames > poop_fx->active;
    bad |= poop_fx->cap_frames > UINT32_MAX / 2u;
    if (poop_fx->cap_frames <= UINT32_MAX / 2u)
        bad |= poop_fx->cap_skipped != poop_fx->cap_frames * 2u;
    KVPP_PRINTF(
        "[kage-vita] ph120.p bid=%.32s win=%u loops=%u abi=1 "
        "add(p,b,o,u)=%u,%u,%u,%u active(n,clouds)=%u,%u "
        "cd(min,max,last)=%u,%u,%u cap(n,skip)=%u,%u bad=%u\n",
        build_id, window, last_outer_loop,
        poop_fx->add_pill, poop_fx->add_black,
        poop_fx->add_other, poop_fx->add_unknown,
        poop_fx->active, poop_fx->clouds,
        countdown_min, poop_fx->countdown_max, poop_fx->countdown_last,
        poop_fx->cap_frames, poop_fx->cap_skipped, bad != 0u ? 1u : 0u);
}
#endif

#if defined(ISAAC_VITA_LASER_PROFILE)
static void kage_vita_profile_log_laser(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const KageVitaLaserProfileStats *laser)
{
    uint32_t bad = laser->bad;

    bad |= laser->render !=
        laser->sample_zero + laser->sample_positive;
    bad |= laser->render !=
        laser->source_1 + laser->source_10_999 + laser->source_other;
    bad |= laser->render != laser->variant_0 + laser->variant_1 +
        laser->variant_2 + laser->variant_other;
    bad |= laser->render != laser->subtype_1_3 + laser->subtype_other;
    KVPP_PRINTF(
        "[kage-vita] ph120.l bid=%.32s win=%u loops=%u "
        "laser(r,h,z,p)=%u,%u,%u,%u src(1,10_999,o)=%u,%u,%u "
        "var(0,1,2,o)=%u,%u,%u,%u sub(1_3,o)=%u,%u bad=%u\n",
        build_id, window, last_outer_loop,
        laser->render, laser->shadow,
        laser->sample_zero, laser->sample_positive,
        laser->source_1, laser->source_10_999, laser->source_other,
        laser->variant_0, laser->variant_1,
        laser->variant_2, laser->variant_other,
        laser->subtype_1_3, laser->subtype_other, bad != 0u ? 1u : 0u);
}

static uint32_t kage_vita_profile_laser_cost_bad(
    const KageVitaLaserProfileStats *laser)
{
    uint32_t bad = laser->bad;

    bad |= laser->cost[KAGE_VITA_LASER_COST_SAMPLE_ZERO].completed !=
        laser->sample_zero;
    bad |= laser->cost[KAGE_VITA_LASER_COST_SAMPLE_POSITIVE].completed !=
        laser->sample_positive;
    bad |= laser->cost[KAGE_VITA_LASER_COST_SHADOW].completed !=
        laser->shadow;
    bad |= laser->cost_other != 0u;
    return bad != 0u ? 1u : 0u;
}

static void kage_vita_profile_log_laser_cost(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const KageVitaLaserProfileStats *laser)
{
    const KageVitaLaserCostBucket *zero =
        &laser->cost[KAGE_VITA_LASER_COST_SAMPLE_ZERO];
    const KageVitaLaserCostBucket *positive =
        &laser->cost[KAGE_VITA_LASER_COST_SAMPLE_POSITIVE];
    const KageVitaLaserCostBucket *shadow =
        &laser->cost[KAGE_VITA_LASER_COST_SHADOW];

    KVPP_PRINTF(
        "[kage-vita] ph120.m bid=%.32s win=%u loops=%u "
        "cost(n/us/mx) z=%u/%u/%u p=%u/%u/%u h=%u/%u/%u other=%u "
        "seq(ov,mm,dg,clk)=%u,%u,%u,%u bad=%u\n",
        build_id, window, last_outer_loop,
        zero->completed, zero->total_us, zero->max_us,
        positive->completed, positive->total_us, positive->max_us,
        shadow->completed, shadow->total_us, shadow->max_us,
        laser->cost_other, laser->active_overflow, laser->end_mismatch,
        laser->dangling, laser->clock_reset,
        kage_vita_profile_laser_cost_bad(laser));
}

static void kage_vita_profile_log_laser_submission(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const KageVitaLaserProfileStats *laser)
{
    const KageVitaLaserCostBucket *zero =
        &laser->cost[KAGE_VITA_LASER_COST_SAMPLE_ZERO];
    const KageVitaLaserCostBucket *positive =
        &laser->cost[KAGE_VITA_LASER_COST_SAMPLE_POSITIVE];
    const KageVitaLaserCostBucket *shadow =
        &laser->cost[KAGE_VITA_LASER_COST_SHADOW];
#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
    const uint32_t enabled = 1u;
#else
    const uint32_t enabled = 0u;
#endif

    KVPP_PRINTF(
        "[kage-vita] ph120.n bid=%.32s win=%u loops=%u "
        "submit(en,ok,ng)=%u,%u,%u draw(z,p,h)=%u,%u,%u "
        "idx(z,p,h)=%u,%u,%u vtx(z,p,h)=%u,%u,%u bad=%u\n",
        build_id, window, last_outer_loop,
        enabled, laser->submission_samples, laser->submission_unavailable,
        zero->draw_requests, positive->draw_requests,
        shadow->draw_requests,
        zero->index_count, positive->index_count, shadow->index_count,
        zero->vertex_count, positive->vertex_count, shadow->vertex_count,
        kage_vita_profile_laser_cost_bad(laser));
}
#endif

#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
static void kage_vita_profile_log_texture_churn(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const IsaacVitaTextureChurnProfile *texture)
{
    /* g=calls/names/ambiguous/observed-us/accepted-scan-slots; d=delete names;
     * i=first/redefine/unknown/other-level call history;
     * p=linear/converted-or-compacted/other; px/b cover only known paths;
     * us=sum/max image bridge intervals; cal is the maximum of eight
     * install-time back-to-back clock pairs; live is logical-now/window-peak/
     * recycled.  `us` fields include the exact `clk` process-clock calls and
     * any other enabled bridge probes.  For pinned vitaGL 73dd57a and accepted
     * frozen n=1 results, scan is the exact texture_slots count.  Neither
     * timing is an isolated native CPU-cycle measurement.  If amb is nonzero,
     * lifecycle and first/redefine/recycle attribution after it are incomplete
     * lower bounds and later windows retain bad until a full backend reset.  In
     * a healthy unsaturated record, clk equals twice g.calls plus twice the sum
     * of the four i buckets. */
    KVPP_PRINTF(
        "[kage-vita] ph120.x bid=%.32s win=%u loops=%u "
        "g=%u/%u/%u/%u/%u d=%u i=%u/%u/%u/%u p=%u/%u/%u "
        "px/b=%u/%u us=%u/%u cal=%u live=%u/%u/%u "
        "bad/clk=%u/%u\n",
        build_id, window, last_outer_loop,
        texture->gen_calls, texture->gen_names, texture->gen_ambiguous,
        texture->gen_observed_us, texture->gen_scan_slots,
        texture->delete_names,
        texture->image_first, texture->image_redefine,
        texture->image_unknown, texture->image_other_level,
        texture->path_linear, texture->path_converted,
        texture->path_other, texture->known_pixels,
        texture->known_alloc_bytes,
        texture->image_observed_us, texture->image_max_observed_us,
        texture->clock_pair_max_us,
        texture->logical_live, texture->window_peak_live,
        texture->recycled_names, texture->bad, texture->clock_calls);
}

static void kage_vita_profile_log_texture_delete(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const IsaacVitaTextureChurnProfile *texture)
{
    /* del=calls/names; native and post are sum/max process-clock intervals.
     * Native brackets only glDeleteTextures.  Post brackets only lifecycle
     * bookkeeping.  Each observed call contributes four entries to the shared
     * full texture-profile bad/clk integrity fields repeated here. */
    KVPP_PRINTF(
        "[kage-vita] ph120.xd bid=%.32s win=%u loops=%u "
        "del=%u/%u native-us=%u/%u post-us=%u/%u bad/clk=%u/%u\n",
        build_id, window, last_outer_loop,
        texture->delete_calls, texture->delete_names,
        texture->delete_native_observed_us,
        texture->delete_native_max_observed_us,
        texture->delete_post_observed_us,
        texture->delete_post_max_observed_us,
        texture->bad, texture->clock_calls);
}
#endif

#if defined(ISAAC_VITA_KAGE_QUAD_FASTPATH)
/* ph120.kq: the Image::PushQuad host replay's verdicts for the window
 * (ISAAC_VITA_KAGE_QUAD_FASTPATH; take-and-zero at the window boundary,
 * discarded once at the first loop head).  mode = 1 replay ON, 2 OBSERVE
 * (checks only, the translated body runs); n = `_try` entries; h = every
 * check passed (OBSERVE: would have been replayed); c(x,k,g,s) = handled
 * quads that took the cull exit / were culled against the constant 480x270
 * / called the GetWidth+GetHeight getters / took the pixel-snap path (four
 * floor imports; OBSERVE: g and s are the candidates before the cull);
 * p(b,t,f,r,d) = handled quads without the batched flag that entered the
 * translated blend/batch select / of those with alpha sum < 4 (bool 1) /
 * ring records met with no buffer and capacity 0 / ring reserves delegated
 * to the translated Ring::Alloc (growth) / dirty-vector pushes delegated to
 * the translated growth (OBSERVE: b and t are the candidates, f/r/d the
 * entry snapshot of batched images); d(f,oa,ob,on,c,a,s,g,t) = declines by
 * reason: stack frame, null this/quad/formats/uv/colour argument, null
 * batch or ring records, null ring buffer the x86 stores through, attribute
 * count > 16, attribute format outside 1..8, format sizes not summing to
 * the stride, floor import route not authenticated, target vtable/getter
 * slot null. */
static void kage_vita_profile_log_kage_quad(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const IsaacVitaKageQuadCounters *k)
{
    KVPP_PRINTF(
        "[kage-vita] ph120.kq bid=%.32s win=%u loops=%u mode=%u n=%u h=%u "
        "c(x,k,g,s)=%u,%u,%u,%u p(b,t,f,r,d)=%u,%u,%u,%u,%u "
        "d(f,oa,ob,on,c,a,s,g,t)=%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
        build_id, window, last_outer_loop, k->mode, k->calls, k->handled,
        k->culled, k->constant_cull, k->getters, k->snapped,
        k->unbatched, k->translucent, k->fresh_records, k->ring_growths,
        k->dirty_growths,
        k->declined[0], k->declined[1], k->declined[2], k->declined[3],
        k->declined[4], k->declined[5], k->declined[6], k->declined[7],
        k->declined[8]);
}
#endif

#if defined(ISAAC_VITA_HEAP_CENSUS)
/* ph120.mem: the per-window guest-heap receipt (ISAAC_VITA_HEAP_CENSUS).
 * mi(a,u,f,n,top) is newlib mallinfo (arena, uordblks, fordblks, ordblks,
 * keepcost = top chunk); hr = headroom = 81 MiB contract - uordblks (the
 * arena grows lazily, so fordblks alone under-reports); la = largest
 * allocatable bound = keepcost + (contract - arena); led(n,b) = live ledger
 * entries and their requested bytes; ov(l,b,nf,pf,rem) = overflow-mspace
 * owned live count/bytes, native failures, pool failures and the remaining
 * requested-byte bound (capacity - owned - internal pages; mspace overhead
 * excluded); slab(p,l,b) = roomslab pages/live entries/page bytes;
 * lua(l,pk,fb) = Lua arena live KiB/peak KiB/guest-heap fallbacks; vgl(f,t)
 * = vitaGL RAM pool free/total; slot = OGG emergency slot live count; ok =
 * every accounting source consistent and mallinfo taken; why = win for the
 * cadence record, badalloc/exit for the entry owner's final records. */
typedef struct kage_vita_heap_record {
    kage_vita_heap_mallinfo mi;
    uint32_t headroom;
    uint32_t largest_bound;
    uint32_t led_count;
    uint32_t led_bytes;
    uint32_t ov_live;
    uint32_t ov_bytes;
    uint32_t ov_native_failures;
    uint32_t ov_pool_failures;
    uint32_t ov_remaining;
    uint32_t slab_pages;
    uint32_t slab_live;
    uint32_t slab_bytes;
    uint32_t lua_live_kb;
    uint32_t lua_peak_kb;
    uint32_t lua_fallbacks;
    uint32_t vgl_free;
    uint32_t vgl_total;
    uint32_t slot_live;
    uint32_t ok;
} kage_vita_heap_record;

static uint32_t kage_vita_heap_sub_sat(uint32_t a, uint32_t b)
{
    return a > b ? a - b : 0u;
}

static uint32_t kage_vita_heap_add_sat(uint32_t a, uint32_t b)
{
    return a > UINT32_MAX - b ? UINT32_MAX : a + b;
}

static void kage_vita_profile_heap_mallinfo(kage_vita_heap_mallinfo *out)
{
# if defined(ISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE)
    kage_vita_phase_profile_oracle_mallinfo(out);
# else
    struct mallinfo heap = mallinfo();

    out->arena = (uint32_t)heap.arena;
    out->ordblks = (uint32_t)heap.ordblks;
    out->uordblks = (uint32_t)heap.uordblks;
    out->fordblks = (uint32_t)heap.fordblks;
    out->keepcost = (uint32_t)heap.keepcost;
# endif
}

static void kage_vita_profile_heap_collect(kage_vita_heap_record *record)
{
    isaac_vita_guest_heap_census_locked census;
    uint64_t slab_bytes;

    memset(record, 0, sizeof *record);
    /* One heap-lock section first (terminal flag included), then mallinfo
     * outside it: mallinfo takes newlib's own lock and walks the free bins,
     * and a terminal overflow domain means newlib may be inconsistent. */
    record->ok = isaac_vita_guest_heap_census_window_locked_get(&census)
        ? 1u : 0u;
    if (census.terminal)
        record->ok = 0u;
    else
        kage_vita_profile_heap_mallinfo(&record->mi);
    record->headroom = kage_vita_heap_sub_sat(
        census.heap_total_bytes, record->mi.uordblks);
    record->largest_bound = kage_vita_heap_add_sat(
        record->mi.keepcost,
        kage_vita_heap_sub_sat(census.heap_total_bytes, record->mi.arena));
    record->led_count = census.ledger_live_count;
    record->led_bytes = census.ledger_live_requested_bytes;
    record->ov_live = census.overflow_live_count;
    record->ov_bytes = census.overflow_live_requested_bytes;
    record->ov_native_failures = census.overflow_native_failures;
    record->ov_pool_failures = census.overflow_pool_failures;
    record->ov_remaining = kage_vita_heap_sub_sat(
        kage_vita_heap_sub_sat(census.overflow_capacity_bytes,
                               census.overflow_live_requested_bytes),
        census.overflow_internal_requested_bytes);
    record->slab_pages = census.slab_pages;
    record->slab_live = census.slab_live_slots;
    slab_bytes = (uint64_t)census.slab_pages * census.slab_page_bytes;
    record->slab_bytes = slab_bytes > UINT32_MAX
        ? UINT32_MAX : (uint32_t)slab_bytes;
# if defined(ISAAC_VITA_HEAP_CENSUS_LUA)
    isaac_vita_lua_arena_stats(&record->lua_live_kb, &record->lua_peak_kb,
                               &record->lua_fallbacks);
# endif
# if defined(ISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE)
    kage_vita_phase_profile_oracle_vgl_ram(&record->vgl_free,
                                           &record->vgl_total);
# else
    record->vgl_free = (uint32_t)vglMemFree(VGL_MEM_RAM);
    record->vgl_total = (uint32_t)vglMemTotal(VGL_MEM_RAM);
# endif
    record->slot_live = census.ogg_slot_live;
}

static void kage_vita_profile_log_heap(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const kage_vita_heap_record *record, const char *why)
{
    KVPP_PRINTF(
        "[kage-vita] ph120.mem bid=%.32s win=%u loops=%u "
        "mi(a,u,f,n,top)=%u,%u,%u,%u,%u hr=%u la=%u led(n,b)=%u,%u "
        "ov(l,b,nf,pf,rem)=%u,%u,%u,%u,%u slab(p,l,b)=%u,%u,%u "
        "lua(l,pk,fb)=%u,%u,%u vgl(f,t)=%u,%u slot=%u ok=%u why=%.8s\n",
        build_id, window, last_outer_loop,
        record->mi.arena, record->mi.uordblks, record->mi.fordblks,
        record->mi.ordblks, record->mi.keepcost,
        record->headroom, record->largest_bound,
        record->led_count, record->led_bytes,
        record->ov_live, record->ov_bytes, record->ov_native_failures,
        record->ov_pool_failures, record->ov_remaining,
        record->slab_pages, record->slab_live, record->slab_bytes,
        record->lua_live_kb, record->lua_peak_kb, record->lua_fallbacks,
        record->vgl_free, record->vgl_total, record->slot_live,
        record->ok, why);
}

static void kage_vita_profile_heap_report(
    const char *build_id, uint32_t window, uint32_t last_outer_loop,
    const char *why)
{
    kage_vita_heap_record record;

    kage_vita_profile_heap_collect(&record);
    kage_vita_profile_log_heap(build_id, window, last_outer_loop,
                               &record, why);
}
#endif

#if defined(ISAAC_VITA_IMAGE_RETAIN)
/* ph120.ir: per-window deltas of the retention counters (off=candidates,
 * new=retained, reuse=Load hits re-offered, inel=ineligible, pass=passthrough,
 * ev=evicted/bytes/budget/headroom/table_full, drop=stale) plus the
 * instantaneous live=records/bytes, the knobs and vglMemFree RAM/VRAM. */
static isaac_ir_window s_image_retain_prev;
static void kage_vita_profile_log_image_retain(
    const char *build_id, uint32_t window, uint32_t last_outer_loop)
{
    isaac_ir_window now;
    const isaac_ir_stats *a = &now.totals;
    const isaac_ir_stats *b = &s_image_retain_prev.totals;

    isaac_vita_image_retain_window(&now);
    KVPP_PRINTF(
        "[kage-vita] ph120.ir bid=%.32s win=%u loops=%u "
        "off=%u new=%u reuse=%u inel=%u pass=%u ev=%u/%u/%u/%u/%u drop=%u "
        "live=%u/%u budget=%u head=%u vf(r/v)=%u/%u armed=%u\n",
        build_id, window, last_outer_loop,
        a->offered - b->offered, a->retained_new - b->retained_new,
        a->reuse - b->reuse, a->ineligible - b->ineligible,
        a->passthrough - b->passthrough,
        a->evicted - b->evicted, a->evicted_bytes - b->evicted_bytes,
        a->budget_evict - b->budget_evict,
        a->headroom_evict - b->headroom_evict,
        a->table_full - b->table_full,
        a->dropped_stale - b->dropped_stale,
        now.live_records, a->retained_bytes,
        now.budget_bytes, now.headroom_bytes, now.vf_ram, now.vf_vram,
        now.armed);
    s_image_retain_prev = now;
}
#endif

static void kage_vita_profile_report(
    uint32_t last_outer_loop, uint64_t report_at_us,
    const GuestPhaseProfileCounters *guest,
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
    uint32_t lookup_cache_hits, uint32_t lookup_cache_misses,
#endif
#if defined(ISAAC_VITA_GL_TIME_PROFILE)
    const IsaacVitaGlTimeProfile *gl_time,
    const kage_vita_scene_times *scene,
#endif
#if defined(ISAAC_VITA_GL_WRAPPER_TIME)
    const IsaacVitaGlWrapperTimeProfile *wrapper,
#endif
    const IsaacVitaGlPhaseProfileCounters *gl
#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
    , const vglIsaacGpuDrawStats *vitagl
#endif
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
    , const vglIsaacColorOffsetStats *coloroffset
#endif
#if defined(ISAAC_VITA_COLOROFFSET_FS_PROBE)
    , const vglIsaacColorOffsetFsProbeStats *fs_probe
#endif
    )
{
    kage_vita_profile_summary service = kage_vita_profile_summarize(
        &s_samples[KAGE_VITA_PROFILE_SERVICE]);
    kage_vita_profile_summary update = kage_vita_profile_summarize(
        &s_samples[KAGE_VITA_PROFILE_UPDATE]);
    kage_vita_profile_summary render = kage_vita_profile_summarize(
        &s_samples[KAGE_VITA_PROFILE_RENDER]);
    kage_vita_profile_summary present = kage_vita_profile_summarize(
        &s_samples[KAGE_VITA_PROFILE_PRESENT]);
    kage_vita_profile_summary limiter = kage_vita_profile_summarize(
        &s_samples[KAGE_VITA_PROFILE_LIMITER]);
    kage_vita_profile_summary whole = kage_vita_profile_summarize(
        &s_samples[KAGE_VITA_PROFILE_WHOLE]);
    kage_vita_profile_summary present_gap = kage_vita_profile_summarize(
        &s_samples[KAGE_VITA_PROFILE_PRESENT_GAP]);
#if defined(ISAAC_VITA_PHASE_PROFILE_OTHER)
    kage_vita_profile_summary other = kage_vita_profile_summarize(
        &s_samples[KAGE_VITA_PROFILE_OTHER]);
#endif
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
    uint32_t pace_wait_calls = 0u;
    uint32_t pace_waited_us = 0u;
#endif

#if !defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
    (void)report_at_us;
#endif
    /* Every enabled record is one call with one newline, emitted in
     * t,c,a,gt,gw,gd,gu,gx,x,xd,g,d,i,o,y,v,q,f,fa,fp,e,k,b,h,p,l,m,n,s,r,w,ik,ih,mem,ha,rl order
     * (kage_vita_phase_profile.h; gw (ISAAC_VITA_GL_WRAPPER_TIME) directly
     * after gt, then gx: gt and gx come from the same scene take).
     * Prefix/payload splitting is forbidden because the Vita logger
     * attributes records per call. */
    kage_vita_profile_log_timings(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        last_outer_loop, &service, &update, &render, &present,
        &limiter,
#if defined(ISAAC_VITA_PHASE_PROFILE_OTHER)
        &other,
#endif
        &whole, &present_gap, s_bad_sequence, s_clamped_duration);
#if defined(ISAAC_VITA_FRAME_SPIKE_PROFILE)
    kage_vita_profile_log_spikes(last_outer_loop);
#endif
    /* Guest sampler (opt-in): per-phase hot translated functions of the
     * window just summarised, printed directly after t so a reader can join
     * them on win.  Compiles to nothing without ISAAC_VITA_GUEST_SAMPLER. */
    KAGE_VITA_GUEST_SAMPLER_REPORT(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        last_outer_loop);
    kage_vita_profile_log_counts(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        last_outer_loop, &service, &update, &render, &present,
        &limiter, &whole, guest, gl, kage_vita_profile_dropped_total());
    kage_vita_profile_log_locations(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        last_outer_loop, gl,
        g_isaac_vita_post_com_time_get_time_calls - s_window_time_get_time);
#if defined(ISAAC_VITA_GL_TIME_PROFILE)
    kage_vita_profile_log_gl_time(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        last_outer_loop, gl_time, scene);
# if defined(ISAAC_VITA_GL_WRAPPER_TIME)
    kage_vita_profile_log_gl_wrapper_time(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        last_outer_loop, wrapper);
    kage_vita_profile_log_gl_draw_split(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        last_outer_loop, wrapper);
    kage_vita_profile_log_gl_uniform_split(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        last_outer_loop, wrapper);
# endif
    kage_vita_profile_log_gl_scene_split(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        last_outer_loop, scene);
# if defined(ISAAC_VITA_STOCK_FBO_VALID_REGION)
    kage_vita_profile_log_fbo_valid_region_census(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        last_outer_loop, scene);
# endif
#endif
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    {
        IsaacVitaTextureChurnProfile texture;

        gl_vita_backend_texture_churn_profile_take_window(&texture);
        kage_vita_profile_log_texture_churn(
            ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
            s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
            last_outer_loop, &texture);
        kage_vita_profile_log_texture_delete(
            ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
            s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
            last_outer_loop, &texture);
    }
#endif
    KVPP_ROOM_REPORT(ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW, last_outer_loop);
#if defined(ISAAC_VITA_IMAGE_RETAIN)
    kage_vita_profile_log_image_retain(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        last_outer_loop);
#endif
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
    kage_vita_profile_log_guest_cache(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        last_outer_loop, lookup_cache_hits, lookup_cache_misses);
#endif
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
    kage_vita_profile_log_dispatch(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        last_outer_loop, guest);
    kage_vita_profile_log_imports(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        last_outer_loop, s_import_calls_delta, guest->sync_fastpath);
#endif
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    kage_vita_profile_log_gl_redundancy(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        last_outer_loop, gl);
#endif
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    kage_vita_profile_log_gl_typed_state(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        last_outer_loop, gl);
#endif
#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
    kage_vita_profile_log_vitagl_draw(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        last_outer_loop, vitagl);
    kage_vita_profile_log_draw_submission(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        last_outer_loop, gl, vitagl);
    kage_vita_profile_log_fusion_ceiling(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        last_outer_loop, gl);
#endif
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    {
        IsaacVitaGlFillCensus fill;

        /* Take-and-zero; the window's render p50 arms the optional dump. */
        gl_vita_backend_fill_census_take_window(
            &fill, s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
            render.p50);
        kage_vita_profile_log_fill_census(
            ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
            s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
            last_outer_loop, &fill);
    }
#endif
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION) ||         defined(ISAAC_VITA_FBO_RASTER_SCALE)
    kage_vita_profile_log_fbo_policy(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        last_outer_loop, gl);
#endif
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
    kage_vita_profile_log_coloroffset(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        last_outer_loop, coloroffset);
#endif
#if defined(ISAAC_VITA_COLOROFFSET_FS_PROBE)
    {
        uint32_t plain[3], pair[3], half[2];
        int has_pair = kage_vita_profile_take_plain_vertex_pair(pair);
        int has_half = kage_vita_profile_take_plain_fp16(half);
        kage_vita_profile_take_plain(plain);
        kage_vita_profile_log_coloroffset_fs_probe(
            ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
            s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
            last_outer_loop, fs_probe, plain, has_pair ? pair : NULL,
            has_half ? half : NULL);
    }
#endif
#if defined(ISAAC_VITA_PILL_BLOOM_BYPASS)
    kage_vita_profile_log_pill_bloom(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        last_outer_loop,
        s_pill_bloom_capture_skips, s_pill_bloom_composite_skips);
#endif
#if defined(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES)
    kage_vita_profile_log_bloom_half(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        last_outer_loop,
        s_bloom_half_create_initial, s_bloom_half_create_applied,
        s_bloom_half_create_rejected,
        s_bloom_half_captures, s_bloom_half_composites);
#endif
#if defined(ISAAC_VITA_POOP_FX_PROFILE)
    kage_vita_profile_log_poop_fx(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        last_outer_loop, &s_poop_fx);
#endif
#if defined(ISAAC_VITA_LASER_PROFILE)
    kage_vita_profile_log_laser(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        last_outer_loop, &s_laser);
    kage_vita_profile_log_laser_cost(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        last_outer_loop, &s_laser);
    kage_vita_profile_log_laser_submission(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        last_outer_loop, &s_laser);
#endif
#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
    {
        KageVitaFullspeedSnapshot scheduler;

        kage_vita_fullspeed_scheduler_snapshot(&scheduler);
        kage_vita_profile_log_scheduler(
            ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
            s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
            last_outer_loop, report_at_us,
            &scheduler, &s_window_scheduler);
        kage_vita_profile_log_emission(
            ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
            s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
            last_outer_loop, s_last_emit_us, s_max_emit_us);
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
        pace_wait_calls = scheduler.pace_wait_calls -
            s_window_scheduler.pace_wait_calls;
        pace_waited_us = scheduler.pace_waited_us -
            s_window_scheduler.pace_waited_us;
#endif
        s_window_scheduler = scheduler;
    }
#endif
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
    /* Last: ik and ih extend the established t..r order without moving any
     * earlier record. */
    kage_vita_profile_log_import_kinds(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        last_outer_loop, guest, pace_wait_calls, pace_waited_us);
#endif
#if defined(ISAAC_VITA_HEAP_CENSUS)
    /* Last: mem extends the established t..ih order without moving any
     * earlier record.  Its cost (one heap-lock section, one mallinfo, the
     * Lua/vitaGL scalar reads) is inside the ph120.w emit(us) bracket. */
    kage_vita_profile_heap_report(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        last_outer_loop, "win");
#endif
#if defined(ISAAC_VITA_KAGE_QUAD_FASTPATH)
    /* Last: kq extends the established t..mem order without moving any
     * earlier record; the take-and-zero is the window boundary. */
    {
        IsaacVitaKageQuadCounters kage_quad;

        isaac_vita_kage_quad_fastpath_take(&kage_quad);
        kage_vita_profile_log_kage_quad(
            ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
            s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
            last_outer_loop, &kage_quad);
    }
#endif
#if defined(ISAAC_VITA_LASER_HALO_PROFILE) && ISAAC_VITA_LASER_HALO_PROFILE
    /* One counter snapshot at the outer window boundary, never at the
     * nested laser entry/return snapshots. Keep every legacy record ordered. */
    kage_vita_profile_report_halo(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW, last_outer_loop);
#endif
#if defined(ISAAC_VITA_ROOM_LOG_MARKERS) && ISAAC_VITA_ROOM_LOG_MARKERS
    /* Inside the existing emission-cost bracket; never print/lock in CRT. */
    kage_vita_profile_log_room_events(ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW, last_outer_loop);
#endif
}

#if defined(ISAAC_VITA_HEAP_CENSUS)
void kage_vita_phase_profile_heap_census_final(const char *why)
{
    uint32_t claim;

    /* Boot failures reach the entry owner's terminal tail before the first
     * profiled loop head: nothing to report and no window to attribute. */
    if (!s_running || !why)
        return;
    claim = strcmp(why, "badalloc") == 0 ? 1u
        : strcmp(why, "exit") == 0 ? 2u : 4u;
    if (s_heap_census_final_claimed & claim)
        return;
    s_heap_census_final_claimed |= claim;
    kage_vita_profile_heap_report(
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW,
        s_last_outer_loop, why);
}
#endif

#if defined(ISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE)
void kage_vita_phase_profile_oracle_emit_max_records(void)
{
    static const char max_build_id[] =
        "1234567890123456789012345678901234567890";
    kage_vita_profile_summary summary;
    GuestPhaseProfileCounters guest;
    IsaacVitaGlPhaseProfileCounters gl;
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    IsaacVitaTextureChurnProfile texture;
#endif
#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
    vglIsaacGpuDrawStats vitagl;
#endif
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
    vglIsaacColorOffsetStats coloroffset;
#endif
#if defined(ISAAC_VITA_COLOROFFSET_FS_PROBE)
    vglIsaacColorOffsetFsProbeStats fs_probe;
#endif
#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
    KageVitaFullspeedSnapshot scheduler;
    KageVitaFullspeedSnapshot scheduler_prior;
#endif

    summary.count = UINT32_MAX;
    summary.minimum = UINT32_MAX;
    summary.p50 = UINT32_MAX;
    summary.p95 = UINT32_MAX;
    summary.maximum = UINT32_MAX;
    guest.guest_calls = UINT32_MAX;
    guest.guest_lookups = UINT32_MAX;
    guest.lookup_iterations = UINT32_MAX;
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
    guest.dispatch_calls = UINT32_MAX;
    guest.dispatch_slow = UINT32_MAX;
    guest.sync_fastpath = UINT32_MAX;
# if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
    /* The derived g(l) of ph120.c / ph120.ik is longest with no GL hits to
     * subtract; ph120.d gets its own all-ones field below. */
    guest.dispatch_gl = 0U;
# endif
#endif
    memset(&gl, 0xff, sizeof gl);
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    memset(&texture, 0xff, sizeof texture);
#endif
    kage_vita_profile_log_timings(
        max_build_id, UINT32_MAX, UINT32_MAX,
        &summary, &summary, &summary, &summary, &summary,
#if defined(ISAAC_VITA_PHASE_PROFILE_OTHER)
        &summary,
#endif
        &summary, &summary,
        UINT32_MAX, UINT32_MAX);
    kage_vita_profile_log_counts(
        max_build_id, UINT32_MAX, UINT32_MAX,
        &summary, &summary, &summary, &summary, &summary, &summary,
        &guest, &gl, UINT32_MAX);
    kage_vita_profile_log_locations(
        max_build_id, UINT32_MAX, UINT32_MAX, &gl, UINT32_MAX);
#if defined(ISAAC_VITA_GL_TIME_PROFILE)
    {
        IsaacVitaGlTimeProfile gl_time;
        kage_vita_scene_times scene;

        memset(&gl_time, 0xff, sizeof gl_time);
        memset(&scene, 0xff, sizeof scene);
        kage_vita_profile_log_gl_time(
            max_build_id, UINT32_MAX, UINT32_MAX, &gl_time, &scene);
# if defined(ISAAC_VITA_GL_WRAPPER_TIME)
        {
            IsaacVitaGlWrapperTimeProfile wrapper;

            memset(&wrapper, 0xff, sizeof wrapper);
            kage_vita_profile_log_gl_wrapper_time(
                max_build_id, UINT32_MAX, UINT32_MAX, &wrapper);
            kage_vita_profile_log_gl_draw_split(
                max_build_id, UINT32_MAX, UINT32_MAX, &wrapper);
            kage_vita_profile_log_gl_uniform_split(
                max_build_id, UINT32_MAX, UINT32_MAX, &wrapper);
        }
# endif
        kage_vita_profile_log_gl_scene_split(
            max_build_id, UINT32_MAX, UINT32_MAX, &scene);
# if defined(ISAAC_VITA_STOCK_FBO_VALID_REGION)
        kage_vita_profile_log_fbo_valid_region_census(
            max_build_id, UINT32_MAX, UINT32_MAX, &scene);
# endif
    }
#endif
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    kage_vita_profile_log_texture_churn(
        max_build_id, UINT32_MAX, UINT32_MAX, &texture);
    kage_vita_profile_log_texture_delete(
        max_build_id, UINT32_MAX, UINT32_MAX, &texture);
#endif
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
    kage_vita_profile_log_guest_cache(
        max_build_id, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX);
#endif
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
    {
        static uint32_t max_imports[GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS];

        memset(max_imports, 0xff, sizeof max_imports);
# if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
        guest.dispatch_gl = UINT32_MAX;
# endif
        kage_vita_profile_log_dispatch(
            max_build_id, UINT32_MAX, UINT32_MAX, &guest);
# if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
        guest.dispatch_gl = 0U;
# endif
        kage_vita_profile_log_imports(
            max_build_id, UINT32_MAX, UINT32_MAX, max_imports, UINT32_MAX);
    }
#endif
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    kage_vita_profile_log_gl_redundancy(
        max_build_id, UINT32_MAX, UINT32_MAX, &gl);
#endif
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    kage_vita_profile_log_gl_typed_state(
        max_build_id, UINT32_MAX, UINT32_MAX, &gl);
#endif
#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
    memset(&vitagl, 0xff, sizeof vitagl);
    kage_vita_profile_log_vitagl_draw(
        max_build_id, UINT32_MAX, UINT32_MAX, &vitagl);
    kage_vita_profile_log_draw_submission(
        max_build_id, UINT32_MAX, UINT32_MAX, &gl, &vitagl);
    kage_vita_profile_log_fusion_ceiling(
        max_build_id, UINT32_MAX, UINT32_MAX, &gl);
#endif
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    {
        IsaacVitaGlFillCensus fill;

        memset(&fill, 0xff, sizeof fill);
        kage_vita_profile_log_fill_census(
            max_build_id, UINT32_MAX, UINT32_MAX, &fill);
    }
#endif
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
    memset(&coloroffset, 0xff, sizeof coloroffset);
    kage_vita_profile_log_coloroffset(
        max_build_id, UINT32_MAX, UINT32_MAX, &coloroffset);
#endif
#if defined(ISAAC_VITA_COLOROFFSET_FS_PROBE)
    memset(&fs_probe, 0xff, sizeof fs_probe);
    {
        const uint32_t plain[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
        kage_vita_profile_log_coloroffset_fs_probe(
            max_build_id, UINT32_MAX, UINT32_MAX, &fs_probe, plain,
#if defined(ISAAC_VITA_COLOROFFSET_PLAIN_VERTEX_PAIR_ORACLE)
            plain,
#else
            NULL,
#endif
#if defined(ISAAC_VITA_COLOROFFSET_PLAIN_FP16_ORACLE)
            plain);
#else
            NULL);
#endif
    }
#endif
#if defined(ISAAC_VITA_PILL_BLOOM_BYPASS)
    kage_vita_profile_log_pill_bloom(
        max_build_id, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX);
#endif
#if defined(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES)
    kage_vita_profile_log_bloom_half(
        max_build_id, UINT32_MAX, UINT32_MAX,
        UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX);
#endif
#if defined(ISAAC_VITA_POOP_FX_PROFILE)
    {
        KageVitaPoopFxProfileStats poop_fx;
        memset(&poop_fx, 0xff, sizeof poop_fx);
        kage_vita_profile_log_poop_fx(
            max_build_id, UINT32_MAX, UINT32_MAX, &poop_fx);
    }
#endif
#if defined(ISAAC_VITA_LASER_PROFILE)
    {
        KageVitaLaserProfileStats laser;
        memset(&laser, 0xff, sizeof laser);
        kage_vita_profile_log_laser(
            max_build_id, UINT32_MAX, UINT32_MAX, &laser);
        kage_vita_profile_log_laser_cost(
            max_build_id, UINT32_MAX, UINT32_MAX, &laser);
        kage_vita_profile_log_laser_submission(
            max_build_id, UINT32_MAX, UINT32_MAX, &laser);
    }
#endif
#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
    memset(&scheduler, 0xff, sizeof scheduler);
    memset(&scheduler_prior, 0, sizeof scheduler_prior);
    kage_vita_profile_log_scheduler(
        max_build_id, UINT32_MAX, UINT32_MAX, UINT64_MAX,
        &scheduler, &scheduler_prior);
    kage_vita_profile_log_emission(
        max_build_id, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX);
#endif
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
    {
        IsaacVitaImportKindSnapshot imports;

        memset(&imports, 0xff, sizeof imports);
        kage_vita_profile_log_import_kinds_records(
            max_build_id, UINT32_MAX, UINT32_MAX, &imports, UINT32_MAX,
            UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
            INT32_MIN, INT32_MIN, 0u);
    }
#endif
#if defined(ISAAC_VITA_HEAP_CENSUS)
    {
        kage_vita_heap_record heap;

        /* All-ones fields with the longest why= (badalloc, %.8s). */
        memset(&heap, 0xff, sizeof heap);
        kage_vita_profile_log_heap(
            max_build_id, UINT32_MAX, UINT32_MAX, &heap, "badalloc");
    }
#endif
#if defined(ISAAC_VITA_KAGE_QUAD_FASTPATH)
    {
        IsaacVitaKageQuadCounters kage_quad;

        memset(&kage_quad, 0xff, sizeof kage_quad);
        kage_vita_profile_log_kage_quad(
            max_build_id, UINT32_MAX, UINT32_MAX, &kage_quad);
    }
#endif
#if defined(ISAAC_VITA_LASER_HALO_PROFILE) && ISAAC_VITA_LASER_HALO_PROFILE
    {
        vglIsaacLaserHaloStats halo;
        memset(&halo, 0xff, sizeof halo);
        halo.white_enabled = 1u;
        kage_vita_profile_log_halo(max_build_id, UINT32_MAX, UINT32_MAX,
            UINT32_MAX, &halo);
    }
#endif
#if defined(ISAAC_VITA_ROOM_LOG_MARKERS) && ISAAC_VITA_ROOM_LOG_MARKERS
    kage_vita_phase_profile_oracle_room_seed(UINT32_MAX);
    kage_vita_room_log_add(&s_room_log_epoch, 1u, 1u);
    kage_vita_room_log_add(&s_room_log_seq, 1u, 2u);
    kage_vita_room_log_add(&s_room_log_drop, 1u, 4u);
    kage_vita_room_log_add(&s_room_log_reset_lost, 1u, 8u);
    kage_vita_profile_log_room_events(max_build_id, UINT32_MAX, UINT32_MAX);
#endif
}
#endif

static void kage_vita_profile_close_dangling(void)
{
    kage_vita_profile_active *active[] = {
        &s_service, &s_update, &s_render, &s_present, &s_limiter
    };
    size_t i;

    if (s_present.active)
        s_have_present_return = 0u;
    for (i = 0u; i < sizeof active / sizeof active[0]; ++i) {
        if (active[i]->active) {
            active[i]->active = 0u;
            ++s_bad_sequence;
        }
    }
#if defined(ISAAC_VITA_LASER_PROFILE)
    for (i = 0u; i < 2u; ++i) {
        if (s_laser_active_depth[i] != 0u) {
            s_laser.dangling += s_laser_active_depth[i];
            ++s_laser.bad;
            s_laser_active_depth[i] = 0u;
        }
    }
#endif
}

void kage_vita_phase_profile_note_loop_head(uint32_t outer_loop_count)
{
    uint64_t now = kage_vita_profile_now();
    int reported = 0;

    if (!s_running) {
#if defined(ISAAC_VITA_ROOM_LOG_MARKERS) && ISAAC_VITA_ROOM_LOG_MARKERS
        if (s_room_log_epoch == 0u) {
            int saved_errno = errno;
            /* The direct native initialization path need not call reset.
             * Bind once at this live owner boundary, without resetting any
             * phase state. Explicit reset always leaves a nonzero epoch.
             * A failed UID remains unbound until an explicit reset. */
            s_room_log_owner = sceKernelGetThreadId();
            s_room_log_epoch = 1u;
            errno = saved_errno;
        }
#endif
        s_running = 1u;
        KVPP_ROOM_DISCARD();
#if defined(ISAAC_VITA_LASER_HALO_PROFILE) && ISAAC_VITA_LASER_HALO_PROFILE
        s_window_halo_valid = vglGetIsaacLaserHaloStats(
            &s_window_halo, ISAAC_LASER_HALO_STATS_WORDS)
            && s_window_halo.abi_version == ISAAC_LASER_HALO_STATS_ABI
            && s_window_halo.white_enabled <= 1u;
        s_window_halo_from = 0u;
#endif
        s_last_outer_loop = outer_loop_count;
        s_window_guest = g_guest_phase_profile_counters;
        KAGE_VITA_PROFILE_SNAPSHOT_IMPORTS();
        KAGE_VITA_PROFILE_IMPORT_KINDS_BASELINE();
#if defined(ISAAC_VITA_KAGE_QUAD_FASTPATH)
        /* Discard the replay verdicts of the loading/menu quads before the
         * first window, like the gt buckets below. */
        isaac_vita_kage_quad_fastpath_take(NULL);
#endif
#if defined(ISAAC_VITA_GL_TIME_PROFILE)
        {
            /* Discard whatever accumulated before the first window (loading
             * uploads, first scenes) so window 1 is as clean as the others. */
            kage_vita_scene_times discarded;

            memset(&g_isaac_vita_gl_time_profile, 0,
                   sizeof g_isaac_vita_gl_time_profile);
            kage_vita_profile_take_scene_times(&discarded);
#if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE) && ISAAC_VITA_GL_TIME_SDK_SPARSE
            vglIsaacSdkSparseTake(NULL, 1u);
# if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE_ATTRIB)
            isaac_vita_attrib_replay_sparse_take(NULL, 1u);
# endif
#endif
#if defined(ISAAC_VITA_GL_WRAPPER_TIME)
            memset(&g_isaac_vita_gl_wrapper_time, 0,
                   sizeof g_isaac_vita_gl_wrapper_time);
#endif
        }
#endif
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
        s_window_lookup_cache_hits = g_guest_lookup_cache_hits;
        s_window_lookup_cache_misses = g_guest_lookup_cache_misses;
#endif
        s_window_gl = g_isaac_vita_gl_phase_profile_counters;
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
        s_window_attrib_pending = gl_vita_backend_attrib_pending();
#endif
        s_window_time_get_time = g_isaac_vita_post_com_time_get_time_calls;
#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
        vglGetIsaacGpuDrawStats(&s_window_vitagl);
#endif
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
        vglGetIsaacColorOffsetStats(&s_window_coloroffset);
#endif
#if defined(ISAAC_VITA_COLOROFFSET_FS_PROBE)
        vglGetIsaacColorOffsetFsProbeStats(&s_window_fs_probe);
        {
            uint32_t discarded[3];
            kage_vita_profile_take_plain(discarded);
            (void)kage_vita_profile_take_plain_vertex_pair(discarded);
            (void)kage_vita_profile_take_plain_fp16(discarded);
        }
#endif
        s_whole_started_at = now;
#if defined(ISAAC_VITA_FRAME_SPIKE_PROFILE)
        kage_vita_profile_spike_begin();
#endif
#if defined(ISAAC_VITA_DEEP_PROFILE)
        kage_vita_deep_frame_begin(outer_loop_count,
            s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW + 1u);
#endif
        return;
    }
    if (outer_loop_count != s_last_outer_loop + 1u)
        ++s_bad_sequence;
    KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_OTH);
    kage_vita_profile_close_dangling();
#if defined(ISAAC_VITA_DEEP_PROFILE)
    kage_vita_deep_frame_end();
#endif
    kage_vita_profile_record(
        KAGE_VITA_PROFILE_WHOLE, s_whole_started_at, now);
#if defined(ISAAC_VITA_FRAME_SPIKE_PROFILE)
    kage_vita_profile_spike_finish(now);
#endif
    ++s_completed_loops;
    if (s_completed_loops % KAGE_VITA_PHASE_PROFILE_WINDOW == 0u) {
        GuestPhaseProfileCounters guest;
        IsaacVitaGlPhaseProfileCounters gl;
#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
        vglIsaacGpuDrawStats vitagl_current;
        vglIsaacGpuDrawStats vitagl_delta;
#endif
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
        vglIsaacColorOffsetStats coloroffset_current;
        vglIsaacColorOffsetStats coloroffset_delta;
#endif
#if defined(ISAAC_VITA_COLOROFFSET_FS_PROBE)
        vglIsaacColorOffsetFsProbeStats fs_probe_current;
        vglIsaacColorOffsetFsProbeStats fs_probe_delta;
#endif
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
        uint32_t lookup_cache_hits;
        uint32_t lookup_cache_misses;
#endif
#if defined(ISAAC_VITA_GL_TIME_PROFILE)
        IsaacVitaGlTimeProfile gl_time;
        kage_vita_scene_times scene;
#endif
#if defined(ISAAC_VITA_GL_WRAPPER_TIME)
        IsaacVitaGlWrapperTimeProfile wrapper;
#endif
#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
        /* ph120.w emit(us,max) of the next window: this whole block's wall
         * time on the game thread, the same clock as the phases. */
        uint64_t emit_started_at = kage_vita_profile_now();
#endif

        guest.guest_calls =
            g_guest_phase_profile_counters.guest_calls -
            s_window_guest.guest_calls;
        guest.guest_lookups =
            g_guest_phase_profile_counters.guest_lookups -
            s_window_guest.guest_lookups;
        guest.lookup_iterations =
            g_guest_phase_profile_counters.lookup_iterations -
            s_window_guest.lookup_iterations;
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
        guest.dispatch_calls =
            g_guest_phase_profile_counters.dispatch_calls -
            s_window_guest.dispatch_calls;
        guest.dispatch_slow =
            g_guest_phase_profile_counters.dispatch_slow -
            s_window_guest.dispatch_slow;
        guest.sync_fastpath =
            g_guest_phase_profile_counters.sync_fastpath -
            s_window_guest.sync_fastpath;
# if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
        guest.dispatch_gl =
            g_guest_phase_profile_counters.dispatch_gl -
            s_window_guest.dispatch_gl;
# endif
#endif
#if defined(KAGE_VITA_PROFILE_IMPORT_CENSUS)
        kage_vita_profile_take_import_window();
#endif
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
        lookup_cache_hits =
            g_guest_lookup_cache_hits - s_window_lookup_cache_hits;
        lookup_cache_misses =
            g_guest_lookup_cache_misses - s_window_lookup_cache_misses;
# if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
        /* A dispatch-table hit is the successor of a confirmed dispatch-cache
         * hit, so gc(h) + gc(m) keeps covering every g(l) lookup. */
        lookup_cache_hits += guest.dispatch_calls - guest.dispatch_slow;
#  if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
        lookup_cache_hits -= guest.dispatch_gl;
#  endif
# endif
#endif
        gl.draw_elements =
            g_isaac_vita_gl_phase_profile_counters.draw_elements -
            s_window_gl.draw_elements;
        gl.clear = g_isaac_vita_gl_phase_profile_counters.clear -
            s_window_gl.clear;
        gl.bind_texture =
            g_isaac_vita_gl_phase_profile_counters.bind_texture -
            s_window_gl.bind_texture;
        gl.use_program =
            g_isaac_vita_gl_phase_profile_counters.use_program -
            s_window_gl.use_program;
        gl.tex_image = g_isaac_vita_gl_phase_profile_counters.tex_image -
            s_window_gl.tex_image;
        gl.tex_sub_image =
            g_isaac_vita_gl_phase_profile_counters.tex_sub_image -
            s_window_gl.tex_sub_image;
        gl.uniform = g_isaac_vita_gl_phase_profile_counters.uniform -
            s_window_gl.uniform;
        gl.attrib_pointer =
            g_isaac_vita_gl_phase_profile_counters.attrib_pointer -
            s_window_gl.attrib_pointer;
        gl.attrib_toggle =
            g_isaac_vita_gl_phase_profile_counters.attrib_toggle -
            s_window_gl.attrib_toggle;
        gl.bind_framebuffer =
            g_isaac_vita_gl_phase_profile_counters.bind_framebuffer -
            s_window_gl.bind_framebuffer;
        gl.state = g_isaac_vita_gl_phase_profile_counters.state -
            s_window_gl.state;
        gl.attrib_location =
            g_isaac_vita_gl_phase_profile_counters.attrib_location -
            s_window_gl.attrib_location;
        gl.uniform_location =
            g_isaac_vita_gl_phase_profile_counters.uniform_location -
            s_window_gl.uniform_location;
        gl.location_cache_hit =
            g_isaac_vita_gl_phase_profile_counters.location_cache_hit -
            s_window_gl.location_cache_hit;
        gl.location_cache_mismatch =
            g_isaac_vita_gl_phase_profile_counters.location_cache_mismatch -
            s_window_gl.location_cache_mismatch;
        gl.canonical_quad_hits =
            g_isaac_vita_gl_phase_profile_counters.canonical_quad_hits -
            s_window_gl.canonical_quad_hits;
        gl.canonical_quad_reject_callsite =
            g_isaac_vita_gl_phase_profile_counters
                .canonical_quad_reject_callsite -
            s_window_gl.canonical_quad_reject_callsite;
        gl.canonical_quad_reject_shape =
            g_isaac_vita_gl_phase_profile_counters
                .canonical_quad_reject_shape -
            s_window_gl.canonical_quad_reject_shape;
        gl.canonical_quad_reject_bounds =
            g_isaac_vita_gl_phase_profile_counters
                .canonical_quad_reject_bounds -
            s_window_gl.canonical_quad_reject_bounds;
        gl.canonical_quad_reject_pointer =
            g_isaac_vita_gl_phase_profile_counters
                .canonical_quad_reject_pointer -
            s_window_gl.canonical_quad_reject_pointer;
        gl.canonical_quad_driver_fallback =
            g_isaac_vita_gl_phase_profile_counters
                .canonical_quad_driver_fallback -
            s_window_gl.canonical_quad_driver_fallback;
        gl.canonical_quad_index_bytes_saved =
            g_isaac_vita_gl_phase_profile_counters
                .canonical_quad_index_bytes_saved -
            s_window_gl.canonical_quad_index_bytes_saved;
        gl.fusion_index_bytes =
            g_isaac_vita_gl_phase_profile_counters.fusion_index_bytes -
            s_window_gl.fusion_index_bytes;
        gl.fusion_blend_additive =
            g_isaac_vita_gl_phase_profile_counters.fusion_blend_additive -
            s_window_gl.fusion_blend_additive;
        gl.fusion_blend_alpha =
            g_isaac_vita_gl_phase_profile_counters.fusion_blend_alpha -
            s_window_gl.fusion_blend_alpha;
        gl.fusion_blend_other =
            g_isaac_vita_gl_phase_profile_counters.fusion_blend_other -
            s_window_gl.fusion_blend_other;
        gl.fusion_shader_same =
            g_isaac_vita_gl_phase_profile_counters.fusion_shader_same -
            s_window_gl.fusion_shader_same;
        gl.fusion_shader_change =
            g_isaac_vita_gl_phase_profile_counters.fusion_shader_change -
            s_window_gl.fusion_shader_change;
        gl.fusion_fbo_default =
            g_isaac_vita_gl_phase_profile_counters.fusion_fbo_default -
            s_window_gl.fusion_fbo_default;
        gl.fusion_fbo_offscreen =
            g_isaac_vita_gl_phase_profile_counters.fusion_fbo_offscreen -
            s_window_gl.fusion_fbo_offscreen;
        gl.fusion_atlas_same =
            g_isaac_vita_gl_phase_profile_counters.fusion_atlas_same -
            s_window_gl.fusion_atlas_same;
        gl.fusion_atlas_change =
            g_isaac_vita_gl_phase_profile_counters.fusion_atlas_change -
            s_window_gl.fusion_atlas_change;
        gl.fusion_additive_draws =
            g_isaac_vita_gl_phase_profile_counters.fusion_additive_draws -
            s_window_gl.fusion_additive_draws;
        gl.fusion_additive_runs =
            g_isaac_vita_gl_phase_profile_counters.fusion_additive_runs -
            s_window_gl.fusion_additive_runs;
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION) ||         defined(ISAAC_VITA_FBO_RASTER_SCALE)
        gl.clear_suppressed =
            g_isaac_vita_gl_phase_profile_counters.clear_suppressed -
            s_window_gl.clear_suppressed;
        gl.fbo_elision_poison =
            g_isaac_vita_gl_phase_profile_counters.fbo_elision_poison -
            s_window_gl.fbo_elision_poison;
        gl.fbo_raster_textures =
            g_isaac_vita_gl_phase_profile_counters.fbo_raster_textures -
            s_window_gl.fbo_raster_textures;
        gl.fbo_raster_viewports =
            g_isaac_vita_gl_phase_profile_counters.fbo_raster_viewports -
            s_window_gl.fbo_raster_viewports;
        gl.clear_replayed =
            g_isaac_vita_gl_phase_profile_counters.clear_replayed -
            s_window_gl.clear_replayed;
# if defined(ISAAC_VITA_FBO_CLEAR_ELISION_DEPTH_DROP)
        gl.clear_dropped_attach =
            g_isaac_vita_gl_phase_profile_counters.clear_dropped_attach -
            s_window_gl.clear_dropped_attach;
        gl.clear_dropped_scene =
            g_isaac_vita_gl_phase_profile_counters.clear_dropped_scene -
            s_window_gl.clear_dropped_scene;
# endif
        gl.fbo_raster_respecified =
            g_isaac_vita_gl_phase_profile_counters.fbo_raster_respecified -
            s_window_gl.fbo_raster_respecified;
#endif
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
        gl.use_program_suppressed =
            g_isaac_vita_gl_phase_profile_counters.use_program_suppressed -
            s_window_gl.use_program_suppressed;
        gl.uniform_suppressed =
            g_isaac_vita_gl_phase_profile_counters.uniform_suppressed -
            s_window_gl.uniform_suppressed;
#endif
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
# define ISAAC_GL_TYPED_DELTA(field) \
        gl.field = g_isaac_vita_gl_phase_profile_counters.field - \
            s_window_gl.field
        ISAAC_GL_TYPED_DELTA(typed_state_hit_active_texture);
        ISAAC_GL_TYPED_DELTA(typed_state_hit_bind_texture);
        ISAAC_GL_TYPED_DELTA(typed_state_hit_blend);
        ISAAC_GL_TYPED_DELTA(typed_state_hit_depth);
        ISAAC_GL_TYPED_DELTA(typed_state_hit_viewport);
        ISAAC_GL_TYPED_DELTA(typed_state_hit_attrib_toggle);
        ISAAC_GL_TYPED_DELTA(typed_state_hit_attrib_pointer);
        ISAAC_GL_TYPED_DELTA(typed_state_miss);
        ISAAC_GL_TYPED_DELTA(typed_state_reject_invalid);
        ISAAC_GL_TYPED_DELTA(typed_state_reject_unknown);
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
        ISAAC_GL_TYPED_DELTA(attrib_deferred);
        ISAAC_GL_TYPED_DELTA(attrib_cancelled);
#endif
# undef ISAAC_GL_TYPED_DELTA
#endif
#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
        vglGetIsaacGpuDrawStats(&vitagl_current);
        kage_vita_profile_vitagl_delta(
            &vitagl_delta, &vitagl_current, &s_window_vitagl);
#endif
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
        vglGetIsaacColorOffsetStats(&coloroffset_current);
        kage_vita_profile_coloroffset_delta(
            &coloroffset_delta, &coloroffset_current,
            &s_window_coloroffset);
#endif
#if defined(ISAAC_VITA_COLOROFFSET_FS_PROBE)
        vglGetIsaacColorOffsetFsProbeStats(&fs_probe_current);
        kage_vita_profile_coloroffset_fs_probe_delta(
            &fs_probe_delta, &fs_probe_current, &s_window_fs_probe);
#endif

#if defined(ISAAC_VITA_GL_TIME_PROFILE)
        /* Take-and-zero: the buckets belong to exactly the 120 loops that
         * ended at this loop head, like every other window counter. */
        gl_time = g_isaac_vita_gl_time_profile;
        memset(&g_isaac_vita_gl_time_profile, 0,
               sizeof g_isaac_vita_gl_time_profile);
        kage_vita_profile_take_scene_times(&scene);
#if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE) && ISAAC_VITA_GL_TIME_SDK_SPARSE
        vglIsaacSdkSparseTake(&scene.sparse,
            s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW + 1u);
# if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE_ATTRIB)
        isaac_vita_attrib_replay_sparse_take(&scene.attrib,
            s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW + 1u);
# endif
#endif
#endif
#if defined(ISAAC_VITA_GL_WRAPPER_TIME)
        /* Same take-and-zero as the gt buckets, same loop head. */
        wrapper = g_isaac_vita_gl_wrapper_time;
        memset(&g_isaac_vita_gl_wrapper_time, 0,
               sizeof g_isaac_vita_gl_wrapper_time);
#endif
        kage_vita_profile_report(
            s_last_outer_loop, now, &guest,
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
            lookup_cache_hits, lookup_cache_misses,
#endif
#if defined(ISAAC_VITA_GL_TIME_PROFILE)
            &gl_time, &scene,
#endif
#if defined(ISAAC_VITA_GL_WRAPPER_TIME)
            &wrapper,
#endif
            &gl
#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
            , &vitagl_delta
#endif
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
            , &coloroffset_delta
#endif
#if defined(ISAAC_VITA_COLOROFFSET_FS_PROBE)
            , &fs_probe_delta
#endif
            );
#if defined(ISAAC_VITA_DEEP_PROFILE)
        kage_vita_deep_report(ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
            s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW);
        isaac_vita_png_deep_report(ISAAC_VITA_PHASE_PROFILE_BUILD_ID,
            s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW);
#endif
        kage_vita_profile_reset_window();
        gl_vita_backend_phase_profile_window_boundary();
        s_window_guest = g_guest_phase_profile_counters;
        KAGE_VITA_PROFILE_SNAPSHOT_IMPORTS();
        KAGE_VITA_PROFILE_IMPORT_KINDS_BASELINE();
#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)
        s_window_lookup_cache_hits = g_guest_lookup_cache_hits;
        s_window_lookup_cache_misses = g_guest_lookup_cache_misses;
#endif
        s_window_gl = g_isaac_vita_gl_phase_profile_counters;
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
        s_window_attrib_pending = gl_vita_backend_attrib_pending();
#endif
        s_window_time_get_time = g_isaac_vita_post_com_time_get_time_calls;
#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
        s_window_vitagl = vitagl_current;
#endif
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
        s_window_coloroffset = coloroffset_current;
#endif
#if defined(ISAAC_VITA_COLOROFFSET_FS_PROBE)
        s_window_fs_probe = fs_probe_current;
#endif
#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
        {
            uint64_t emit_elapsed =
                kage_vita_profile_now() - emit_started_at;

            s_last_emit_us = emit_elapsed > UINT32_MAX
                ? UINT32_MAX : (uint32_t)emit_elapsed;
            if (s_last_emit_us > s_max_emit_us)
                s_max_emit_us = s_last_emit_us;
        }
#endif
        reported = 1;
    }
    s_last_outer_loop = outer_loop_count;
    /* Exclude percentile sorting and the synchronous report itself from the
     * following whole-loop sample. */
    s_whole_started_at = reported ? kage_vita_profile_now() : now;
#if defined(ISAAC_VITA_FRAME_SPIKE_PROFILE)
    kage_vita_profile_spike_begin();
#endif
#if defined(ISAAC_VITA_DEEP_PROFILE)
    kage_vita_deep_frame_begin(outer_loop_count,
        s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW + 1u);
#endif
}

/* The guest sampler's phase word is written at every seam regardless of
 * s_running so its buckets are meaningful from the first sampled loop; the
 * profile's own timing state below is unchanged. */
void kage_vita_phase_profile_note_service_entry(void)
{
#if defined(ISAAC_VITA_DEEP_PROFILE)
    kage_vita_deep_phase_set(KVD_PHASE_OTHER);
#endif
    KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_SVC);
#if defined(ISAAC_VITA_PHASE_PROFILE_OTHER)
    if (s_running) {
        uint64_t now = kage_vita_profile_now();

        kage_vita_profile_other_close(now);
        kage_vita_profile_begin(&s_service, now);
    }
#else
    if (s_running)
        kage_vita_profile_begin(&s_service, kage_vita_profile_now());
#endif
}

void kage_vita_phase_profile_note_update_entry(void)
{
    uint64_t now;

#if defined(ISAAC_VITA_DEEP_PROFILE)
    kage_vita_deep_phase_set(KVD_PHASE_UPDATE);
#endif

    KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_UPD);
    if (!s_running)
        return;
    now = kage_vita_profile_now();
    kage_vita_profile_end(KAGE_VITA_PROFILE_SERVICE, &s_service, now);
    kage_vita_profile_begin(&s_update, now);
}

void kage_vita_phase_profile_note_render_entry(void)
{
    uint64_t now;

#if defined(ISAAC_VITA_DEEP_PROFILE)
    kage_vita_deep_phase_set(KVD_PHASE_RENDER);
#endif

    KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_RND);
    if (!s_running)
        return;
    now = kage_vita_profile_now();
    kage_vita_profile_end(KAGE_VITA_PROFILE_UPDATE, &s_update, now);
    kage_vita_profile_begin(&s_render, now);
}

void kage_vita_phase_profile_note_render_return(void)
{
#if defined(ISAAC_VITA_DEEP_PROFILE)
    kage_vita_deep_phase_set(KVD_PHASE_OTHER);
#endif
    KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_OTH);
#if defined(ISAAC_VITA_PHASE_PROFILE_OTHER)
    if (s_running) {
        uint64_t now = kage_vita_profile_now();

        kage_vita_profile_end(KAGE_VITA_PROFILE_RENDER, &s_render, now);
        kage_vita_profile_other_begin(now);
    }
#else
    if (s_running)
        kage_vita_profile_end(
            KAGE_VITA_PROFILE_RENDER, &s_render, kage_vita_profile_now());
#endif
}

void kage_vita_phase_profile_note_render_skipped(void)
{
#if defined(ISAAC_VITA_DEEP_PROFILE)
    kage_vita_deep_phase_set(KVD_PHASE_OTHER);
#endif
    KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_OTH);
#if defined(ISAAC_VITA_PHASE_PROFILE_OTHER)
    if (s_running) {
        uint64_t now = kage_vita_profile_now();

        kage_vita_profile_end(KAGE_VITA_PROFILE_UPDATE, &s_update, now);
        kage_vita_profile_other_begin(now);
    }
#else
    if (s_running)
        kage_vita_profile_end(
            KAGE_VITA_PROFILE_UPDATE, &s_update, kage_vita_profile_now());
#endif
}

void kage_vita_phase_profile_note_present_enter(void)
{
    KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_SWP);
    if (s_running) {
        uint64_t now = kage_vita_profile_now();

        /* A replaced dangling pair or observed clock rollback cannot supply
         * a trustworthy preceding endpoint, even if return catches up. */
        if (s_present.active ||
                (s_have_present_return && now < s_last_present_return))
            s_have_present_return = 0u;
        kage_vita_profile_begin(&s_present, now);
    }
}

void kage_vita_phase_profile_note_present_return(void)
{
    /* The present is nested inside Render; fall back to the enclosing phase. */
    KAGE_VITA_GUEST_SAMPLER_PHASE(
        s_render.active ? KAGE_VITA_GUEST_SAMPLER_RND
                        : KAGE_VITA_GUEST_SAMPLER_OTH);
    if (s_running) {
        uint64_t now = kage_vita_profile_now();
        uint32_t valid = s_present.active && now >= s_present.started_at;

        kage_vita_profile_end(
            KAGE_VITA_PROFILE_PRESENT, &s_present, now);
        if (valid) {
            if (s_have_present_return)
                kage_vita_profile_record(KAGE_VITA_PROFILE_PRESENT_GAP,
                                        s_last_present_return, now);
            s_last_present_return = now;
        }
        /* A missing/invalid return breaks the chain. A backwards inter-call
         * clock is rejected by record(), then this valid pair starts a new
         * epoch. Do not turn either discontinuity into a giant unsigned gap. */
        s_have_present_return = valid;
    }
}

void kage_vita_phase_profile_note_limiter_entry(void)
{
    KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_LIM);
#if defined(ISAAC_VITA_PHASE_PROFILE_OTHER)
    if (s_running) {
        uint64_t now = kage_vita_profile_now();

        kage_vita_profile_other_pause(now);
        kage_vita_profile_begin(&s_limiter, now);
    }
#else
    if (s_running)
        kage_vita_profile_begin(&s_limiter, kage_vita_profile_now());
#endif
}

void kage_vita_phase_profile_note_limiter_exit(void)
{
    KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_OTH);
#if defined(ISAAC_VITA_PHASE_PROFILE_OTHER)
    if (s_running) {
        uint64_t now = kage_vita_profile_now();

        kage_vita_profile_end(KAGE_VITA_PROFILE_LIMITER, &s_limiter, now);
        kage_vita_profile_other_begin(now);
    }
#else
    if (s_running)
        kage_vita_profile_end(
            KAGE_VITA_PROFILE_LIMITER, &s_limiter, kage_vita_profile_now());
#endif
}

#if defined(ISAAC_VITA_PILL_BLOOM_BYPASS)
void kage_vita_phase_profile_note_pill_bloom_capture_skip(void)
{
    if (s_running)
        ++s_pill_bloom_capture_skips;
}

void kage_vita_phase_profile_note_pill_bloom_composite_skip(void)
{
    if (s_running)
        ++s_pill_bloom_composite_skips;
}
#endif

#if defined(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES)
void kage_vita_phase_profile_note_bloom_half_create(uint32_t receipt)
{
    /* Surface creation precedes the measured render windows.  Keep this
     * lifetime receipt even while no window is running; window resets clear
     * only capture/composite counts. */
    if (receipt == 1u)
        ++s_bloom_half_create_initial;
    else if (receipt == 2u)
        ++s_bloom_half_create_applied;
    else
        ++s_bloom_half_create_rejected;
}

void kage_vita_phase_profile_note_bloom_half_capture(void)
{
    if (s_running)
        ++s_bloom_half_captures;
}

void kage_vita_phase_profile_note_bloom_half_composite(void)
{
    if (s_running)
        ++s_bloom_half_composites;
}
#endif

#if defined(ISAAC_VITA_POOP_FX_PROFILE)
static void kage_vita_poop_fx_add(uint32_t *value, uint32_t increment)
{
    if (*value > UINT32_MAX - increment) {
        *value = UINT32_MAX;
        if (s_poop_fx.bad != UINT32_MAX)
            ++s_poop_fx.bad;
    } else {
        *value += increment;
    }
}

void kage_vita_phase_profile_note_poop_fx_add(uint32_t return_rva)
{
    uint32_t *counter;

    if (!s_running)
        return;
    if (return_rva == 0x00388202u)
        counter = &s_poop_fx.add_pill;
    else if (return_rva == 0x002e7e96u)
        counter = &s_poop_fx.add_black;
    else {
        switch (return_rva) {
        case 0x00036b42u:
        case 0x0020c153u:
        case 0x002d33efu:
        case 0x0032cf42u:
        case 0x0036c47du:
        case 0x003c0006u:
            counter = &s_poop_fx.add_other;
            break;
        default:
            counter = &s_poop_fx.add_unknown;
            kage_vita_poop_fx_add(&s_poop_fx.bad, 1u);
            break;
        }
    }
    kage_vita_poop_fx_add(counter, 1u);
}

void kage_vita_phase_profile_note_poop_fx_render(
    uint32_t countdown, uint32_t cloud_count)
{
    if (!s_running)
        return;
    kage_vita_poop_fx_add(&s_poop_fx.active, 1u);
    kage_vita_poop_fx_add(&s_poop_fx.clouds, cloud_count);
    if (countdown < s_poop_fx.countdown_min)
        s_poop_fx.countdown_min = countdown;
    if (countdown > s_poop_fx.countdown_max)
        s_poop_fx.countdown_max = countdown;
    s_poop_fx.countdown_last = countdown;
    if (countdown == 0u || countdown > 180u || cloud_count != 3u)
        kage_vita_poop_fx_add(&s_poop_fx.bad, 1u);
}

void kage_vita_phase_profile_note_poop_fx_cap(uint32_t skipped_clouds)
{
    if (!s_running)
        return;
    kage_vita_poop_fx_add(&s_poop_fx.cap_frames, 1u);
    kage_vita_poop_fx_add(&s_poop_fx.cap_skipped, skipped_clouds);
    if (skipped_clouds != 2u)
        kage_vita_poop_fx_add(&s_poop_fx.bad, 1u);
}
#endif

#if defined(ISAAC_VITA_LASER_PROFILE)
static void kage_vita_laser_add(uint32_t *value, uint32_t increment)
{
    if (*value > UINT32_MAX - increment) {
        *value = UINT32_MAX;
        ++s_laser.bad;
    } else {
        *value += increment;
    }
}

static uint32_t kage_vita_laser_classify(
    uint32_t source_type, uint32_t variant, uint32_t subtype,
    uint32_t sample, uint32_t shadow)
{
    if (shadow) {
        ++s_laser.shadow;
        /* The exact Shadow seam is reachable only for signed sample > 0. */
        if (sample == 0u || sample >= 0x80u) {
            ++s_laser.bad;
            return KAGE_VITA_LASER_COST_CLASS_COUNT;
        }
        return KAGE_VITA_LASER_COST_SHADOW;
    }

    ++s_laser.render;
    if (sample == 0u)
        ++s_laser.sample_zero;
    else if (sample < 0x80u)
        ++s_laser.sample_positive;
    else
        ++s_laser.bad;

    if (source_type == 1u)
        ++s_laser.source_1;
    else if (source_type >= 10u && source_type < 1000u)
        ++s_laser.source_10_999;
    else
        ++s_laser.source_other;

    if (variant == 0u)
        ++s_laser.variant_0;
    else if (variant == 1u)
        ++s_laser.variant_1;
    else if (variant == 2u)
        ++s_laser.variant_2;
    else
        ++s_laser.variant_other;

    if (subtype >= 1u && subtype <= 3u)
        ++s_laser.subtype_1_3;
    else
        ++s_laser.subtype_other;

    if (sample == 0u)
        return KAGE_VITA_LASER_COST_SAMPLE_ZERO;
    if (sample < 0x80u)
        return KAGE_VITA_LASER_COST_SAMPLE_POSITIVE;
    return KAGE_VITA_LASER_COST_CLASS_COUNT;
}

void kage_vita_phase_profile_laser_begin(
    uint32_t token, uint32_t source_type, uint32_t variant,
    uint32_t subtype, uint32_t sample, uint32_t shadow)
{
    KageVitaLaserActive *active;
    uint32_t cost_class;
    uint32_t depth;

    if (!s_running)
        return;
    if (shadow > 1u) {
        ++s_laser.bad;
        return;
    }
    cost_class = kage_vita_laser_classify(
        source_type, variant, subtype, sample, shadow);
    depth = s_laser_active_depth[shadow];
    if (depth >= KAGE_VITA_LASER_ACTIVE_DEPTH) {
        ++s_laser.active_overflow;
        ++s_laser.bad;
        return;
    }

    active = &s_laser_active[shadow][depth];
    active->token = token;
    active->cost_class = cost_class;
#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
    vglGetIsaacGpuDrawStats(&active->vitagl);
#endif
    /* The clock follows the statistics snapshot so its cost is not charged to
     * the translated laser owner. */
    active->started_at = kage_vita_profile_now();
    s_laser_active_depth[shadow] = depth + 1u;
}

void kage_vita_phase_profile_laser_end(uint32_t token, uint32_t shadow)
{
    KageVitaLaserActive *active;
    KageVitaLaserCostBucket *cost;
    uint64_t ended_at;
    uint64_t elapsed;
    uint32_t depth;
    uint32_t index;
    uint32_t duration;
#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
    vglIsaacGpuDrawStats vitagl;
#endif

    if (!s_running)
        return;
    if (shadow > 1u) {
        ++s_laser.bad;
        return;
    }
    depth = s_laser_active_depth[shadow];
    for (index = depth; index != 0u; --index) {
        if (s_laser_active[shadow][index - 1u].token == token)
            break;
    }
    /* Every inactive original call reaches the same epilogue without a begin.
     * A token absent from the bounded active stack is therefore expected. */
    if (index == 0u)
        return;
    if (index != depth) {
        ++s_laser.end_mismatch;
        ++s_laser.bad;
        s_laser_active_depth[shadow] = 0u;
        return;
    }

    active = &s_laser_active[shadow][depth - 1u];
    ended_at = kage_vita_profile_now();
#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
    vglGetIsaacGpuDrawStats(&vitagl);
#endif
    s_laser_active_depth[shadow] = depth - 1u;
    if (active->cost_class >= KAGE_VITA_LASER_COST_CLASS_COUNT) {
        ++s_laser.cost_other;
        return;
    }

    cost = &s_laser.cost[active->cost_class];
    ++cost->completed;
    if (ended_at < active->started_at) {
        ++s_laser.clock_reset;
        ++s_laser.bad;
        duration = 0u;
    } else {
        elapsed = ended_at - active->started_at;
        if (elapsed > UINT32_MAX) {
            duration = UINT32_MAX;
            ++s_laser.bad;
        } else {
            duration = (uint32_t)elapsed;
        }
    }
    kage_vita_laser_add(&cost->total_us, duration);
    if (duration > cost->max_us)
        cost->max_us = duration;

#if defined(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
    if (active->vitagl.abi_version == 2u && vitagl.abi_version == 2u &&
            active->vitagl.enabled != 0u && vitagl.enabled != 0u) {
        ++s_laser.submission_samples;
        kage_vita_laser_add(
            &cost->draw_requests,
            vitagl.draw_elements_requests -
                active->vitagl.draw_elements_requests);
        kage_vita_laser_add(
            &cost->index_count,
            vitagl.index_count - active->vitagl.index_count);
        kage_vita_laser_add(
            &cost->vertex_count,
            vitagl.vertex_count - active->vitagl.vertex_count);
    } else {
        ++s_laser.submission_unavailable;
    }
#else
    ++s_laser.submission_unavailable;
#endif
}
#endif
