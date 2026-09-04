/* Deterministic host oracle for the aggregate physical-Vita phase profiler. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kage_vita_phase_profile.h"
#include "guest.h"
#include "gl_vita_backend.h"
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
# define ORACLE_GL_TIME_RECORDS 1U
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
#define ORACLE_RECORDS_PER_WINDOW \
    (2U + ORACLE_LOCATION_RECORDS + ORACLE_GL_TIME_RECORDS + \
     ORACLE_TEXTURE_CHURN_RECORDS + ORACLE_GUEST_CACHE_RECORDS + \
     ORACLE_DISPATCH_RECORDS + \
     ORACLE_GL_REDUNDANCY_RECORDS + ORACLE_GL_TYPED_STATE_RECORDS + \
     ORACLE_VITAGL_RECORDS + \
      ORACLE_COLOROFFSET_RECORDS + ORACLE_PILL_BLOOM_RECORDS + \
      ORACLE_BLOOM_HALF_RECORDS + ORACLE_POOP_FX_RECORDS + \
      ORACLE_LASER_RECORDS + \
      ORACLE_SCHEDULER_RECORDS + ORACLE_IMPORT_KINDS_RECORDS)
IsaacVitaGlPhaseProfileCounters g_isaac_vita_gl_phase_profile_counters;
/* host_vita_post_com.c owns this in production; the oracle links without it. */
uint32_t g_isaac_vita_post_com_time_get_time_calls;

static uint64_t s_now;
static char s_logs[8192];
static size_t s_log_length;
static unsigned s_log_calls;

static void add_gl_counts(uint32_t base)
{
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

int main(void)
{
    const char *timing;
    const char *counts;
    const char *location;
#if defined(ISAAC_VITA_GL_TIME_PROFILE)
    const char *gl_time;
    size_t max_gl_time_length;
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
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
    const char *coloroffset;
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
#endif

    kage_vita_phase_profile_reset();
    CHECK(s_fusion_boundaries == 1u);
#if defined(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES)
    kage_vita_phase_profile_note_bloom_half_create(1u);
    kage_vita_phase_profile_note_bloom_half_create(2u);
#endif
    s_now = 1000u;
    kage_vita_phase_profile_note_loop_head(1u);
#if defined(ISAAC_VITA_GL_TIME_PROFILE)
    /* The first loop head must have discarded the pre-window accumulation
     * (one scene take) and zeroed the buckets. */
    CHECK(s_scene_takes == 1u);
    CHECK(g_isaac_vita_gl_time_profile.draw.calls == 0u);
    set_gl_time_window(29u);
#endif
    run_variable_window(1u);
    CHECK(s_fusion_boundaries == 2u);
#if defined(ISAAC_VITA_GL_TIME_PROFILE)
    CHECK(s_scene_takes == 2u);
    CHECK(g_isaac_vita_gl_time_profile.draw.calls == 0u &&
          g_isaac_vita_gl_time_profile.present.total_us == 0u);
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
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
    coloroffset = log_line(next_line++);
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
    CHECK(timing != NULL && counts != NULL && location != NULL);
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
    CHECK(strstr(timing, "all=6060/6114/6120 bad=0 clamp=0\n") != NULL);
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
#if defined(ISAAC_VITA_GL_TIME_PROFILE)
    CHECK(gl_time != NULL);
    CHECK(strstr(gl_time,
                 "ph120.gt bid=phase:oracle win=1 loops=120 "
                 "us(d,c,b,t,s,p)=290,300,310,320,330,340 "
                 "max(d,c,b)=2900,3000,3100 "
                 "n(d,c,b,t,s,p)=29,30,31,32,33,34 "
                 "scene(n,b,e,bm)=10,1400,180,600 bad=0\n") != NULL);
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
                 "miss=4800 reject(i,u)=4920,5040\n") != NULL);
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
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
    CHECK(coloroffset != NULL);
    CHECK(strstr(coloroffset,
                 "ph120.k bid=phase:oracle win=1 loops=120 abi=1 en=1 "
                 "src=120/120/120/120 opq=120/120/120/120 "
                 "fail=120/120/120/120/120/120/120/120 "
                  "cache=120/120/120 sta=120/120/120\n") != NULL);
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
    CHECK(strstr(gl_time,
                 "ph120.gt bid=phase:oracle win=2 loops=240 "
                 "us(d,c,b,t,s,p)=0,0,0,0,0,0 max(d,c,b)=0,0,0 "
                 "n(d,c,b,t,s,p)=0,0,0,0,0,0 "
                 "scene(n,b,e,bm)=15,2100,270,900 bad=0\n") != NULL);
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
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
    coloroffset = log_line(next_line++);
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
    CHECK(timing != NULL && counts != NULL);
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
    CHECK(strstr(timing, "all=5000/5000/5000 bad=0 clamp=0\n") != NULL);
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
                 "miss=4920 reject(i,u)=5040,5160\n") != NULL);
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
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
    CHECK(coloroffset != NULL);
    CHECK(strstr(coloroffset,
                 "ph120.k bid=phase:oracle win=2 loops=240 abi=1 en=1 "
                 "src=240/240/240/240 opq=240/240/240/240 "
                 "fail=240/240/240/240/240/240/240/240 "
                  "cache=240/240/240 sta=240/240/240\n") != NULL);
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

    memset(s_logs, 0, sizeof s_logs);
    s_log_length = 0u;
    s_log_calls = 0u;
    kage_vita_phase_profile_oracle_emit_max_records();
    CHECK(s_log_calls == ORACLE_RECORDS_PER_WINDOW);
    max_timing_length = log_line_length(log_line(0u));
    max_counts_length = log_line_length(log_line(1u));
    max_location_length = log_line_length(log_line(2u));
#if defined(ISAAC_VITA_GL_TIME_PROFILE)
    max_gl_time_length = log_line_length(
        log_line(2u + ORACLE_LOCATION_RECORDS));
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
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
    max_coloroffset_length = log_line_length(log_line(next_line++));
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
#if defined(ISAAC_VITA_PHASE_PROFILE_OTHER)
    /* oth= adds 37 bytes at ten digits each; ph120.s already uses the
     * 512-byte allowance (503). */
    CHECK(max_timing_length == 394u && max_timing_length <= 512u);
#else
    CHECK(max_timing_length == 357u && max_timing_length < 384u);
#endif
    CHECK(max_counts_length == 375u && max_counts_length < 384u);
    CHECK(max_location_length == 161u && max_location_length < 384u);
#if defined(ISAAC_VITA_GL_TIME_PROFILE)
    CHECK(max_gl_time_length == 372u && max_gl_time_length < 384u);
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
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
    CHECK(max_coloroffset_length == 383u && max_coloroffset_length < 384u);
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
    CHECK(next_line == ORACLE_RECORDS_PER_WINDOW);
    CHECK(log_line(ORACLE_RECORDS_PER_WINDOW) != NULL &&
          *log_line(ORACLE_RECORDS_PER_WINDOW) == '\0');

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
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
    printf("; d=%zu/i=%zu", max_dispatch_length, max_imports_length);
#endif
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
    printf("; ik/ih=%zu/%zu", max_import_kinds_length,
           max_import_hot_length);
#endif
    printf("; limiter skips; log cost excluded)\n");
    return 0;
}
