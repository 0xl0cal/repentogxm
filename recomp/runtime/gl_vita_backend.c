/* Typed vitaGL implementation of the generated guest GL boundary.
 *
 * Guest and host pointers are both 32-bit and identity-mapped in the Vita
 * runtime.  Pointer-shaped parameters are converted only here, never exposed
 * as untyped native proc addresses to translated x86 code. */
#include "gl_vita_backend.h"
#include "kage_vita_deep_profile.h"
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE && \
    !defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
#error "attribute-enable coalescing requires the typed state cache"
#endif

#include <stdint.h>
#include <string.h>

#if defined(_MSC_VER)
# include <intrin.h>
#endif

#include "gl_bridge.h"
#include "kage_vita_canonical_quads.h"

#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
# include "gl_vita_coloroffset_source.h"
#endif
#if defined(ISAAC_VITA_COLOROFFSET_FS_PROBE) && \
    !defined(ISAAC_GL_VITA_BACKEND_ORACLE) && \
    !defined(ISAAC_GL_VITA_FIRST_FRAME_ORACLE)
/* DIAGNOSTIC 0008 probe: consumer-declared statistics of the stock archive. */
# include "gl_vita_coloroffset_fs_probe.h"
#endif

#if defined(ISAAC_GL_VITA_FIRST_FRAME_ORACLE)
# include "gl_vita_first_frame_oracle_vitagl.h"
#elif defined(ISAAC_GL_VITA_BACKEND_ORACLE)
# include "gl_vita_backend_test_vitagl.h"
# if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE) || \
    defined(ISAAC_VITA_GL_TIME_PROFILE)
uint64_t sceKernelGetProcessTimeWide(void);
# endif
#else
# include <psp2/kernel/processmgr.h>
# include <vitaGL.h>
#endif

#include "kage_vita_io_profile.h"
#include "kage_vita_backend.h"
#include "kage_vita_loading.h"
#include "kage_vita_world_seam_diag.h"

#define GL_VITA_RESOLVED_COUNT 62u
#define GL_VITA_MISSING_COUNT  11u
#define GL_VITA_RBO_CAPACITY   64u

#define GL_VITA_RENDERBUFFER        0x00008d41u
#define GL_VITA_RENDERBUFFER_WIDTH  0x00008d42u
#define GL_VITA_RENDERBUFFER_HEIGHT 0x00008d43u

#define GL_VITA_FRAMEBUFFER      0x00008d40u
#define GL_VITA_READ_FRAMEBUFFER 0x00008ca8u
#define GL_VITA_DRAW_FRAMEBUFFER 0x00008ca9u
#define GL_VITA_VIEWPORT         0x00000ba2u
#define GL_VITA_TEXTURE_2D       0x00000de1u
#define GL_VITA_TEXTURE_BINDING_2D 0x00008069u
#define GL_VITA_TEXTURE_MAG_FILTER 0x00002800u
#define GL_VITA_TEXTURE_MIN_FILTER 0x00002801u
#define GL_VITA_TEXTURE_WRAP_S     0x00002802u
#define GL_VITA_TEXTURE_WRAP_T     0x00002803u
#define GL_VITA_ALPHA            0x00001906u
#define GL_VITA_RED              0x00001903u
#define GL_VITA_RGB              0x00001907u
#define GL_VITA_RGBA             0x00001908u
#define GL_VITA_ABGR_EXT         0x00008000u
#define GL_VITA_BLEND            0x00000be2u
#define GL_VITA_TEXTURE0         0x000084c0u
#define GL_VITA_TEXTURE_UNITS    16u
#define GL_VITA_TEXTURE_CAPACITY 16384u
#define GL_VITA_VERTEX_ATTRIBS   16u
#define GL_VITA_ONE              1u
#define GL_VITA_ONE_MINUS_SRC_ALPHA 0x00000303u

#define GL_VITA_ZERO                  0x00000000u
#define GL_VITA_SRC_COLOR             0x00000300u
#define GL_VITA_ONE_MINUS_SRC_COLOR   0x00000301u
#define GL_VITA_SRC_ALPHA             0x00000302u
#define GL_VITA_DST_ALPHA             0x00000304u
#define GL_VITA_ONE_MINUS_DST_ALPHA   0x00000305u
#define GL_VITA_DST_COLOR             0x00000306u
#define GL_VITA_ONE_MINUS_DST_COLOR   0x00000307u
#define GL_VITA_SRC_ALPHA_SATURATE    0x00000308u

#define GL_VITA_NEVER                 0x00000200u
#define GL_VITA_LESS                  0x00000201u
#define GL_VITA_EQUAL                 0x00000202u
#define GL_VITA_LEQUAL                0x00000203u
#define GL_VITA_GREATER               0x00000204u
#define GL_VITA_NOTEQUAL              0x00000205u
#define GL_VITA_GEQUAL                0x00000206u
#define GL_VITA_ALWAYS                0x00000207u

#define GL_VITA_BYTE                  0x00001400u
#define GL_VITA_UNSIGNED_BYTE         0x00001401u
#define GL_VITA_SHORT                 0x00001402u
#define GL_VITA_UNSIGNED_SHORT        0x00001403u
#define GL_VITA_FLOAT                 0x00001406u
#define GL_VITA_HALF_FLOAT            0x0000140bu
#define GL_VITA_HALF_FLOAT_OES        0x00008d61u

#define GL_VITA_TOKEN_BIND_RENDERBUFFER   0x7e9bfd5cu
#define GL_VITA_TOKEN_DELETE_RENDERBUFFERS 0x7efa9491u
#define GL_VITA_TOKEN_GEN_RENDERBUFFERS    0x7e9316cdu
#define GL_VITA_TOKEN_GET_RBO_PARAMETER    0x7e7972cau
#define GL_VITA_TOKEN_RBO_STORAGE          0x7eca621du

#if defined(ISAAC_VITA_FXRAY_ALPHA_MASK)
/* The FXLayers fx_ray{,_2}.png assets are the only 256x512 images in the
 * frozen 9,194-PNG corpus whose RGB channels are uniformly white.  Their
 * alpha channel is therefore the complete sampled value: vitaGL's GL_ALPHA
 * U8_R111 storage returns (1, 1, 1, A), exactly like their original RGBA8.
 *
 * Keep two CPU alpha shadows so an unexpected mutating/attachment edge can
 * restore ordinary RGBA8 before it reaches native GL.  This remains a net
 * roughly 511 KiB reduction for the two resident textures (768 KiB less
 * texture storage, 256 KiB of bounded shadows and one 1 KiB restore row),
 * without a raw heap allocation. */
# define GL_VITA_FXRAY_WIDTH              256u
# define GL_VITA_FXRAY_HEIGHT             512u
# define GL_VITA_FXRAY_PIXELS             \
    (GL_VITA_FXRAY_WIDTH * GL_VITA_FXRAY_HEIGHT)
# define GL_VITA_FXRAY_RECORDS            2u
# define GL_VITA_FXRAY_ATTACHMENT_RECORDS 64u
#endif

_Static_assert(GL_VITA_RESOLVED_COUNT + GL_VITA_MISSING_COUNT ==
               GUEST_GL_SURFACE_COUNT,
               "Vita GL table must be re-audited when the guest ABI changes");
#if UINTPTR_MAX == UINT32_MAX
_Static_assert(sizeof(IsaacVitaGlRenderTargetTelemetry) == 92u,
               "vitaGL render-target telemetry ABI drifted");
#endif
_Static_assert(ISAAC_VITAGL_RT_EVENT_MAX_BODY < 384u,
               "vitaGL render-target event exceeds isaac_vita_log body");

typedef struct gl_vita_renderbuffer_record {
    guest_gl_uint name;
    guest_gl_enum internal_format;
    guest_gl_sizei width;
    guest_gl_sizei height;
} gl_vita_renderbuffer_record;

static int s_installed;
static guest_gl_uint s_bound_renderbuffer;
static gl_vita_renderbuffer_record s_renderbuffers[GL_VITA_RBO_CAPACITY];
static const char *s_last_error;
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
enum gl_vita_texture_profile_name_bits {
    GL_VITA_TEXTURE_PROFILE_LIVE = 1u,
    GL_VITA_TEXTURE_PROFILE_DEFINED = 2u,
    GL_VITA_TEXTURE_PROFILE_EVER = 4u
};

typedef struct gl_vita_texture_profile_state {
    IsaacVitaTextureChurnProfile window;
    uint8_t names[GL_VITA_TEXTURE_CAPACITY];
    guest_gl_uint bound_2d[GL_VITA_TEXTURE_UNITS];
    uint32_t active_unit;
    uint32_t logical_live;
    uint32_t window_peak_live;
    uint32_t clock_pair_max_us;
    uint32_t lifecycle_incomplete;
} gl_vita_texture_profile_state;

static gl_vita_texture_profile_state s_texture_profile;

/* guest_gl_dispatch serializes all typed-wrapper mutations process-wide and
 * faults nested/concurrent entry before reaching this owner.  The phase-loop
 * snapshot inherits the existing main-thread ownership assumption; future
 * worker-thread GL or snapshot use would require explicit synchronization. */
static void gl_vita_texture_profile_bad(void)
{
    if (s_texture_profile.window.bad != UINT32_MAX)
        ++s_texture_profile.window.bad;
}

static void gl_vita_texture_profile_poison_lifecycle(void)
{
    s_texture_profile.lifecycle_incomplete = 1u;
    gl_vita_texture_profile_bad();
}

static void gl_vita_texture_profile_add(uint32_t *value, uint64_t amount)
{
    if (amount > UINT32_MAX - *value) {
        *value = UINT32_MAX;
        gl_vita_texture_profile_bad();
    } else {
        *value += (uint32_t)amount;
    }
}

static uint32_t gl_vita_texture_profile_elapsed(
    uint64_t started_at, uint64_t ended_at)
{
    uint64_t elapsed;

    if (ended_at < started_at) {
        gl_vita_texture_profile_bad();
        return 0u;
    }
    elapsed = ended_at - started_at;
    if (elapsed > UINT32_MAX) {
        gl_vita_texture_profile_bad();
        return UINT32_MAX;
    }
    return (uint32_t)elapsed;
}

static void gl_vita_texture_profile_reset_all(void)
{
    memset(&s_texture_profile, 0, sizeof s_texture_profile);
}

static void gl_vita_texture_profile_calibrate_clock(void)
{
    uint32_t sample;
    uint32_t maximum = 0u;

    /* One install-time calibration, outside every phase window.  Eight
     * back-to-back pairs sample the same two calls charged to each observed
     * wrapper interval without perturbing room-transition calls further. */
    for (sample = 0u; sample < 8u; ++sample) {
        uint64_t started_at = sceKernelGetProcessTimeWide();
        uint64_t ended_at = sceKernelGetProcessTimeWide();

        if (ended_at < started_at) {
            maximum = UINT32_MAX;
            break;
        }
        if (ended_at - started_at > maximum) {
            maximum = ended_at - started_at > UINT32_MAX ? UINT32_MAX :
                (uint32_t)(ended_at - started_at);
        }
    }
    s_texture_profile.clock_pair_max_us = maximum;
}

static void gl_vita_texture_profile_reset_window(void)
{
    memset(&s_texture_profile.window, 0, sizeof s_texture_profile.window);
    s_texture_profile.window_peak_live = s_texture_profile.logical_live;
}

void gl_vita_backend_texture_churn_profile_take_window(
    IsaacVitaTextureChurnProfile *profile)
{
    uint64_t classified_images;
    uint64_t expected_clock_calls;

    if (!profile)
        return;
    classified_images =
        (uint64_t)s_texture_profile.window.image_first +
        s_texture_profile.window.image_redefine +
        s_texture_profile.window.image_unknown +
        s_texture_profile.window.image_other_level;
    /* Gen/image own one interval; delete owns native and post intervals. */
    expected_clock_calls =
        2u * ((uint64_t)s_texture_profile.window.gen_calls +
              classified_images) +
        4u * (uint64_t)s_texture_profile.window.delete_calls;
    if (s_texture_profile.window.bad == 0u &&
            (expected_clock_calls > UINT32_MAX ||
             s_texture_profile.window.clock_calls != expected_clock_calls))
        gl_vita_texture_profile_bad();
    /* A missed native allocation/deletion can poison every later first/live
     * classification.  Keep subsequent quiet windows visibly incomplete. */
    if (s_texture_profile.lifecycle_incomplete &&
            s_texture_profile.window.bad == 0u)
        gl_vita_texture_profile_bad();
    s_texture_profile.window.logical_live = s_texture_profile.logical_live;
    s_texture_profile.window.window_peak_live =
        s_texture_profile.window_peak_live;
    s_texture_profile.window.clock_pair_max_us =
        s_texture_profile.clock_pair_max_us;
    *profile = s_texture_profile.window;
    gl_vita_texture_profile_reset_window();
}

static void gl_vita_texture_profile_note_active(guest_gl_enum texture)
{
    if (texture >= GL_VITA_TEXTURE0 &&
            texture < GL_VITA_TEXTURE0 + GL_VITA_TEXTURE_UNITS) {
        s_texture_profile.active_unit = texture - GL_VITA_TEXTURE0;
    } else {
        /* The frozen guest never emits this shape.  NO_DEBUG vitaGL omits
         * enum checks, so fail the attribution model closed instead of
         * following a potentially out-of-range native unit. */
        gl_vita_texture_profile_bad();
    }
}

static void gl_vita_texture_profile_note_bind(
    guest_gl_enum target, guest_gl_uint texture)
{
    if (target != GL_VITA_TEXTURE_2D ||
            texture >= GL_VITA_TEXTURE_CAPACITY) {
        gl_vita_texture_profile_bad();
        return;
    }
    s_texture_profile.bound_2d[s_texture_profile.active_unit] = texture;
}

static void gl_vita_texture_profile_note_gen(
    guest_gl_sizei count, guest_gl_addr textures,
    uint32_t value_before, int value_before_valid,
    uint64_t started_at, uint64_t ended_at)
{
    IsaacVitaTextureChurnProfile *window = &s_texture_profile.window;
    uint32_t name;
    uint8_t state;

    gl_vita_texture_profile_add(&window->gen_calls, 1u);
    gl_vita_texture_profile_add(&window->clock_calls, 2u);
    gl_vita_texture_profile_add(
        &window->gen_observed_us,
        gl_vita_texture_profile_elapsed(started_at, ended_at));

    /* The frozen PE has exactly two glGenTextures sites and both request one
     * name.  Reject every wider shape observationally rather than guessing
     * which outputs were written on slot exhaustion. */
    if (count != 1 || !value_before_valid || !textures ||
            textures > UINT32_MAX - 3u) {
        gl_vita_texture_profile_poison_lifecycle();
        return;
    }
    name = ld32(textures);
    if (name == 0u || name >= GL_VITA_TEXTURE_CAPACITY) {
        gl_vita_texture_profile_poison_lifecycle();
        return;
    }
    /* vitaGL's exhaustion path leaves the destination untouched.  An equal
     * before/after value is ambiguous with a legitimate recycled same-name
     * result, so accept neither interpretation and keep the model closed. */
    if (name == value_before) {
        gl_vita_texture_profile_add(&window->gen_ambiguous, 1u);
        gl_vita_texture_profile_poison_lifecycle();
        return;
    }
    state = s_texture_profile.names[name];
    if (state & GL_VITA_TEXTURE_PROFILE_LIVE) {
        /* A newly returned name cannot alias an already-live accepted name;
         * preserve the earlier lifecycle rather than adopting the conflict. */
        gl_vita_texture_profile_poison_lifecycle();
        return;
    }
    gl_vita_texture_profile_add(&window->gen_names, 1u);
    /* Pinned vitaGL 73dd57a scans slots 1..N once; for n=1 the returned name
     * is exactly the number of texture_slots entries examined. */
    gl_vita_texture_profile_add(&window->gen_scan_slots, name);
    if (state & GL_VITA_TEXTURE_PROFILE_EVER)
        gl_vita_texture_profile_add(&window->recycled_names, 1u);
    s_texture_profile.names[name] =
        GL_VITA_TEXTURE_PROFILE_LIVE | GL_VITA_TEXTURE_PROFILE_EVER;
    ++s_texture_profile.logical_live;
    if (s_texture_profile.logical_live > s_texture_profile.window_peak_live)
        s_texture_profile.window_peak_live = s_texture_profile.logical_live;
}

static void gl_vita_texture_profile_note_delete(
    guest_gl_sizei count, guest_gl_addr textures)
{
    IsaacVitaTextureChurnProfile *window = &s_texture_profile.window;
    uint32_t name;
    guest_gl_sizei index;
    uint32_t unit;

    if (count == 0)
        return;
    if (count < 0) {
        gl_vita_texture_profile_bad();
        return;
    }
    if (!textures || (uint32_t)count > GL_VITA_TEXTURE_CAPACITY ||
            (uint64_t)textures + (uint64_t)(uint32_t)count * 4u >
                UINT64_C(0x100000000)) {
        gl_vita_texture_profile_poison_lifecycle();
        return;
    }
    gl_vita_texture_profile_add(&window->delete_names, (uint32_t)count);
    for (index = 0; index < count; ++index) {
        name = ld32(textures + (uint32_t)index * 4u);
        if (name == 0u)
            continue;
        if (name >= GL_VITA_TEXTURE_CAPACITY) {
            gl_vita_texture_profile_bad();
            continue;
        }
        if (s_texture_profile.names[name] & GL_VITA_TEXTURE_PROFILE_LIVE) {
            s_texture_profile.names[name] =
                (uint8_t)(s_texture_profile.names[name] &
                          GL_VITA_TEXTURE_PROFILE_EVER);
            if (s_texture_profile.logical_live != 0u)
                --s_texture_profile.logical_live;
            else
                gl_vita_texture_profile_poison_lifecycle();
        } else {
            /* Legal GL no-op, but outside the observed generated-name set. */
            gl_vita_texture_profile_bad();
        }
        for (unit = 0u; unit < GL_VITA_TEXTURE_UNITS; ++unit)
            if (s_texture_profile.bound_2d[unit] == name)
                s_texture_profile.bound_2d[unit] = 0u;
    }
}

static void gl_vita_texture_profile_note_delete_timing(
    uint64_t native_started_at, uint64_t native_ended_at,
    uint64_t post_started_at, uint64_t post_ended_at)
{
    IsaacVitaTextureChurnProfile *window = &s_texture_profile.window;
    uint32_t native_elapsed = gl_vita_texture_profile_elapsed(
        native_started_at, native_ended_at);
    uint32_t post_elapsed = gl_vita_texture_profile_elapsed(
        post_started_at, post_ended_at);

    gl_vita_texture_profile_add(&window->delete_calls, 1u);
    gl_vita_texture_profile_add(&window->clock_calls, 4u);
    gl_vita_texture_profile_add(
        &window->delete_native_observed_us, native_elapsed);
    gl_vita_texture_profile_add(
        &window->delete_post_observed_us, post_elapsed);
    if (native_elapsed > window->delete_native_max_observed_us)
        window->delete_native_max_observed_us = native_elapsed;
    if (post_elapsed > window->delete_post_max_observed_us)
        window->delete_post_max_observed_us = post_elapsed;
}

static void gl_vita_texture_profile_note_image(
    guest_gl_enum target, guest_gl_int level,
    guest_gl_int internal_format,
    guest_gl_sizei width, guest_gl_sizei height,
    guest_gl_int border, guest_gl_enum format,
    guest_gl_enum type, int native_dispatched,
    uint64_t started_at, uint64_t ended_at)
{
    IsaacVitaTextureChurnProfile *window = &s_texture_profile.window;
    uint32_t name = s_texture_profile.bound_2d[s_texture_profile.active_unit];
    uint32_t elapsed = gl_vita_texture_profile_elapsed(started_at, ended_at);
    uint32_t bytes_per_pixel = 0u;
    uint64_t aligned_width;
    int known_name = target == GL_VITA_TEXTURE_2D && name != 0u &&
        name < GL_VITA_TEXTURE_CAPACITY &&
        (s_texture_profile.names[name] & GL_VITA_TEXTURE_PROFILE_LIVE);
    int linear = native_dispatched && target == GL_VITA_TEXTURE_2D &&
        level == 0 && border == 0 && width > 0 && height > 0 &&
        type == GL_VITA_UNSIGNED_BYTE &&
        internal_format == (guest_gl_int)format &&
        (format == GL_VITA_RED || format == GL_VITA_RGB ||
         format == GL_VITA_RGBA);
    int converted = native_dispatched && target == GL_VITA_TEXTURE_2D &&
        level == 0 && border == 0 && width == 256 && height == 1 &&
        internal_format == (guest_gl_int)GL_VITA_RGBA &&
        format == GL_VITA_ABGR_EXT && type == GL_VITA_UNSIGNED_BYTE;
    int bridge_compacted = !native_dispatched &&
        target == GL_VITA_TEXTURE_2D && level == 0 && border == 0 &&
        width == 256 && height == 512 &&
        internal_format == (guest_gl_int)GL_VITA_RGBA &&
        format == GL_VITA_RGBA && type == GL_VITA_UNSIGNED_BYTE;

    gl_vita_texture_profile_add(&window->clock_calls, 2u);
    gl_vita_texture_profile_add(&window->image_observed_us, elapsed);
    if (elapsed > window->image_max_observed_us)
        window->image_max_observed_us = elapsed;

    /* The buckets below describe observed bridge-call history.  NO_DEBUG
     * vitaGL exposes no mapped-allocation success receipt, so first/redefine
     * and byte totals are not allocation-completion proofs. */
    if (!known_name) {
        gl_vita_texture_profile_add(&window->image_unknown, 1u);
    } else if (level != 0) {
        gl_vita_texture_profile_add(&window->image_other_level, 1u);
    } else if (s_texture_profile.names[name] &
               GL_VITA_TEXTURE_PROFILE_DEFINED) {
        gl_vita_texture_profile_add(&window->image_redefine, 1u);
    } else {
        gl_vita_texture_profile_add(&window->image_first, 1u);
        s_texture_profile.names[name] |= GL_VITA_TEXTURE_PROFILE_DEFINED;
    }

    if (linear) {
        gl_vita_texture_profile_add(&window->path_linear, 1u);
        bytes_per_pixel = format == GL_VITA_RED ? 1u :
            (format == GL_VITA_RGB ? 3u : 4u);
    } else if (converted || bridge_compacted) {
        gl_vita_texture_profile_add(&window->path_converted, 1u);
        bytes_per_pixel = bridge_compacted ? 1u : 4u;
    } else {
        gl_vita_texture_profile_add(&window->path_other, 1u);
    }
    if (bytes_per_pixel != 0u) {
        aligned_width = ((uint64_t)(uint32_t)width + 7u) & ~UINT64_C(7);
        gl_vita_texture_profile_add(
            &window->known_pixels,
            (uint64_t)(uint32_t)width * (uint32_t)height);
        gl_vita_texture_profile_add(
            &window->known_alloc_bytes,
            aligned_width * (uint32_t)height * bytes_per_pixel);
    }
}
#else
# define gl_vita_texture_profile_reset_all() ((void)0)
# define gl_vita_texture_profile_calibrate_clock() ((void)0)
# define gl_vita_texture_profile_reset_window() ((void)0)
# define gl_vita_texture_profile_note_active(texture) ((void)0)
# define gl_vita_texture_profile_note_bind(target, texture) ((void)0)
#endif
#if defined(ISAAC_VITA_FXRAY_ALPHA_MASK)
typedef struct gl_vita_fxray_record {
    guest_gl_uint name;
    guest_gl_uint alignment_padding;
    uint8_t alpha[GL_VITA_FXRAY_PIXELS];
} gl_vita_fxray_record;

static _Alignas(8) gl_vita_fxray_record
    s_fxray_records[GL_VITA_FXRAY_RECORDS];
static guest_gl_uint
    s_fxray_attachments[GL_VITA_FXRAY_ATTACHMENT_RECORDS];
static uint8_t s_fxray_attachment_registry_saturated;
static _Alignas(8) uint8_t
    s_fxray_rgba_row[GL_VITA_FXRAY_WIDTH * 4u];
static uint8_t s_fxray_upload_receipts;
static uint8_t s_fxray_fallback_receipt;

void isaac_vita_log(const char *format, ...);

_Static_assert(sizeof s_fxray_rgba_row == 1024u,
               "FXLayers ray restoration row drifted");
_Static_assert(sizeof s_fxray_records[0].alpha == 131072u,
               "FXLayers ray alpha shadow drifted");
_Static_assert(sizeof(gl_vita_fxray_record) % 8u == 0u,
               "FXLayers ray record stride lost 8-byte alignment");
#endif
#if defined(ISAAC_VITA_DISPLAY_RASTER_720) || \
        defined(ISAAC_VITA_FBO_RASTER_SCALE)
/* Either raster A/B makes the native viewport differ from the guest's
 * logical 960x540 request, so the guest viewport is shadowed and replayed. */
# define GL_VITA_LOGICAL_VIEWPORT 1
#endif
#if defined(GL_VITA_LOGICAL_VIEWPORT) || defined(ISAAC_VITA_FBO_CLEAR_ELISION)
/* Every offscreen-target policy needs the guest's draw-framebuffer binding. */
# define GL_VITA_DRAW_FRAMEBUFFER_SHADOW 1
#endif
#if defined(ISAAC_VITA_FBO_RASTER_SCALE) || defined(ISAAC_VITA_FBO_CLEAR_ELISION)
# define GL_VITA_FBO_TABLE 1
#endif
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION_DEPTH_DROP) && \
        !defined(ISAAC_VITA_FBO_CLEAR_ELISION)
# error "ISAAC_VITA_FBO_CLEAR_ELISION_DEPTH_DROP requires ISAAC_VITA_FBO_CLEAR_ELISION"
#endif
#if defined(GL_VITA_LOGICAL_VIEWPORT)
typedef struct gl_vita_logical_viewport {
    guest_gl_int x;
    guest_gl_int y;
    guest_gl_sizei width;
    guest_gl_sizei height;
} gl_vita_logical_viewport;

static gl_vita_logical_viewport s_logical_viewport;
#endif
#if defined(GL_VITA_DRAW_FRAMEBUFFER_SHADOW)
static guest_gl_uint s_draw_framebuffer;
#endif
#if defined(ISAAC_VITA_PHASE_PROFILE)
IsaacVitaGlPhaseProfileCounters g_isaac_vita_gl_phase_profile_counters;
# define GL_VITA_PHASE_COUNT(field) \
    (++g_isaac_vita_gl_phase_profile_counters.field)
# define GL_VITA_PHASE_ADD(field, value) \
    (g_isaac_vita_gl_phase_profile_counters.field += (uint32_t)(value))
#else
# define GL_VITA_PHASE_COUNT(field) ((void)0)
# define GL_VITA_PHASE_ADD(field, value) ((void)0)
#endif

#if defined(ISAAC_VITA_GL_TIME_PROFILE)
# if !defined(ISAAC_VITA_PHASE_PROFILE)
#  error "ISAAC_VITA_GL_TIME_PROFILE requires ISAAC_VITA_PHASE_PROFILE"
# endif
/* The bracket encloses exactly one native vitaGL call and nothing of our
 * own bookkeeping, so the bucket is what vitaGL plus GXM cost on the CPU
 * (including the wait inside sceGxmEndScene that a clear, a draw-target
 * switch or a present forces when it ends the previous scene; on the device
 * sceGxmBeginScene never waits, ph120.gt bm stays under 0.1 ms).  Two 64-bit
 * clock reads per wrapped call. */
IsaacVitaGlTimeProfile g_isaac_vita_gl_time_profile;
# if !defined(ISAAC_VITA_GL_TIME_SDK_SPARSE) || !ISAAC_VITA_GL_TIME_SDK_SPARSE
static inline uint64_t gl_vita_time_now(void)
{
    return sceKernelGetProcessTimeWide();
}
# define GL_VITA_TIME_BEGIN() \
    uint64_t gl_vita_time_started_at = gl_vita_time_now()
#  if defined(ISAAC_VITA_GL_WRAPPER_TIME)
/* The ph120.gd draw split reuses this bracket's end read: after END the
 * bracket local holds the end time (one clock read per boundary, see
 * GL_VITA_DRAW_SPLIT_BODY_DONE below).  Same reads as without the define. */
#  define GL_VITA_TIME_END(bucket) \
    do { \
        uint64_t gl_vita_time_ended_at = gl_vita_time_now(); \
        isaac_vita_gl_time_add(&g_isaac_vita_gl_time_profile.bucket, \
                               gl_vita_time_started_at, \
                               gl_vita_time_ended_at); \
        gl_vita_time_started_at = gl_vita_time_ended_at; \
    } while (0)
#  else
# define GL_VITA_TIME_END(bucket) \
    isaac_vita_gl_time_add(&g_isaac_vita_gl_time_profile.bucket, \
                           gl_vita_time_started_at, gl_vita_time_now())
#  endif
# else
/* Sparse mode observes native SDK sites, not these high-frequency wrappers. */
# define GL_VITA_TIME_BEGIN() ((void)0)
# define GL_VITA_TIME_END(bucket) ((void)0)
# endif
#else
# define GL_VITA_TIME_BEGIN() ((void)0)
# define GL_VITA_TIME_END(bucket) ((void)0)
#endif

#if defined(ISAAC_VITA_GL_WRAPPER_TIME)
/* ph120.gd draw split (legend in gl_vita_backend.h): a running cursor from
 * the wrapper body start with one gl_vita_time_now() read per boundary.
 * ENTER charges `disp` from the gl_bridge entry read; MARK closes a part at
 * a fresh read; AT closes a part at a read the gt bracket already made (its
 * BEGIN local, or the END time that local holds under this define);
 * BODY_DONE closes `gl` on the END read and publishes it so gl_bridge can
 * charge `tail` at its exit read.  Nothing expands without the define. */
# define GL_VITA_DRAW_SPLIT_ENTER() \
    uint64_t gl_vita_split_at = gl_vita_time_now(); \
    isaac_vita_gl_wrapper_time_add( \
        &g_isaac_vita_gl_wrapper_time.draw[ \
            ISAAC_VITA_GL_DRAW_SPLIT_DISPATCH], \
        g_isaac_vita_gl_wrapper_entry_at, gl_vita_split_at)
# define GL_VITA_DRAW_SPLIT_AT(part, at) \
    do { \
        uint64_t gl_vita_split_now = (at); \
        isaac_vita_gl_wrapper_time_add( \
            &g_isaac_vita_gl_wrapper_time.draw[ \
                ISAAC_VITA_GL_DRAW_SPLIT_##part], \
            gl_vita_split_at, gl_vita_split_now); \
        gl_vita_split_at = gl_vita_split_now; \
    } while (0)
# define GL_VITA_DRAW_SPLIT_MARK(part) \
    GL_VITA_DRAW_SPLIT_AT(part, gl_vita_time_now())
# define GL_VITA_DRAW_SPLIT_BODY_DONE() \
    do { \
        GL_VITA_DRAW_SPLIT_AT(BODY, gl_vita_time_started_at); \
        g_isaac_vita_gl_draw_body_ended_at = gl_vita_split_at; \
    } while (0)
# define GL_VITA_DRAW_SPLIT_CANONICAL() \
    ((void)++g_isaac_vita_gl_wrapper_time.draw_canonical)
#else
# define GL_VITA_DRAW_SPLIT_ENTER() ((void)0)
# define GL_VITA_DRAW_SPLIT_AT(part, at) ((void)0)
# define GL_VITA_DRAW_SPLIT_MARK(part) ((void)0)
# define GL_VITA_DRAW_SPLIT_BODY_DONE() ((void)0)
# define GL_VITA_DRAW_SPLIT_CANONICAL() ((void)0)
#endif

/* Shim-side glGetAttribLocation/glGetUniformLocation memo
 * (ISAAC_VITA_GL_LOCATION_CACHE, part of ISAAC_VITA_GL_SHIM_FASTDISPATCH).
 *
 * Pinned vitaGL 73dd57a answers both queries as a pure function of the
 * program object's state: glGetAttribLocation reads p->vshader->prog and
 * p->attr[]/attr_highest_idx (custom_shaders.c:4317), glGetUniformLocation
 * reads p->vshader->prog, p->fshader->prog and the vert/frag uniform lists
 * (custom_shaders.c:3502); neither call mutates anything or sets a GL error.
 * Every guest-reachable mutator of that state is in the typed surface and
 * bumps the single cache generation before it runs: glAttachShader (rebinds
 * the shader pointers), glLinkProgram (rebuilds attr[] and the uniform
 * lists), glDeleteProgram/glCreateProgram (slot reuse), glCompileShader/
 * glShaderSource/glDeleteShader (replace the GXP an attached program reads).
 * glBindAttribLocation is not in the registry, so the guest cannot reach it.
 * A hit therefore returns exactly the value vitaGL returned for the same
 * (program, name bytes) since the last mutation, including -1 for unknown
 * names.  Keys copy the ASCII name bytes (never the pointer) from the same
 * identity-mapped guest memory vitaGL reads.  Fail-open: program 0, a NULL,
 * empty, non-printable or >63-byte name, and any table miss reach vitaGL. */
#define GL_VITA_LOCATION_KIND_ATTRIB 1u
#define GL_VITA_LOCATION_KIND_UNIFORM 2u
#define GL_VITA_LOCATION_NAME_MAX 63u

typedef struct gl_vita_location_cache_key {
    uint32_t hash;
    uint32_t slot;
    guest_gl_uint program;
    uint8_t kind;
    uint8_t length;
    uint8_t cacheable;
    char name[GL_VITA_LOCATION_NAME_MAX + 1u];
} gl_vita_location_cache_key;

#if defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO)
/* ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO (gl_vita_backend.h legend): one
 * monotonic word the replay memo compares against.  It rides the two
 * location-cache entry points below so the memo cannot outlive a mutation
 * the shim cache would not survive either; the wrap-around skips zero (the
 * memo's "never filled" marker).  Independent of ISAAC_VITA_GL_LOCATION_CACHE:
 * without that option the entry points are these bumps and nothing else. */
uint32_t g_isaac_vita_gl_location_generation = 1u;
# define GL_VITA_LOCATION_GENERATION_BUMP() \
    do { \
        if (++g_isaac_vita_gl_location_generation == 0u) \
            g_isaac_vita_gl_location_generation = 1u; \
    } while (0)
# if defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_VERIFY)
void isaac_vita_gl_location_memo_note_mismatch(void)
{
    GL_VITA_PHASE_COUNT(location_cache_mismatch);
}
# endif
#else
# define GL_VITA_LOCATION_GENERATION_BUMP() ((void)0)
#endif

#if defined(ISAAC_VITA_GL_LOCATION_CACHE)
#define GL_VITA_LOCATION_CACHE_CAPACITY 256u
#define GL_VITA_LOCATION_ADDRESS_CAPACITY 64u

typedef struct gl_vita_location_cache_entry {
    uint32_t generation;        /* 0 = never written */
    uint32_t hash;
    guest_gl_uint program;
    guest_gl_int location;
    uint8_t kind;
    uint8_t length;
    char name[GL_VITA_LOCATION_NAME_MAX + 1u];
} gl_vita_location_cache_entry;

/* An address is only a hint to the existing content-cache slot, never proof
 * that its string is unchanged.  No second copy of a location/name is kept. */
typedef struct gl_vita_location_address_entry {
    guest_gl_addr address;
    uint32_t slot;
} gl_vita_location_address_entry;

_Static_assert((GL_VITA_LOCATION_CACHE_CAPACITY &
                (GL_VITA_LOCATION_CACHE_CAPACITY - 1u)) == 0u,
               "GL location cache capacity must be a power of two");
_Static_assert((GL_VITA_LOCATION_ADDRESS_CAPACITY &
                (GL_VITA_LOCATION_ADDRESS_CAPACITY - 1u)) == 0u,
               "GL location address capacity must be a power of two");
_Static_assert(sizeof(gl_vita_location_address_entry) == 8u,
               "GL location address hints must remain eight bytes each");

static gl_vita_location_cache_entry
    s_gl_location_cache[GL_VITA_LOCATION_CACHE_CAPACITY];
static gl_vita_location_address_entry
    s_gl_location_addresses[GL_VITA_LOCATION_ADDRESS_CAPACITY];
static uint32_t s_gl_location_generation = 1u;

static void gl_vita_location_cache_invalidate(void)
{
    /* One monotonic generation covers every program: mutations happen at
     * load time, so a whole-table miss costs one extra native query per
     * (program, name) and never a stale answer. */
    GL_VITA_LOCATION_GENERATION_BUMP();
    memset(s_gl_location_addresses, 0, sizeof s_gl_location_addresses);
    if (++s_gl_location_generation == 0u) {
        memset(s_gl_location_cache, 0, sizeof s_gl_location_cache);
        s_gl_location_generation = 1u;
    }
}

static void gl_vita_location_cache_reset(void)
{
    GL_VITA_LOCATION_GENERATION_BUMP();
    memset(s_gl_location_cache, 0, sizeof s_gl_location_cache);
    memset(s_gl_location_addresses, 0, sizeof s_gl_location_addresses);
    s_gl_location_generation = 1u;
}

static int gl_vita_location_cache_lookup(
    uint32_t kind, guest_gl_uint program, guest_gl_addr name,
    gl_vita_location_cache_key *key, guest_gl_int *location)
{
    uint32_t hash;
    uint32_t length = 0u;
    uint32_t address_slot;
    gl_vita_location_address_entry *address_entry;
    const gl_vita_location_cache_entry *entry;

    key->cacheable = 0u;
    if (!program || !name)
        return 0;
    address_slot = (name ^ (name >> 6) ^ (name >> 12) ^ program ^ kind) &
        (GL_VITA_LOCATION_ADDRESS_CAPACITY - 1u);
    address_entry = &s_gl_location_addresses[address_slot];
    if (address_entry->address == name &&
            address_entry->slot < GL_VITA_LOCATION_CACHE_CAPACITY) {
        entry = &s_gl_location_cache[address_entry->slot];
        if (entry->generation == s_gl_location_generation &&
                entry->program == program && entry->kind == (uint8_t)kind &&
                entry->length && entry->length <= GL_VITA_LOCATION_NAME_MAX) {
            uint32_t i;
            /* Read only through the current string's first mismatch/NUL.
             * A fixed-size memcmp could overread a shortened mutable name.
             * Checking the terminating NUL also rejects a longer new name. */
            for (i = 0u; i < entry->length; ++i) {
                uint8_t byte = ld8(name + i);
                if (!byte || byte != (uint8_t)entry->name[i])
                    break;
            }
            if (i == entry->length && !ld8(name + i)) {
                *location = entry->location;
                return 1;
            }
        }
    }
    /* Any address/content mismatch falls through to the unchanged memo.
     * Invalid, empty, long and non-ASCII names still reach native vitaGL. */
    hash = UINT32_C(2166136261) ^ (program * UINT32_C(0x9e3779b9));
    hash = (hash ^ kind) * UINT32_C(16777619);
    for (;;) {
        uint8_t byte = ld8(name + length);
        if (!byte)
            break;
        if (byte < 0x20u || byte > 0x7eu ||
                length == GL_VITA_LOCATION_NAME_MAX)
            return 0;
        key->name[length++] = (char)byte;
        hash = (hash ^ byte) * UINT32_C(16777619);
    }
    if (!length)
        return 0;
    key->name[length] = '\0';
    key->hash = hash;
    key->slot = (hash ^ (hash >> 16)) &
        (GL_VITA_LOCATION_CACHE_CAPACITY - 1u);
    key->program = program;
    key->kind = (uint8_t)kind;
    key->length = (uint8_t)length;
    key->cacheable = 1u;
    /* On a content miss the caller fills this slot after its native query.
     * Until then this is only a hint: every use validates the slot anew. */
    address_entry->address = name;
    address_entry->slot = key->slot;
    entry = &s_gl_location_cache[key->slot];
    if (entry->generation != s_gl_location_generation ||
            entry->hash != hash || entry->program != program ||
            entry->kind != (uint8_t)kind || entry->length != (uint8_t)length ||
            memcmp(entry->name, key->name, length) != 0)
        return 0;
    *location = entry->location;
    return 1;
}

static void gl_vita_location_cache_store(
    const gl_vita_location_cache_key *key, guest_gl_int location)
{
    gl_vita_location_cache_entry *entry;

    if (!key->cacheable)
        return;
    entry = &s_gl_location_cache[key->slot];
    entry->generation = s_gl_location_generation;
    entry->hash = key->hash;
    entry->program = key->program;
    entry->location = location;
    entry->kind = key->kind;
    entry->length = key->length;
    memcpy(entry->name, key->name, (size_t)key->length + 1u);
}
#else
static inline int gl_vita_location_cache_lookup(
    uint32_t kind, guest_gl_uint program, guest_gl_addr name,
    gl_vita_location_cache_key *key, guest_gl_int *location)
{
    (void)kind;
    (void)program;
    (void)name;
    (void)location;
    key->cacheable = 0u;
    return 0;
}

static inline void gl_vita_location_cache_store(
    const gl_vita_location_cache_key *key, guest_gl_int location)
{
    (void)key;
    (void)location;
}

# define gl_vita_location_cache_invalidate() GL_VITA_LOCATION_GENERATION_BUMP()
# define gl_vita_location_cache_reset() GL_VITA_LOCATION_GENERATION_BUMP()
#endif

#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
/* Exact limits and enum sets are copied from the pinned vitaGL f4b23b6
 * sources (shared.h, textures.c, blending.c, tests.c and custom_shaders.c).
 * The production NO_DEBUG build makes its native invalid paths especially
 * important: unknown or invalid calls always pass through and poison only the
 * shadow which that call could have changed. */
typedef struct gl_vita_typed_attrib_pointer {
    guest_gl_addr pointer;
    guest_gl_enum type;
    guest_gl_int size;
    guest_gl_sizei stride;
    guest_gl_boolean normalized;
} gl_vita_typed_attrib_pointer;

typedef struct gl_vita_typed_state {
    guest_gl_uint texture_2d[GL_VITA_TEXTURE_UNITS];
    gl_vita_typed_attrib_pointer attrib_pointer[GL_VITA_VERTEX_ATTRIBS];
    guest_gl_enum blend[4];
    guest_gl_enum depth;
    guest_gl_int viewport_x;
    guest_gl_int viewport_y;
    guest_gl_sizei viewport_width;
    guest_gl_sizei viewport_height;
    uint16_t texture_known;
    uint16_t attrib_enable_known;
    uint16_t attrib_enabled;
    uint16_t attrib_pointer_known;
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
    uint16_t attrib_applied;
    uint16_t attrib_pending;
#endif
    uint8_t active_texture;
    uint8_t active_texture_known;
    uint8_t blend_known;
    uint8_t depth_known;
    uint8_t viewport_known;
} gl_vita_typed_state;

static gl_vita_typed_state s_gl_typed_state;

#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
#if defined(_MSC_VER)
#define GL_VITA_ATTRIB_NOINLINE __declspec(noinline)
#elif defined(__GNUC__)
#define GL_VITA_ATTRIB_NOINLINE __attribute__((noinline))
#else
#define GL_VITA_ATTRIB_NOINLINE
#endif
/* Keep the register-saving native-call loop off the common empty path. */
static GL_VITA_ATTRIB_NOINLINE void gl_vita_attrib_sync_pending(void)
{
    while (s_gl_typed_state.attrib_pending) {
        unsigned index;
        uint16_t bit;
#if defined(_MSC_VER)
        unsigned long first;
        (void)_BitScanForward(&first, s_gl_typed_state.attrib_pending);
        index = (unsigned)first;
#else
        index = (unsigned)__builtin_ctz(
            (unsigned)s_gl_typed_state.attrib_pending);
#endif
        bit = (uint16_t)(UINT16_C(1) << index);
        GL_VITA_PHASE_COUNT(attrib_toggle);
        {
            GL_VITA_TIME_BEGIN();
            if (s_gl_typed_state.attrib_enabled & bit)
                glEnableVertexAttribArray((GLuint)index);
            else
                glDisableVertexAttribArray((GLuint)index);
            GL_VITA_TIME_END(state);
        }
        s_gl_typed_state.attrib_applied ^= bit;
        s_gl_typed_state.attrib_pending &= (uint16_t)~bit;
        GL_VITA_PHASE_COUNT(typed_state_miss);
    }
}
#undef GL_VITA_ATTRIB_NOINLINE

void gl_vita_backend_attrib_sync(void)
{
    if (s_gl_typed_state.attrib_pending)
        gl_vita_attrib_sync_pending();
}

uint32_t gl_vita_backend_attrib_pending(void)
{
    unsigned bits = s_gl_typed_state.attrib_pending;
    uint32_t count = 0;
    while (bits) {
        bits &= bits - 1u;
        ++count;
    }
    return count;
}

void gl_vita_backend_attrib_external_begin(void)
{
    gl_vita_backend_attrib_sync();
    s_gl_typed_state.attrib_enable_known = 0u;
    s_gl_typed_state.attrib_pointer_known = 0u;
}

/* Only a changed, already-known, valid bit enters here. Unknown valid bits
 * still use the original immediate setter; never assume native default state.
 * Valid native setters modify only the VAO mask, with no allocation/GPU work. */
static void gl_vita_attrib_defer(uint16_t bit)
{
    s_gl_typed_state.attrib_enabled ^= bit;
    GL_VITA_PHASE_COUNT(attrib_deferred);
    if ((s_gl_typed_state.attrib_enabled ^
            s_gl_typed_state.attrib_applied) & bit) {
        s_gl_typed_state.attrib_pending |= bit;
    } else {
        s_gl_typed_state.attrib_pending &= (uint16_t)~bit;
        GL_VITA_PHASE_ADD(attrib_cancelled, 2u);
    }
}
#endif

static void gl_vita_typed_state_reset(void)
{
    gl_vita_backend_attrib_sync();
    memset(&s_gl_typed_state, 0, sizeof s_gl_typed_state);
}

static void gl_vita_typed_invalidate_viewport(void)
{
    s_gl_typed_state.viewport_known = 0u;
}

static int gl_vita_typed_valid_blend_factor(guest_gl_enum factor)
{
    switch (factor) {
    case GL_VITA_ZERO:
    case GL_VITA_ONE:
    case GL_VITA_SRC_COLOR:
    case GL_VITA_ONE_MINUS_SRC_COLOR:
    case GL_VITA_SRC_ALPHA:
    case GL_VITA_ONE_MINUS_SRC_ALPHA:
    case GL_VITA_DST_ALPHA:
    case GL_VITA_ONE_MINUS_DST_ALPHA:
    case GL_VITA_DST_COLOR:
    case GL_VITA_ONE_MINUS_DST_COLOR:
    case GL_VITA_SRC_ALPHA_SATURATE:
        return 1;
    default:
        return 0;
    }
}

static int gl_vita_typed_valid_depth_function(guest_gl_enum function)
{
    return function >= GL_VITA_NEVER && function <= GL_VITA_ALWAYS;
}

static int gl_vita_typed_valid_attrib_type(guest_gl_enum type)
{
    switch (type) {
    case GL_VITA_HALF_FLOAT:
    case GL_VITA_HALF_FLOAT_OES:
    case GL_VITA_FLOAT:
    case GL_VITA_SHORT:
    case GL_VITA_UNSIGNED_SHORT:
    case GL_VITA_BYTE:
    case GL_VITA_UNSIGNED_BYTE:
        return 1;
    default:
        return 0;
    }
}

static int gl_vita_typed_attrib_pointer_equal(
    const gl_vita_typed_attrib_pointer *state,
    guest_gl_int size, guest_gl_enum type, guest_gl_boolean normalized,
    guest_gl_sizei stride, guest_gl_addr pointer)
{
    return state->pointer == pointer && state->type == type &&
        state->size == size && state->normalized == normalized &&
        state->stride == stride;
}
#else
# define gl_vita_typed_state_reset() ((void)0)
# define gl_vita_typed_invalidate_viewport() ((void)0)
#endif

#if defined(ISAAC_VITA_PHASE_PROFILE)
typedef struct gl_vita_fusion_profile_state {
    guest_gl_uint textures[GL_VITA_TEXTURE_UNITS];
    guest_gl_uint framebuffer;
    guest_gl_uint program;
    guest_gl_uint previous_texture;
    guest_gl_uint previous_program;
    guest_gl_enum blend_source_rgb;
    guest_gl_enum blend_destination_rgb;
    uint8_t active_texture;
    uint8_t blend_enabled;
    uint8_t previous_draw_valid;
    uint8_t additive_run_active;
} gl_vita_fusion_profile_state;

static gl_vita_fusion_profile_state s_fusion_profile;

static void gl_vita_fusion_profile_reset(void)
{
    memset(&s_fusion_profile, 0, sizeof s_fusion_profile);
}

#if defined(ISAAC_VITA_GL_FILL_CENSUS)
static void gl_vita_fill_census_reset_window(void);
#endif

void gl_vita_backend_phase_profile_window_boundary(void)
{
    s_fusion_profile.previous_draw_valid = 0u;
    s_fusion_profile.additive_run_active = 0u;
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    gl_vita_texture_profile_reset_window();
#endif
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    gl_vita_fill_census_reset_window();
#endif
}

static void gl_vita_fusion_profile_draw(
    guest_gl_sizei count, guest_gl_enum type)
{
    IsaacVitaGlPhaseProfileCounters *c =
        &g_isaac_vita_gl_phase_profile_counters;
    guest_gl_uint texture = s_fusion_profile.textures[0];
    int additive = s_fusion_profile.blend_enabled &&
        s_fusion_profile.blend_destination_rgb == GL_VITA_ONE;

    /* This is the guest-requested GL stream, measured before any native
     * redundancy suppression.  Same-atlas and additive draws/runs therefore
     * describe the byte-preserving fusion ceiling, not a fused implementation. */
    if (count > 0) {
        uint32_t width = type == KAGE_VITA_GL_UNSIGNED_SHORT ? 2u :
            type == 0x00001405u ? 4u : type == 0x00001401u ? 1u : 0u;
        if (width && (uint32_t)count <= UINT32_MAX / width)
            c->fusion_index_bytes += (uint32_t)count * width;
    }
    if (!s_fusion_profile.blend_enabled)
        c->fusion_blend_other++;
    else if (additive)
        c->fusion_blend_additive++;
    else if (s_fusion_profile.blend_destination_rgb ==
            GL_VITA_ONE_MINUS_SRC_ALPHA)
        c->fusion_blend_alpha++;
    else
        c->fusion_blend_other++;
    if (s_fusion_profile.framebuffer)
        c->fusion_fbo_offscreen++;
    else
        c->fusion_fbo_default++;
    if (s_fusion_profile.previous_draw_valid) {
        if (s_fusion_profile.previous_program == s_fusion_profile.program)
            c->fusion_shader_same++;
        else
            c->fusion_shader_change++;
        if (s_fusion_profile.previous_texture == texture)
            c->fusion_atlas_same++;
        else
            c->fusion_atlas_change++;
    }
    if (additive) {
        c->fusion_additive_draws++;
        if (!s_fusion_profile.additive_run_active)
            c->fusion_additive_runs++;
        s_fusion_profile.additive_run_active = 1u;
    } else {
        s_fusion_profile.additive_run_active = 0u;
    }
    s_fusion_profile.previous_program = s_fusion_profile.program;
    s_fusion_profile.previous_texture = texture;
    s_fusion_profile.previous_draw_valid = 1u;
}
#else
# define gl_vita_fusion_profile_reset() ((void)0)
# define gl_vita_fusion_profile_draw(count, type) ((void)0)
void gl_vita_backend_phase_profile_window_boundary(void)
{
}
#endif

#if defined(ISAAC_VITA_GL_FILL_CENSUS)
# if !defined(ISAAC_VITA_PHASE_PROFILE)
#  error "ISAAC_VITA_GL_FILL_CENSUS requires ISAAC_VITA_PHASE_PROFILE"
# endif
/* GL fill census (ph120.fa / ph120.fp; field legend in gl_vita_backend.h).
 *
 * Everything below is shim-side shadow state written at the top of the typed
 * wrappers, before any early return (typed-state hits, redundancy skips), so
 * the shadows follow the guest's requested state even when the native call
 * is suppressed.  The per-draw arithmetic (three index loads, two or three
 * float loads and one affine transform per vertex, one cross product per
 * triangle) runs inside vita_glDrawElements and must stay free of timers,
 * logging and native GL: the test_kage_vita_phase_profile.py hot-path guard
 * pins that.  The viewport shadow is never invalidated at glBindFramebuffer
 * (GL viewport state persists across binds); draws before the first
 * glViewport count as miss.  Canonical zero-copy draws never read the guest
 * index array (the producer is pinned by kage_vita_canonical_quads.h), so
 * their indices are synthesized from the same base+{0,2,1,1,2,3} pattern.
 * The pass model mirrors vitaGL's scene_reset predicate: a pass is a run of
 * draws/clears on one framebuffer; it ends when a draw or clear targets a
 * different framebuffer, when a non-zero colour texture is attached to the
 * pass framebuffer while bound, at glReadPixels on the pass framebuffer and
 * when the framebuffer is deleted; the present resets the ordinal.  Clears
 * absorbed by ISAAC_VITA_FBO_CLEAR_ELISION never reach vitaGL and are not
 * counted; the owed depth/stencil clears it later issues natively
 * (gl_vita_fbo_owed_materialize) are counted on the framebuffer that owed
 * them, which is where vitaGL runs the quad.  Display clears and draws are
 * measured on the native 960x544 panel in the guest's logical units; the
 * scaled display rasters are not modelled, hence the fail-closed error. */
# if defined(ISAAC_VITA_DISPLAY_RASTER_720)
#  error "ISAAC_VITA_GL_FILL_CENSUS measures the native 960x544 display; ISAAC_VITA_DISPLAY_RASTER 720/480 is not modelled"
# endif
# define GL_VITA_FILL_PROGRAMS          64u
# define GL_VITA_FILL_FRAMEBUFFERS      64u
# define GL_VITA_FILL_PASSES            5u
# define GL_VITA_FILL_PASS_DISPLAY      4u
# define GL_VITA_FILL_PASS_LAST         3u
# define GL_VITA_FILL_NAME_MAX          63u
# define GL_VITA_FILL_COORD_LIMIT       65536.0f
# define GL_VITA_FILL_COLOR_ATTACHMENT0 0x00008ce0u
# define GL_VITA_FILL_UNSIGNED_INT      0x00001405u
# define GL_VITA_FILL_DISPLAY_WIDTH     960u
# define GL_VITA_FILL_DISPLAY_HEIGHT    544u

enum gl_vita_fill_skip {
    GL_VITA_FILL_SKIP_NONE = 0,
    GL_VITA_FILL_SKIP_MODE,     /* not GL_TRIANGLES or count % 3 != 0 */
    GL_VITA_FILL_SKIP_INDICES,  /* index type or array not readable */
    GL_VITA_FILL_SKIP_VIEWPORT, /* no glViewport seen yet, or empty */
    GL_VITA_FILL_SKIP_PROGRAM,  /* program, Position or Transform unknown */
    GL_VITA_FILL_SKIP_ATTRIB    /* Position attribute disabled/wrong shape */
};

typedef struct gl_vita_fill_program {
    guest_gl_uint name;
    int32_t position_location;
    int32_t transform_location;
    uint8_t valid;
    uint8_t pixelation;
    uint8_t transform_known;
    uint8_t reserved;
    float transform[16];
} gl_vita_fill_program;

typedef struct gl_vita_fill_attrib {
    guest_gl_addr pointer;
    guest_gl_sizei stride;
    guest_gl_int size;
    guest_gl_enum type;
} gl_vita_fill_attrib;

typedef struct gl_vita_fill_framebuffer_texture {
    guest_gl_uint framebuffer;
    guest_gl_uint texture;
} gl_vita_fill_framebuffer_texture;

typedef struct gl_vita_fill_state {
    IsaacVitaGlFillCensus window;
    gl_vita_fill_program programs[GL_VITA_FILL_PROGRAMS];
    gl_vita_fill_attrib attribs[GL_VITA_VERTEX_ATTRIBS];
    gl_vita_fill_framebuffer_texture attachments[GL_VITA_FILL_FRAMEBUFFERS];
    guest_gl_int viewport_x;
    guest_gl_int viewport_y;
    guest_gl_sizei viewport_width;
    guest_gl_sizei viewport_height;
    guest_gl_uint pass_framebuffer;
    uint32_t pass_offscreen_begun;
    uint16_t attrib_enabled;
    uint8_t viewport_known;
    uint8_t pass_open;
    uint8_t pass_dirty;
    uint8_t pass_index;
    /* Level-0 glTexImage2D size per texture name (64 KiB, diagnostic). */
    uint16_t texture_width[GL_VITA_TEXTURE_CAPACITY];
    uint16_t texture_height[GL_VITA_TEXTURE_CAPACITY];
} gl_vita_fill_state;

static gl_vita_fill_state s_fill;

# if defined(ISAAC_VITA_GL_FILL_CENSUS_DUMP)
/* One-frame draw list.  Armed from the take-window call (report time) when
 * the window's render p50 reaches the threshold, or at a forced window;
 * capture runs from the next present to the one after; the lines leave
 * through isaac_vita_log at the following take-window call, so no wrapper
 * ever logs.  At most ISAAC_VITA_GL_FILL_CENSUS_DUMP_FRAMES frames per
 * launch. */
#  if !defined(ISAAC_VITA_GL_FILL_CENSUS_DUMP_RND_US)
#   define ISAAC_VITA_GL_FILL_CENSUS_DUMP_RND_US 60000u
#  endif
#  if !defined(ISAAC_VITA_GL_FILL_CENSUS_DUMP_MIN_WIN)
#   define ISAAC_VITA_GL_FILL_CENSUS_DUMP_MIN_WIN 30u
#  endif
#  if !defined(ISAAC_VITA_GL_FILL_CENSUS_DUMP_WIN)
#   define ISAAC_VITA_GL_FILL_CENSUS_DUMP_WIN 0u
#  endif
#  if !defined(ISAAC_VITA_GL_FILL_CENSUS_DUMP_FRAMES)
#   define ISAAC_VITA_GL_FILL_CENSUS_DUMP_FRAMES 2u
#  endif
#  define GL_VITA_FILL_DUMP_CAPACITY 192u
#  define GL_VITA_FILL_DUMP_DRAW  1u
#  define GL_VITA_FILL_DUMP_CLEAR 2u
void isaac_vita_log(const char *format, ...);

typedef struct gl_vita_fill_dump_entry {
    uint32_t rva;
    uint32_t program;
    uint32_t framebuffer;
    uint32_t texture;
    uint32_t kpx;
    uint32_t triangles;
    uint32_t mask;
    int32_t box[4];
    int32_t viewport[4];
    uint16_t texture_width;
    uint16_t texture_height;
    uint16_t blend_source;
    uint16_t blend_destination;
    uint8_t kind;
    uint8_t pass_index;
    uint8_t blend_enabled;
    uint8_t skip;
    uint8_t canonical;
    uint8_t reserved[3];
} gl_vita_fill_dump_entry;

typedef struct gl_vita_fill_dump_state {
    gl_vita_fill_dump_entry entries[GL_VITA_FILL_DUMP_CAPACITY];
    uint32_t count;
    uint32_t overflow;
    uint32_t frame;
    uint32_t frames_done;
    uint8_t armed;
    uint8_t capturing;
    uint8_t complete;
    uint8_t reserved;
} gl_vita_fill_dump_state;

static gl_vita_fill_dump_state s_fill_dump;

static gl_vita_fill_dump_entry *gl_vita_fill_dump_append(uint8_t kind)
{
    gl_vita_fill_dump_entry *entry;

    if (!s_fill_dump.capturing)
        return NULL;
    if (s_fill_dump.count >= GL_VITA_FILL_DUMP_CAPACITY) {
        ++s_fill_dump.overflow;
        return NULL;
    }
    entry = &s_fill_dump.entries[s_fill_dump.count++];
    memset(entry, 0, sizeof *entry);
    entry->kind = kind;
    entry->pass_index = s_fill.pass_index;
    entry->framebuffer = s_fusion_profile.framebuffer;
    entry->program = s_fusion_profile.program;
    return entry;
}

static int32_t gl_vita_fill_dump_coordinate(float value)
{
    if (!(value > -32768.0f))
        return -32768;
    if (!(value < 32767.0f))
        return 32767;
    return (int32_t)value;
}

static void gl_vita_fill_dump_flush(void)
{
    uint32_t index;

    if (!s_fill_dump.complete)
        return;
    for (index = 0u; index < s_fill_dump.count; ++index) {
        const gl_vita_fill_dump_entry *entry = &s_fill_dump.entries[index];

        if (entry->kind == GL_VITA_FILL_DUMP_DRAW) {
            isaac_vita_log(
                "KAGE VITA FILL DUMP f=%u i=%u D ord=%u fb=%u prog=%u "
                "tex=%u/%ux%u bl=%u,%x,%x tri=%u kpx=%u box=%d,%d,%d,%d "
                "vp=%d,%d,%d,%d syn=%u skip=%u rva=%08x",
                (unsigned)s_fill_dump.frame, (unsigned)index,
                (unsigned)entry->pass_index, (unsigned)entry->framebuffer,
                (unsigned)entry->program, (unsigned)entry->texture,
                (unsigned)entry->texture_width,
                (unsigned)entry->texture_height,
                (unsigned)entry->blend_enabled,
                (unsigned)entry->blend_source,
                (unsigned)entry->blend_destination,
                (unsigned)entry->triangles, (unsigned)entry->kpx,
                (int)entry->box[0], (int)entry->box[1],
                (int)entry->box[2], (int)entry->box[3],
                (int)entry->viewport[0], (int)entry->viewport[1],
                (int)entry->viewport[2], (int)entry->viewport[3],
                (unsigned)entry->canonical, (unsigned)entry->skip,
                (unsigned)entry->rva);
        } else {
            isaac_vita_log(
                "KAGE VITA FILL DUMP f=%u i=%u C ord=%u fb=%u mask=%x "
                "att=%ux%u",
                (unsigned)s_fill_dump.frame, (unsigned)index,
                (unsigned)entry->pass_index, (unsigned)entry->framebuffer,
                (unsigned)entry->mask, (unsigned)entry->texture_width,
                (unsigned)entry->texture_height);
        }
    }
    isaac_vita_log(
        "KAGE VITA FILL DUMP f=%u end n=%u trunc=%u",
        (unsigned)s_fill_dump.frame, (unsigned)s_fill_dump.count,
        (unsigned)s_fill_dump.overflow);
    s_fill_dump.complete = 0u;
    s_fill_dump.count = 0u;
    s_fill_dump.overflow = 0u;
    ++s_fill_dump.frames_done;
}

static void gl_vita_fill_dump_arm(uint32_t window, uint32_t render_p50_us)
{
    int forced = ISAAC_VITA_GL_FILL_CENSUS_DUMP_WIN != 0u &&
        window == ISAAC_VITA_GL_FILL_CENSUS_DUMP_WIN;
    int slow = window >= ISAAC_VITA_GL_FILL_CENSUS_DUMP_MIN_WIN &&
        render_p50_us >= ISAAC_VITA_GL_FILL_CENSUS_DUMP_RND_US;

    if (s_fill_dump.frames_done >= ISAAC_VITA_GL_FILL_CENSUS_DUMP_FRAMES ||
            s_fill_dump.armed || s_fill_dump.capturing ||
            s_fill_dump.complete)
        return;
    if (forced || slow)
        s_fill_dump.armed = 1u;
}
# endif

static void gl_vita_fill_census_reset(void)
{
    memset(&s_fill, 0, sizeof s_fill);
    s_fill.pass_index = GL_VITA_FILL_PASS_DISPLAY;
# if defined(ISAAC_VITA_GL_FILL_CENSUS_DUMP)
    memset(&s_fill_dump, 0, sizeof s_fill_dump);
# endif
}

static void gl_vita_fill_census_reset_window(void)
{
    memset(&s_fill.window, 0, sizeof s_fill.window);
}

static gl_vita_fill_program *gl_vita_fill_program_find(guest_gl_uint program)
{
    gl_vita_fill_program *slot =
        &s_fill.programs[program % GL_VITA_FILL_PROGRAMS];

    return program && slot->valid && slot->name == program ? slot : NULL;
}

static gl_vita_fill_program *gl_vita_fill_program_acquire(
    guest_gl_uint program)
{
    gl_vita_fill_program *slot = gl_vita_fill_program_find(program);

    if (slot || !program)
        return slot;
    slot = &s_fill.programs[program % GL_VITA_FILL_PROGRAMS];
    memset(slot, 0, sizeof *slot);
    slot->name = program;
    slot->valid = 1u;
    slot->position_location = -1;
    slot->transform_location = -1;
    return slot;
}

static void gl_vita_fill_note_program_forget(guest_gl_uint program)
{
    gl_vita_fill_program *slot = gl_vita_fill_program_find(program);

    if (slot)
        slot->valid = 0u;
}

/* Bounded byte compare of a guest C string; reads stop at the first
 * mismatch or terminator, so no byte past the guest's own string is
 * touched.  Independent of ISAAC_VITA_GL_LOCATION_CACHE. */
static int gl_vita_fill_name_is(guest_gl_addr name, const char *expected)
{
    uint32_t index;

    if (!name || name > UINT32_MAX - GL_VITA_FILL_NAME_MAX - 1u)
        return 0;
    for (index = 0u; index <= GL_VITA_FILL_NAME_MAX; ++index) {
        uint8_t byte = ld8(name + index);

        if (byte != (uint8_t)expected[index])
            return 0;
        if (!byte)
            return 1;
    }
    return 0;
}

static void gl_vita_fill_note_attrib_location(
    guest_gl_uint program, guest_gl_addr name, guest_gl_int location)
{
    gl_vita_fill_program *slot;

    if (gl_vita_fill_name_is(name, "Position")) {
        slot = gl_vita_fill_program_acquire(program);
        if (slot)
            slot->position_location =
                location >= 0 && (uint32_t)location < GL_VITA_VERTEX_ATTRIBS ?
                location : -1;
    } else if (gl_vita_fill_name_is(name, "PixelationAmount")) {
        slot = gl_vita_fill_program_acquire(program);
        if (slot)
            slot->pixelation = 1u;
    }
}

static void gl_vita_fill_note_uniform_location(
    guest_gl_uint program, guest_gl_addr name, guest_gl_int location)
{
    gl_vita_fill_program *slot;

    if (!gl_vita_fill_name_is(name, "Transform"))
        return;
    slot = gl_vita_fill_program_acquire(program);
    if (!slot)
        return;
    if (slot->transform_location != location)
        slot->transform_known = 0u;
    slot->transform_location = location >= 0 ? location : -1;
}

static void gl_vita_fill_note_uniform_matrix(
    guest_gl_int location, guest_gl_sizei count,
    guest_gl_boolean transpose, guest_gl_addr value)
{
    gl_vita_fill_program *slot =
        gl_vita_fill_program_find(s_fusion_profile.program);
    uint32_t index;

    if (!slot || slot->transform_location < 0 ||
            location != slot->transform_location || count < 1 ||
            !value || (value & 3u) || value > UINT32_MAX - 64u)
        return;
    for (index = 0u; index < 16u; ++index) {
        float element = ldf(value + index * 4u);

        if (transpose)
            slot->transform[(index & 3u) * 4u + (index >> 2)] = element;
        else
            slot->transform[index] = element;
    }
    slot->transform_known = 1u;
}

static void gl_vita_fill_note_attrib_pointer(
    guest_gl_uint index, guest_gl_int size, guest_gl_enum type,
    guest_gl_sizei stride, guest_gl_addr pointer)
{
    gl_vita_fill_attrib *attrib;

    if (index >= GL_VITA_VERTEX_ATTRIBS)
        return;
    attrib = &s_fill.attribs[index];
    attrib->pointer = pointer;
    attrib->stride = stride;
    attrib->size = size;
    attrib->type = type;
}

static void gl_vita_fill_note_attrib_toggle(guest_gl_uint index, int enabled)
{
    uint16_t bit;

    if (index >= GL_VITA_VERTEX_ATTRIBS)
        return;
    bit = (uint16_t)(UINT16_C(1) << index);
    if (enabled)
        s_fill.attrib_enabled |= bit;
    else
        s_fill.attrib_enabled = (uint16_t)(s_fill.attrib_enabled & ~bit);
}

static void gl_vita_fill_note_viewport(
    guest_gl_int x, guest_gl_int y,
    guest_gl_sizei width, guest_gl_sizei height)
{
    /* GL rejects negative sizes without changing the viewport. */
    if (width < 0 || height < 0)
        return;
    if (!s_fill.viewport_known || s_fill.viewport_x != x ||
            s_fill.viewport_y != y || s_fill.viewport_width != width ||
            s_fill.viewport_height != height)
        ++s_fill.window.viewport_changes;
    s_fill.viewport_x = x;
    s_fill.viewport_y = y;
    s_fill.viewport_width = width;
    s_fill.viewport_height = height;
    s_fill.viewport_known = 1u;
}

static void gl_vita_fill_note_tex_image(
    guest_gl_enum target, guest_gl_int level,
    guest_gl_sizei width, guest_gl_sizei height)
{
    guest_gl_uint texture;

    if (target != GL_VITA_TEXTURE_2D || level != 0 ||
            s_fusion_profile.active_texture >= GL_VITA_TEXTURE_UNITS)
        return;
    texture = s_fusion_profile.textures[s_fusion_profile.active_texture];
    if (!texture || texture >= GL_VITA_TEXTURE_CAPACITY)
        return;
    s_fill.texture_width[texture] =
        width > 0 && width <= 0xffff ? (uint16_t)width : 0u;
    s_fill.texture_height[texture] =
        height > 0 && height <= 0xffff ? (uint16_t)height : 0u;
}

static int gl_vita_fill_attachment_size(
    guest_gl_uint framebuffer, uint32_t *width, uint32_t *height)
{
    const gl_vita_fill_framebuffer_texture *slot =
        &s_fill.attachments[framebuffer % GL_VITA_FILL_FRAMEBUFFERS];

    if (!framebuffer || slot->framebuffer != framebuffer ||
            !slot->texture || slot->texture >= GL_VITA_TEXTURE_CAPACITY)
        return 0;
    *width = s_fill.texture_width[slot->texture];
    *height = s_fill.texture_height[slot->texture];
    return *width != 0u && *height != 0u;
}

static void gl_vita_fill_note_framebuffer_texture(
    guest_gl_enum target, guest_gl_enum attachment, guest_gl_uint texture)
{
    guest_gl_uint framebuffer = s_fusion_profile.framebuffer;

    if (target != GL_VITA_FRAMEBUFFER && target != GL_VITA_DRAW_FRAMEBUFFER)
        return;
    if (attachment == GL_VITA_FILL_COLOR_ATTACHMENT0 && framebuffer) {
        gl_vita_fill_framebuffer_texture *slot =
            &s_fill.attachments[framebuffer % GL_VITA_FILL_FRAMEBUFFERS];

        slot->framebuffer = framebuffer;
        slot->texture = texture;
    }
    /* vitaGL marks the in-use framebuffer dirty for a non-zero texture, so
     * the next draw/clear on it ends the scene (shared.h
     * _glFramebufferTexture2D). */
    if (texture && s_fill.pass_open && framebuffer == s_fill.pass_framebuffer)
        s_fill.pass_dirty = 1u;
}

static void gl_vita_fill_note_read_pixels(void)
{
    /* framebuffers.c:674 glReadPixels ends and finishes the in-use scene
     * only when it reads that scene's framebuffer (in_use_framebuffer ==
     * active_read_fb; the guest binds GL_FRAMEBUFFER, so read == draw). */
    if (s_fill.pass_open &&
            s_fusion_profile.framebuffer == s_fill.pass_framebuffer)
        s_fill.pass_dirty = 1u;
}

static void gl_vita_fill_note_delete_framebuffers(
    guest_gl_sizei count, guest_gl_addr framebuffers)
{
    guest_gl_sizei index;

    if (count <= 0 || !framebuffers ||
            (uint32_t)count > (UINT32_MAX - framebuffers) / 4u)
        return;
    for (index = 0; index < count; ++index) {
        guest_gl_uint deleted = ld32(framebuffers + (uint32_t)index * 4u);
        gl_vita_fill_framebuffer_texture *slot =
            &s_fill.attachments[deleted % GL_VITA_FILL_FRAMEBUFFERS];

        if (deleted && slot->framebuffer == deleted) {
            slot->framebuffer = 0u;
            slot->texture = 0u;
        }
        if (deleted && s_fill.pass_open && deleted == s_fill.pass_framebuffer)
            s_fill.pass_open = 0u;
    }
}

static void gl_vita_fill_pass_touch(guest_gl_uint framebuffer)
{
    if (s_fill.pass_open && !s_fill.pass_dirty &&
            s_fill.pass_framebuffer == framebuffer)
        return;
    s_fill.pass_open = 1u;
    s_fill.pass_dirty = 0u;
    s_fill.pass_framebuffer = framebuffer;
    if (framebuffer) {
        uint32_t width;
        uint32_t height;

        s_fill.pass_index = (uint8_t)(
            s_fill.pass_offscreen_begun < GL_VITA_FILL_PASS_LAST ?
            s_fill.pass_offscreen_begun : GL_VITA_FILL_PASS_LAST);
        ++s_fill.pass_offscreen_begun;
        if (gl_vita_fill_attachment_size(framebuffer, &width, &height)) {
            s_fill.window.attachment_width = width;
            s_fill.window.attachment_height = height;
        }
    } else {
        s_fill.pass_index = GL_VITA_FILL_PASS_DISPLAY;
    }
}

static uint32_t gl_vita_fill_kpx(float area)
{
    float kpx = area * (1.0f / 1024.0f) + 0.5f;

    if (!(kpx > 0.0f))
        return 0u;
    if (kpx >= 4294967040.0f)
        return UINT32_MAX;
    return (uint32_t)kpx;
}

/* A native glClear reached vitaGL (after any elision) on |framebuffer| (the
 * bound one for a guest glClear, the owing one for a materialized owed
 * clear): attachment or display area, and a pass boundary exactly like a
 * draw.  An offscreen target of unknown size counts 0 kpx and one
 * clear_unknown so the shortfall is visible in the record. */
static void gl_vita_fill_census_clear(
    guest_gl_uint framebuffer, guest_gl_bitfield mask)
{
    uint32_t width = GL_VITA_FILL_DISPLAY_WIDTH;
    uint32_t height = GL_VITA_FILL_DISPLAY_HEIGHT;
# if defined(ISAAC_VITA_GL_FILL_CENSUS_DUMP)
    gl_vita_fill_dump_entry *entry;
# endif

    gl_vita_fill_pass_touch(framebuffer);
    ++s_fill.window.pass_clears[s_fill.pass_index];
    if (framebuffer &&
            !gl_vita_fill_attachment_size(framebuffer, &width, &height)) {
        width = 0u;
        height = 0u;
        ++s_fill.window.clear_unknown;
    }
    /* 65535 * 65535 + 512 still fits uint32_t. */
    s_fill.window.kpx_clear += (width * height + 512u) / 1024u;
# if defined(ISAAC_VITA_GL_FILL_CENSUS_DUMP)
    entry = gl_vita_fill_dump_append((uint8_t)GL_VITA_FILL_DUMP_CLEAR);
    if (entry) {
        entry->framebuffer = framebuffer;
        entry->mask = mask;
        entry->texture_width = (uint16_t)width;
        entry->texture_height = (uint16_t)height;
    }
# else
    (void)mask;
# endif
}

static void gl_vita_fill_census_draw(
    guest_gl_enum mode, guest_gl_sizei count, guest_gl_enum type,
    guest_gl_addr indices, int canonical)
{
    IsaacVitaGlFillCensus *window = &s_fill.window;
    guest_gl_uint framebuffer = s_fusion_profile.framebuffer;
    const gl_vita_fill_program *program = NULL;
    const gl_vita_fill_attrib *attrib = NULL;
    uint32_t triangles = 0u;
    uint32_t index_width = 0u;
    uint32_t vertex_bytes = 0u;
    uint32_t draw_kpx = 0u;
    int skip = GL_VITA_FILL_SKIP_NONE;
    int box_valid = 0;
    float box_min_x = 0.0f;
    float box_min_y = 0.0f;
    float box_max_x = 0.0f;
    float box_max_y = 0.0f;
# if defined(ISAAC_VITA_GL_FILL_CENSUS_DUMP)
    gl_vita_fill_dump_entry *entry;
# endif

    gl_vita_fill_pass_touch(framebuffer);
    ++window->draws;
    ++window->pass_draws[s_fill.pass_index];
    if (mode != KAGE_VITA_GL_TRIANGLES || count <= 0 ||
            (uint32_t)count % 3u != 0u) {
        skip = GL_VITA_FILL_SKIP_MODE;
    } else {
        triangles = (uint32_t)count / 3u;
        window->triangles += triangles;
    }
    if (!skip) {
        if (canonical) {
            ++window->synthesized;
            index_width = 2u;
        } else {
            index_width = type == GL_VITA_UNSIGNED_SHORT ? 2u :
                type == GL_VITA_FILL_UNSIGNED_INT ? 4u : 0u;
            if (!index_width || !indices ||
                    (indices & (index_width - 1u)) != 0u ||
                    (uint32_t)count > (UINT32_MAX - indices) / index_width)
                skip = GL_VITA_FILL_SKIP_INDICES;
        }
    }
    if (!skip && (!s_fill.viewport_known || s_fill.viewport_width <= 0 ||
            s_fill.viewport_height <= 0))
        skip = GL_VITA_FILL_SKIP_VIEWPORT;
    if (!skip) {
        program = gl_vita_fill_program_find(s_fusion_profile.program);
        if (!program || program->position_location < 0 ||
                (uint32_t)program->position_location >=
                    GL_VITA_VERTEX_ATTRIBS ||
                !program->transform_known)
            skip = GL_VITA_FILL_SKIP_PROGRAM;
    }
    if (!skip) {
        attrib = &s_fill.attribs[program->position_location];
        vertex_bytes = attrib->stride > 0 ? (uint32_t)attrib->stride :
            (uint32_t)attrib->size * 4u;
        if (!(s_fill.attrib_enabled &
                (uint16_t)(UINT16_C(1) << program->position_location)) ||
                attrib->type != GL_VITA_FLOAT || attrib->size < 2 ||
                attrib->size > 3 || !attrib->pointer ||
                (attrib->pointer & 3u) != 0u ||
                attrib->pointer > UINT32_MAX - 12u ||
                (vertex_bytes & 3u) != 0u ||
                vertex_bytes < (uint32_t)attrib->size * 4u)
            skip = GL_VITA_FILL_SKIP_ATTRIB;
    }
    if (!skip) {
        const float *m = program->transform;
        int affine = m[3] == 0.0f && m[7] == 0.0f && m[11] == 0.0f &&
            m[15] == 1.0f;
        uint32_t vertex_limit =
            (UINT32_MAX - 12u - attrib->pointer) / vertex_bytes;
        float viewport_x = (float)s_fill.viewport_x;
        float viewport_y = (float)s_fill.viewport_y;
        float viewport_w = (float)s_fill.viewport_width;
        float viewport_h = (float)s_fill.viewport_height;
        float viewport_right = viewport_x + viewport_w;
        float viewport_top = viewport_y + viewport_h;
        float area_unclipped = 0.0f;
        float area_clipped = 0.0f;
        uint32_t bad = 0u;
        uint32_t triangle;

        if (!affine)
            ++window->projective;
        for (triangle = 0u; triangle < triangles; ++triangle) {
            float px[3];
            float py[3];
            uint32_t corner;
            int ok = 1;

            for (corner = 0u; corner < 3u; ++corner) {
                static const uint8_t canonical_pattern[6] =
                    { 0u, 2u, 1u, 1u, 2u, 3u };
                uint32_t element = triangle * 3u + corner;
                uint32_t vertex;
                guest_gl_addr address;
                float x;
                float y;
                float z = 0.0f;
                float cx;
                float cy;

                if (canonical)
                    vertex = (element / 6u) * 4u +
                        canonical_pattern[element % 6u];
                else if (index_width == 2u)
                    vertex = ld16(indices + element * 2u);
                else
                    vertex = ld32(indices + element * 4u);
                if (vertex > vertex_limit) {
                    ok = 0;
                    break;
                }
                address = attrib->pointer + vertex * vertex_bytes;
                x = ldf(address);
                y = ldf(address + 4u);
                if (attrib->size == 3)
                    z = ldf(address + 8u);
                cx = m[0] * x + m[4] * y + m[8] * z + m[12];
                cy = m[1] * x + m[5] * y + m[9] * z + m[13];
                if (!affine) {
                    float cw = m[3] * x + m[7] * y + m[11] * z + m[15];

                    if (!(cw > 1e-12f || cw < -1e-12f)) {
                        ok = 0;
                        break;
                    }
                    cx /= cw;
                    cy /= cw;
                }
                px[corner] = (cx + 1.0f) * 0.5f * viewport_w + viewport_x;
                py[corner] = (cy + 1.0f) * 0.5f * viewport_h + viewport_y;
                /* NaN fails every comparison. */
                if (!(px[corner] > -GL_VITA_FILL_COORD_LIMIT &&
                        px[corner] < GL_VITA_FILL_COORD_LIMIT &&
                        py[corner] > -GL_VITA_FILL_COORD_LIMIT &&
                        py[corner] < GL_VITA_FILL_COORD_LIMIT)) {
                    ok = 0;
                    break;
                }
            }
            if (!ok) {
                ++bad;
                continue;
            }
            {
                float area = ((px[1] - px[0]) * (py[2] - py[0]) -
                              (px[2] - px[0]) * (py[1] - py[0])) * 0.5f;
                float min_x = px[0] < px[1] ? px[0] : px[1];
                float max_x = px[0] > px[1] ? px[0] : px[1];
                float min_y = py[0] < py[1] ? py[0] : py[1];
                float max_y = py[0] > py[1] ? py[0] : py[1];
                float box_w;
                float box_h;
                float clip_x0;
                float clip_y0;
                float clip_x1;
                float clip_y1;

                if (area < 0.0f)
                    area = -area;
                if (px[2] < min_x)
                    min_x = px[2];
                if (px[2] > max_x)
                    max_x = px[2];
                if (py[2] < min_y)
                    min_y = py[2];
                if (py[2] > max_y)
                    max_y = py[2];
                box_w = max_x - min_x;
                box_h = max_y - min_y;
                if (box_w > 2.0f * viewport_w || box_h > 2.0f * viewport_h)
                    ++window->big;
                area_unclipped += area;
                clip_x0 = min_x > viewport_x ? min_x : viewport_x;
                clip_y0 = min_y > viewport_y ? min_y : viewport_y;
                clip_x1 = max_x < viewport_right ? max_x : viewport_right;
                clip_y1 = max_y < viewport_top ? max_y : viewport_top;
                if (clip_x1 > clip_x0 && clip_y1 > clip_y0 &&
                        box_w > 0.0f && box_h > 0.0f) {
                    float ratio = ((clip_x1 - clip_x0) * (clip_y1 - clip_y0)) /
                        (box_w * box_h);
                    int32_t tiles_x = (int32_t)(clip_x1 * (1.0f / 32.0f)) -
                        (int32_t)(clip_x0 * (1.0f / 32.0f)) + 1;
                    int32_t tiles_y = (int32_t)(clip_y1 * (1.0f / 32.0f)) -
                        (int32_t)(clip_y0 * (1.0f / 32.0f)) + 1;

                    area_clipped += area * ratio;
                    window->tiles += (uint32_t)tiles_x * (uint32_t)tiles_y;
                }
                if (!box_valid) {
                    box_min_x = min_x;
                    box_min_y = min_y;
                    box_max_x = max_x;
                    box_max_y = max_y;
                    box_valid = 1;
                } else {
                    if (min_x < box_min_x)
                        box_min_x = min_x;
                    if (min_y < box_min_y)
                        box_min_y = min_y;
                    if (max_x > box_max_x)
                        box_max_x = max_x;
                    if (max_y > box_max_y)
                        box_max_y = max_y;
                }
            }
        }
        window->bad += bad;
        draw_kpx = gl_vita_fill_kpx(area_clipped);
        window->kpx_unclipped += gl_vita_fill_kpx(area_unclipped);
        window->kpx_clipped += draw_kpx;
        if (draw_kpx > window->max_draw_kpx)
            window->max_draw_kpx = draw_kpx;
        window->prog_kpx[program->pixelation ? 0 : 1] += draw_kpx;
        if (!s_fusion_profile.blend_enabled)
            window->blend_kpx[2] += draw_kpx;
        else if (s_fusion_profile.blend_destination_rgb == GL_VITA_ONE)
            window->blend_kpx[0] += draw_kpx;
        else if (s_fusion_profile.blend_destination_rgb ==
                GL_VITA_ONE_MINUS_SRC_ALPHA)
            window->blend_kpx[1] += draw_kpx;
        else
            window->blend_kpx[2] += draw_kpx;
        window->pass_kpx[s_fill.pass_index] += draw_kpx;
    } else {
        ++window->miss;
    }
# if defined(ISAAC_VITA_GL_FILL_CENSUS_DUMP)
    entry = gl_vita_fill_dump_append((uint8_t)GL_VITA_FILL_DUMP_DRAW);
    if (entry) {
        guest_gl_uint texture = s_fusion_profile.textures[0];

        entry->rva = guest_gl_backend_return_rva();
        entry->texture = texture;
        if (texture && texture < GL_VITA_TEXTURE_CAPACITY) {
            entry->texture_width = s_fill.texture_width[texture];
            entry->texture_height = s_fill.texture_height[texture];
        }
        entry->kpx = draw_kpx;
        entry->triangles = triangles;
        entry->blend_enabled = s_fusion_profile.blend_enabled;
        entry->blend_source = (uint16_t)s_fusion_profile.blend_source_rgb;
        entry->blend_destination =
            (uint16_t)s_fusion_profile.blend_destination_rgb;
        entry->skip = (uint8_t)skip;
        entry->canonical = (uint8_t)(canonical != 0);
        if (box_valid) {
            entry->box[0] = gl_vita_fill_dump_coordinate(box_min_x);
            entry->box[1] = gl_vita_fill_dump_coordinate(box_min_y);
            entry->box[2] = gl_vita_fill_dump_coordinate(box_max_x);
            entry->box[3] = gl_vita_fill_dump_coordinate(box_max_y);
        }
        if (s_fill.viewport_known) {
            entry->viewport[0] = s_fill.viewport_x;
            entry->viewport[1] = s_fill.viewport_y;
            entry->viewport[2] = s_fill.viewport_width;
            entry->viewport[3] = s_fill.viewport_height;
        }
    }
# endif
}

void gl_vita_backend_fill_census_take_window(
    IsaacVitaGlFillCensus *census, uint32_t window, uint32_t render_p50_us)
{
    s_fill.window.viewport_width =
        s_fill.viewport_known && s_fill.viewport_width > 0 ?
        (uint32_t)s_fill.viewport_width : 0u;
    s_fill.window.viewport_height =
        s_fill.viewport_known && s_fill.viewport_height > 0 ?
        (uint32_t)s_fill.viewport_height : 0u;
    if (census)
        *census = s_fill.window;
    memset(&s_fill.window, 0, sizeof s_fill.window);
# if defined(ISAAC_VITA_GL_FILL_CENSUS_DUMP)
    gl_vita_fill_dump_flush();
    gl_vita_fill_dump_arm(window, render_p50_us);
# else
    (void)window;
    (void)render_p50_us;
# endif
}

void gl_vita_backend_fill_census_present(void)
{
    s_fill.pass_open = 0u;
    s_fill.pass_dirty = 0u;
    s_fill.pass_offscreen_begun = 0u;
    s_fill.pass_index = GL_VITA_FILL_PASS_DISPLAY;
# if defined(ISAAC_VITA_GL_FILL_CENSUS_DUMP)
    if (s_fill_dump.capturing) {
        s_fill_dump.capturing = 0u;
        s_fill_dump.complete = 1u;
    } else if (s_fill_dump.armed) {
        s_fill_dump.armed = 0u;
        s_fill_dump.capturing = 1u;
        s_fill_dump.count = 0u;
        s_fill_dump.overflow = 0u;
        ++s_fill_dump.frame;
    }
# endif
}

uint32_t gl_vita_backend_fill_census_dump_config(
    uint32_t *render_p50_us, uint32_t *min_window, uint32_t *window,
    uint32_t *frames)
{
# if defined(ISAAC_VITA_GL_FILL_CENSUS_DUMP)
    *render_p50_us = ISAAC_VITA_GL_FILL_CENSUS_DUMP_RND_US;
    *min_window = ISAAC_VITA_GL_FILL_CENSUS_DUMP_MIN_WIN;
    *window = ISAAC_VITA_GL_FILL_CENSUS_DUMP_WIN;
    *frames = ISAAC_VITA_GL_FILL_CENSUS_DUMP_FRAMES;
    return 1u;
# else
    *render_p50_us = 0u;
    *min_window = 0u;
    *window = 0u;
    *frames = 0u;
    return 0u;
# endif
}
#elif defined(ISAAC_VITA_GL_FILL_CENSUS_DUMP)
# error "ISAAC_VITA_GL_FILL_CENSUS_DUMP requires ISAAC_VITA_GL_FILL_CENSUS"
#endif

#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
/* The frozen vitaGL has 1024 native program slots, but Isaac keeps only a
 * handful live.  A bounded tracker deliberately fails open if that changes. */
#define GL_VITA_REDUNDANCY_PROGRAM_CAPACITY 32u
#define GL_VITA_REDUNDANCY_LOCATION_CAPACITY 256u
#define GL_VITA_REDUNDANCY_UNIFORM_CAPACITY 128u
#define GL_VITA_REDUNDANCY_PAYLOAD_CAPACITY 4u
#define GL_VITA_LINK_STATUS 0x00008b82u
#define GL_VITA_CURRENT_PROGRAM 0x00008b8du

enum gl_vita_uniform_kind {
    GL_VITA_UNIFORM_1FV = 1,
    GL_VITA_UNIFORM_1I,
    GL_VITA_UNIFORM_1IV,
    GL_VITA_UNIFORM_2FV,
    GL_VITA_UNIFORM_2IV,
    GL_VITA_UNIFORM_3FV,
    GL_VITA_UNIFORM_3IV,
    GL_VITA_UNIFORM_4FV,
    GL_VITA_UNIFORM_4IV,
    GL_VITA_UNIFORM_MATRIX2FV,
    GL_VITA_UNIFORM_MATRIX3FV,
    GL_VITA_UNIFORM_MATRIX4FV
};

typedef struct gl_vita_program_record {
    guest_gl_uint name;
    uint32_t generation;
    uint8_t live;
    uint8_t linked;
} gl_vita_program_record;

typedef struct gl_vita_location_record {
    guest_gl_uint program;
    guest_gl_int location;
    uint32_t generation;
    uint8_t valid;
} gl_vita_location_record;

typedef struct gl_vita_uniform_record {
    guest_gl_uint program;
    guest_gl_int location;
    uint32_t generation;
    guest_gl_addr source;
    uint8_t kind;
    uint8_t transpose;
    uint8_t size;
    uint8_t valid;
    uint8_t payload[GL_VITA_REDUNDANCY_PAYLOAD_CAPACITY];
} gl_vita_uniform_record;

static gl_vita_program_record
    s_gl_programs[GL_VITA_REDUNDANCY_PROGRAM_CAPACITY];
static gl_vita_location_record
    s_gl_locations[GL_VITA_REDUNDANCY_LOCATION_CAPACITY];
static gl_vita_uniform_record
    s_gl_uniforms[GL_VITA_REDUNDANCY_UNIFORM_CAPACITY];
static guest_gl_uint s_gl_current_program;
static uint32_t s_gl_current_generation;
static uint32_t s_gl_next_generation;
static uint8_t s_gl_current_known;
static uint8_t s_gl_generation_exhausted;

_Static_assert((GL_VITA_REDUNDANCY_LOCATION_CAPACITY &
                (GL_VITA_REDUNDANCY_LOCATION_CAPACITY - 1u)) == 0u,
               "GL location cache capacity must be a power of two");
_Static_assert((GL_VITA_REDUNDANCY_UNIFORM_CAPACITY &
                (GL_VITA_REDUNDANCY_UNIFORM_CAPACITY - 1u)) == 0u,
               "GL uniform cache capacity must be a power of two");

static void gl_vita_redundancy_reset(void)
{
    memset(s_gl_programs, 0, sizeof s_gl_programs);
    memset(s_gl_locations, 0, sizeof s_gl_locations);
    memset(s_gl_uniforms, 0, sizeof s_gl_uniforms);
    s_gl_current_program = 0u;
    s_gl_current_generation = 0u;
    s_gl_next_generation = 1u;
    s_gl_current_known = 0u;
    s_gl_generation_exhausted = 0u;
}

static uint32_t gl_vita_redundancy_generation(void)
{
    uint32_t result;

    if (s_gl_generation_exhausted)
        return 0u;
    result = s_gl_next_generation++;
    if (!s_gl_next_generation)
        s_gl_generation_exhausted = 1u;
    return result;
}

static gl_vita_program_record *gl_vita_program_find(guest_gl_uint program)
{
    uint32_t index;

    for (index = 0u; index < GL_VITA_REDUNDANCY_PROGRAM_CAPACITY; ++index) {
        gl_vita_program_record *record = &s_gl_programs[index];
        if (record->live && record->name == program)
            return record;
    }
    return NULL;
}

static void gl_vita_program_created(guest_gl_uint program)
{
    gl_vita_program_record *record = gl_vita_program_find(program);
    uint32_t index;

    if (!program || program == UINT32_MAX || s_gl_generation_exhausted)
        return;
    if (!record) {
        for (index = 0u;
                index < GL_VITA_REDUNDANCY_PROGRAM_CAPACITY; ++index) {
            if (!s_gl_programs[index].live) {
                record = &s_gl_programs[index];
                break;
            }
        }
    }
    if (!record)
        return;
    record->name = program;
    record->generation = gl_vita_redundancy_generation();
    record->live = record->generation != 0u;
    record->linked = 0u;
    if (s_gl_current_known && s_gl_current_program == program)
        s_gl_current_known = 0u;
}

static void gl_vita_program_mutated(guest_gl_uint program, int deleted)
{
    gl_vita_program_record *record = gl_vita_program_find(program);

    if (record) {
        record->linked = 0u;
        record->generation = gl_vita_redundancy_generation();
        if (deleted || !record->generation)
            record->live = 0u;
    }
    if (s_gl_current_known && s_gl_current_program == program)
        s_gl_current_known = 0u;
}

static void gl_vita_program_link_status(
    guest_gl_uint program, guest_gl_enum name)
{
    gl_vita_program_record *record;
    GLint value = 0;

    if (name != GL_VITA_LINK_STATUS)
        return;
    record = gl_vita_program_find(program);
    if (!record)
        return;
    /* Never read the guest output pointer: a native query can return after an
     * invalid request without proving it wrote that address.  This second,
     * internal exact-pname query writes only our known local word. */
    glGetProgramiv((GLuint)program, (GLenum)GL_VITA_LINK_STATUS, &value);
    record->linked = value == 1;
}

static uint32_t gl_vita_redundancy_hash(
    guest_gl_uint program, uint32_t generation, guest_gl_int location)
{
    uint32_t value = (uint32_t)location;
    value ^= program * UINT32_C(0x9e3779b9);
    value ^= generation * UINT32_C(0x85ebca6b);
    value ^= value >> 16;
    return value;
}

static gl_vita_location_record *gl_vita_location_slot(
    guest_gl_uint program, uint32_t generation, guest_gl_int location)
{
    uint32_t index = gl_vita_redundancy_hash(
        program, generation, location) &
        (GL_VITA_REDUNDANCY_LOCATION_CAPACITY - 1u);
    return &s_gl_locations[index];
}

static gl_vita_uniform_record *gl_vita_uniform_slot(
    guest_gl_uint program, uint32_t generation, guest_gl_int location)
{
    uint32_t index = gl_vita_redundancy_hash(
        program, generation, location) &
        (GL_VITA_REDUNDANCY_UNIFORM_CAPACITY - 1u);
    return &s_gl_uniforms[index];
}

static void gl_vita_location_observed(
    guest_gl_uint program, guest_gl_int location)
{
    gl_vita_program_record *record;
    gl_vita_location_record *slot;

    /* Pinned vitaGL encodes a uniform pointer in positive locations and uses
     * both zero and -1 as no-op sentinels.  Neither sentinel is evidence of a
     * mutable uniform which can safely be shadowed. */
    if (location == 0 || location == -1)
        return;
    record = gl_vita_program_find(program);
    if (!record || !record->linked)
        return;
    slot = gl_vita_location_slot(program, record->generation, location);
    slot->program = program;
    slot->location = location;
    slot->generation = record->generation;
    slot->valid = 1u;
}

static gl_vita_program_record *gl_vita_current_program_for_location(
    guest_gl_int location)
{
    gl_vita_program_record *program;
    gl_vita_location_record *slot;

    if (!s_gl_current_known || !s_gl_current_program ||
            location == 0 || location == -1)
        return NULL;
    program = gl_vita_program_find(s_gl_current_program);
    if (!program || !program->linked ||
            program->generation != s_gl_current_generation)
        return NULL;
    slot = gl_vita_location_slot(
        program->name, program->generation, location);
    if (!slot->valid || slot->program != program->name ||
            slot->generation != program->generation ||
            slot->location != location)
        return NULL;
    return program;
}

static void gl_vita_uniform_invalidate_program(
    const gl_vita_program_record *program)
{
    uint32_t index;

    if (!program)
        return;
    for (index = 0u; index < GL_VITA_REDUNDANCY_UNIFORM_CAPACITY; ++index) {
        gl_vita_uniform_record *slot = &s_gl_uniforms[index];
        if (slot->valid && slot->program == program->name &&
                slot->generation == program->generation)
            slot->valid = 0u;
    }
}

static void gl_vita_uniform_invalidate_location(
    const gl_vita_program_record *program, guest_gl_int location)
{
    gl_vita_uniform_record *slot;

    if (!program)
        return;
    slot = gl_vita_uniform_slot(
        program->name, program->generation, location);
    if (slot->valid && slot->program == program->name &&
            slot->generation == program->generation &&
            slot->location == location)
        slot->valid = 0u;
}

static int gl_vita_use_program_skip(guest_gl_uint program)
{
    gl_vita_program_record *record;

    if (program == 0u) {
        if (s_gl_current_known && s_gl_current_program == 0u) {
            GL_VITA_PHASE_COUNT(use_program_suppressed);
            return 1;
        }
        return 0;
    }
    record = gl_vita_program_find(program);
    if (!record || !record->linked)
        return 0;
    if (s_gl_current_known && s_gl_current_program == program &&
            s_gl_current_generation == record->generation) {
        GL_VITA_PHASE_COUNT(use_program_suppressed);
        return 1;
    }
    return 0;
}

static void gl_vita_use_program_forwarded(guest_gl_uint program)
{
    gl_vita_program_record *record;
    GLint observed = -1;

    record = program ? gl_vita_program_find(program) : NULL;
    if (program && (!record || !record->linked)) {
        s_gl_current_known = 0u;
        return;
    }
    /* Link status proves the object eligible; the driver query proves this
     * particular UseProgram returned with the requested program current.  A
     * failed/invalid bind therefore remains on the native path forever. */
    glGetIntegerv((GLenum)GL_VITA_CURRENT_PROGRAM, &observed);
    if ((guest_gl_uint)observed != program) {
        s_gl_current_known = 0u;
        return;
    }
    s_gl_current_program = program;
    s_gl_current_generation = record ? record->generation : 0u;
    s_gl_current_known = 1u;
}

static int gl_vita_uniform_skip(
    enum gl_vita_uniform_kind kind, guest_gl_int location,
    guest_gl_sizei count, guest_gl_boolean transpose,
    guest_gl_addr source, const void *inline_value, uint32_t size)
{
    gl_vita_program_record *program =
        gl_vita_current_program_for_location(location);
    gl_vita_uniform_record *slot;
    const void *payload;

    if (!program) {
        /* An unknown non-sentinel location/current program may alias state which
         * was previously cached.  Fail open and forget every shadow. */
        if (location != 0 && location != -1)
            memset(s_gl_uniforms, 0, sizeof s_gl_uniforms);
        return 0;
    }
    /* Pointer-form uniforms remain exact native calls.  Native return is not
     * proof that the source span was read (sentinel/type paths may return
     * early), and probing a Vita memblock before every comparison would erase
     * the intended hot-path win.  They also invalidate a same-location scalar
     * shadow before being forwarded. */
    if (source) {
        if (count == 1)
            gl_vita_uniform_invalidate_location(program, location);
        else
            gl_vita_uniform_invalidate_program(program);
        return 0;
    }
    if (count != 1 || !size ||
            size > GL_VITA_REDUNDANCY_PAYLOAD_CAPACITY ||
            (!source && !inline_value)) {
        /* count>1 can overlap locations obtained for array elements. */
        gl_vita_uniform_invalidate_program(program);
        return 0;
    }
    slot = gl_vita_uniform_slot(
        program->name, program->generation, location);
    if (!slot->valid || slot->program != program->name ||
            slot->generation != program->generation ||
            slot->location != location || slot->kind != (uint8_t)kind ||
            slot->transpose != (uint8_t)transpose || slot->size != size ||
            slot->source != source) {
        slot->valid = 0u;
        return 0;
    }
    payload = inline_value;
    if (memcmp(slot->payload, payload, size) != 0) {
        slot->valid = 0u;
        return 0;
    }
    GL_VITA_PHASE_COUNT(uniform_suppressed);
    return 1;
}

static void gl_vita_uniform_forwarded(
    enum gl_vita_uniform_kind kind, guest_gl_int location,
    guest_gl_sizei count, guest_gl_boolean transpose,
    guest_gl_addr source, const void *inline_value, uint32_t size)
{
    gl_vita_program_record *program;
    gl_vita_uniform_record *slot;
    const void *payload;

    /* Pointer forms cannot populate this scalar shadow; do not look up the
     * current program/location merely to reject them after forwarding. */
    if (source || count != 1 || !size ||
            size > GL_VITA_REDUNDANCY_PAYLOAD_CAPACITY ||
            !inline_value)
        return;
    program = gl_vita_current_program_for_location(location);
    if (!program)
        return;
    payload = inline_value;
    slot = gl_vita_uniform_slot(
        program->name, program->generation, location);
    slot->program = program->name;
    slot->location = location;
    slot->generation = program->generation;
    slot->source = source;
    slot->kind = (uint8_t)kind;
    slot->transpose = (uint8_t)transpose;
    slot->size = (uint8_t)size;
    memcpy(slot->payload, payload, size);
    slot->valid = 1u;
}
#endif
static const char s_partial[] =
    "Vita GL backend: 62/73 typed symbols installed; 11 remain loud";
static const char *const s_missing_symbols[] = {
    "glClampColorARB",
    "glUniform1uiv",
    "glUniform2uiv",
    "glUniform3uiv",
    "glUniform4uiv",
    "glUniformMatrix2x3fv",
    "glUniformMatrix2x4fv",
    "glUniformMatrix3x2fv",
    "glUniformMatrix3x4fv",
    "glUniformMatrix4x2fv",
    "glUniformMatrix4x3fv"
};

#if defined(ISAAC_VITA_FIRST_FRAME_PROBE)
#ifndef ISAAC_VITA_FIRST_FRAME_BUILD_ID
# error ISAAC_VITA_FIRST_FRAME_BUILD_ID must identify the diagnostic binary
#endif

#define GL_VITA_FF_FRAMEBUFFER         0x00008d40u
#define GL_VITA_FF_READ_FRAMEBUFFER    0x00008ca8u
#define GL_VITA_FF_DRAW_FRAMEBUFFER    0x00008ca9u
#define GL_VITA_FF_FRAMEBUFFER_BINDING 0x00008ca6u
#define GL_VITA_FF_READ_FRAMEBUFFER_BINDING 0x00008caau
#define GL_VITA_FF_COLOR_ATTACHMENT0   0x00008ce0u
#define GL_VITA_FF_VIEWPORT            0x00000ba2u
#define GL_VITA_FF_SCISSOR_TEST        0x00000c11u
#define GL_VITA_FF_COLOR_CLEAR_VALUE   0x00000c22u
#define GL_VITA_FF_COLOR_WRITEMASK     0x00000c23u
#define GL_VITA_FF_COLOR_BUFFER_BIT    0x00004000u
#define GL_VITA_FF_FRAMEBUFFER_COMPLETE 0x00008cd5u
#define GL_VITA_FF_NEAREST             0x00002600u
#define GL_VITA_FF_RGBA                0x00001908u
#define GL_VITA_FF_UNSIGNED_BYTE       0x00001401u
#define GL_VITA_FF_NO_ERROR            0u
#define GL_VITA_FF_DISPLAY_WIDTH       960
#define GL_VITA_FF_DISPLAY_HEIGHT      544
#define GL_VITA_FF_VIEWPORT_Y          2
#define GL_VITA_FF_LOGICAL_WIDTH       960
#define GL_VITA_FF_LOGICAL_HEIGHT      540
#define GL_VITA_FF_READBACK_WIDTH      1
#define GL_VITA_FF_READBACK_HEIGHT     1
#define GL_VITA_FF_QUEUE_CAPACITY      16u
#define GL_VITA_FF_QUEUE_STOP_PRESENT  8u
#define GL_VITA_FF_DISPLAY_BUFFER_COUNT 3u

_Static_assert(sizeof(ISAAC_VITA_FIRST_FRAME_BUILD_ID) - 1u <= 96u,
               "first-frame build stamp exceeds durable-log bound");
#if !defined(ISAAC_VITA_DIRECT_DEFAULT)
_Static_assert(ISAAC_VITA_FIRST_FRAME_CLEAR_END_PRESENT <
               ISAAC_VITA_FIRST_FRAME_BLIT_END_PRESENT,
               "first-frame diagnostic phases overlap");
_Static_assert(ISAAC_VITA_FIRST_FRAME_BLIT_END_PRESENT <
               ISAAC_VITA_FIRST_FRAME_BYPASS_END_PRESENT,
               "first-frame postprocess phase is empty");
#endif
_Static_assert((GL_VITA_FF_QUEUE_CAPACITY &
                (GL_VITA_FF_QUEUE_CAPACITY - 1u)) == 0u,
               "display-queue ring capacity must be a power of two");
_Static_assert(GL_VITA_FF_READBACK_WIDTH == 1 &&
               GL_VITA_FF_READBACK_HEIGHT == 1,
               "guarded readback payload holds exactly one RGBA pixel");
_Static_assert(sizeof(IsaacVitaGlDisplayQueueProbe) == 96u,
               "vitaGL display-lineage probe ABI drifted");

#if defined(ISAAC_GL_VITA_FIRST_FRAME_ORACLE)
void isaac_gl_vita_first_frame_oracle_log(const char *format, ...);
# define GL_VITA_FF_LOG isaac_gl_vita_first_frame_oracle_log
#else
void isaac_vita_log(const char *format, ...);
# define GL_VITA_FF_LOG isaac_vita_log
#endif

static IsaacVitaFirstFrameSnapshot s_first_frame;
#if !defined(ISAAC_VITA_DIRECT_DEFAULT)
static uint32_t s_first_frame_result_log_mask;
#endif
static unsigned s_first_frame_config_logged;
#if !defined(ISAAC_VITA_DIRECT_DEFAULT)
static unsigned s_first_frame_final_logged;
#endif

typedef struct gl_vita_first_frame_queue_slot {
    volatile uint32_t published_sequence;
    uint32_t generation;
    uint32_t guest_present;
    IsaacVitaGlDisplayQueueProbe event;
} gl_vita_first_frame_queue_slot;

typedef struct gl_vita_first_frame_queue_map {
    uint32_t sequence;
    uint32_t guest_present;
} gl_vita_first_frame_queue_map;

static gl_vita_first_frame_queue_slot
    s_first_frame_queue_add[GL_VITA_FF_QUEUE_CAPACITY];
static gl_vita_first_frame_queue_slot
    s_first_frame_queue_callback[GL_VITA_FF_QUEUE_CAPACITY];
static gl_vita_first_frame_queue_map
    s_first_frame_queue_map[GL_VITA_FF_QUEUE_CAPACITY];
static volatile uint32_t s_first_frame_queue_active;
static volatile uint32_t s_first_frame_queue_generation;
static volatile uint32_t s_first_frame_queue_present_tag;
static uint32_t s_first_frame_queue_log_mask;
static uint32_t s_first_frame_queue_dedicated_mask;
static unsigned s_first_frame_queue_final_logged;

#if defined(ISAAC_VITA_KNOWN_COLOR_PROBE)
typedef struct gl_vita_known_color_clear_state {
    uint32_t attempted;
    uint32_t present_index;
    uint32_t saved_read_framebuffer;
    uint32_t saved_draw_framebuffer;
    int32_t saved_viewport[4];
    uint32_t saved_color_mask;
    uint32_t saved_scissor;
    uint32_t pre_error;
    uint32_t post_error;
} gl_vita_known_color_clear_state;

static gl_vita_known_color_clear_state s_known_color_clear;
static IsaacVitaGlKnownColorProbe s_known_color_transfer;
static volatile uint32_t s_known_color_transfer_valid;
static unsigned s_known_color_logged;
#endif

static uint32_t gl_vita_first_frame_atomic_load(
    const volatile uint32_t *value)
{
#if defined(_MSC_VER)
    return (uint32_t)_InterlockedCompareExchange(
        (volatile long *)value, 0, 0);
#else
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
#endif
}

static void gl_vita_first_frame_atomic_store(
    volatile uint32_t *destination, uint32_t value)
{
#if defined(_MSC_VER)
    (void)_InterlockedExchange(
        (volatile long *)destination, (long)value);
#else
    __atomic_store_n(destination, value, __ATOMIC_RELEASE);
#endif
}

static int gl_vita_first_frame_atomic_compare_exchange(
    volatile uint32_t *destination, uint32_t expected, uint32_t desired)
{
#if defined(_MSC_VER)
    return (uint32_t)_InterlockedCompareExchange(
        (volatile long *)destination, (long)desired,
        (long)expected) == expected;
#else
    return __atomic_compare_exchange_n(
        destination, &expected, desired, 0,
        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
#endif
}

static void gl_vita_first_frame_increment(uint32_t *value)
{
    if (*value != UINT32_MAX)
        ++*value;
}

static void gl_vita_first_frame_reset(void)
{
    uint32_t generation;

    /* A callback which already passed the active test may still publish.
     * Retire its generation before resetting owner-only state; fixed static
     * slots deliberately remain allocated and are never memset. */
    gl_vita_first_frame_atomic_store(&s_first_frame_queue_active, 0u);
    gl_vita_first_frame_atomic_store(
        &s_first_frame_queue_present_tag, 0u);
    generation = gl_vita_first_frame_atomic_load(
        &s_first_frame_queue_generation) + 1u;
    if (!generation)
        generation = 1u;
    gl_vita_first_frame_atomic_store(
        &s_first_frame_queue_generation, generation);
    memset(&s_first_frame, 0, sizeof s_first_frame);
    memset(s_first_frame_queue_map, 0, sizeof s_first_frame_queue_map);
#if !defined(ISAAC_VITA_DIRECT_DEFAULT)
    s_first_frame_result_log_mask = 0u;
#endif
    s_first_frame_config_logged = 0u;
#if !defined(ISAAC_VITA_DIRECT_DEFAULT)
    s_first_frame_final_logged = 0u;
#endif
    s_first_frame_queue_log_mask = 0u;
    s_first_frame_queue_dedicated_mask = 0u;
    s_first_frame_queue_final_logged = 0u;
#if defined(ISAAC_VITA_KNOWN_COLOR_PROBE)
    gl_vita_first_frame_atomic_store(&s_known_color_transfer_valid, 0u);
    memset(&s_known_color_clear, 0, sizeof s_known_color_clear);
    memset(&s_known_color_transfer, 0, sizeof s_known_color_transfer);
    s_known_color_logged = 0u;
#endif
}

static void gl_vita_first_frame_log_summary(
    const char *phase, uint32_t present_index)
{
    GL_VITA_FF_LOG(
        "KAGE VITA FIRST FRAME: bid40=%.40s phase=%s present=%u "
        "gfbo=%u mfbo=%u att_arg=%u att_calls=%u "
        "bind=%u/%u clear=%u/%u "
        "draw=%u/%u last_draw=%u control=%u blit=%u/%u skip=%u/%u end=ff",
        ISAAC_VITA_FIRST_FRAME_BUILD_ID, phase, (unsigned)present_index,
        (unsigned)s_first_frame.current_guest_fbo,
        (unsigned)s_first_frame.manager_fbo,
        (unsigned)s_first_frame.manager_color_attach_arg,
        (unsigned)s_first_frame.manager_color_attach_calls,
        (unsigned)s_first_frame.bind_zero,
        (unsigned)s_first_frame.bind_nonzero,
        (unsigned)s_first_frame.clear_default,
        (unsigned)s_first_frame.clear_offscreen,
        (unsigned)s_first_frame.draw_default,
        (unsigned)s_first_frame.draw_offscreen,
        (unsigned)s_first_frame.last_draw_fbo,
        (unsigned)s_first_frame.control_clears,
        (unsigned)s_first_frame.blit_successes,
        (unsigned)s_first_frame.blit_attempts,
        (unsigned)s_first_frame.blit_no_manager,
        (unsigned)s_first_frame.blit_incomplete);
}

#if defined(ISAAC_GL_VITA_FIRST_FRAME_ORACLE)
void isaac_gl_vita_first_frame_oracle_log_worst_summary(void)
{
    IsaacVitaFirstFrameSnapshot saved = s_first_frame;

    memset(&s_first_frame, 0xff, sizeof s_first_frame);
    gl_vita_first_frame_log_summary(
#if defined(ISAAC_VITA_DIRECT_DEFAULT)
        "true-direct-default-passive", UINT32_MAX);
#else
        "postprocess-bypass-complete", UINT32_MAX);
#endif
    s_first_frame = saved;
}
#endif

void gl_vita_backend_first_frame_set_manager_framebuffer(
    uint32_t framebuffer)
{
    if (framebuffer != s_first_frame.manager_fbo) {
        s_first_frame.manager_color_attach_arg = 0u;
        s_first_frame.manager_color_attach_calls = 0u;
    }
    s_first_frame.manager_fbo = framebuffer;
    if (framebuffer && !s_first_frame_config_logged) {
        s_first_frame_config_logged = 1u;
#if defined(ISAAC_VITA_DIRECT_DEFAULT)
        GL_VITA_FF_LOG(
            "KAGE VITA FIRST FRAME: bid40=%.40s phase=config manager_fbo=%u "
            "mode=true-direct-default probes=passive",
            ISAAC_VITA_FIRST_FRAME_BUILD_ID, (unsigned)framebuffer);
#else
        GL_VITA_FF_LOG(
            "KAGE VITA FIRST FRAME: bid40=%.40s phase=config manager_fbo=%u "
            "clear=[0,%u) blit=[%u,%u) bypass=[%u,%u)",
            ISAAC_VITA_FIRST_FRAME_BUILD_ID, (unsigned)framebuffer,
            (unsigned)ISAAC_VITA_FIRST_FRAME_CLEAR_END_PRESENT,
            (unsigned)ISAAC_VITA_FIRST_FRAME_CLEAR_END_PRESENT,
            (unsigned)ISAAC_VITA_FIRST_FRAME_BLIT_END_PRESENT,
            (unsigned)ISAAC_VITA_FIRST_FRAME_BLIT_END_PRESENT,
            (unsigned)ISAAC_VITA_FIRST_FRAME_BYPASS_END_PRESENT);
#endif
    }
}

void gl_vita_backend_first_frame_snapshot(
    IsaacVitaFirstFrameSnapshot *snapshot)
{
    if (snapshot)
        *snapshot = s_first_frame;
}

/* vitaGL calls this strong endpoint from both the owner and display-queue
 * threads.  It must remain a bounded copy only: no GL, logging, allocation,
 * or waiting is legal here.  Each stage has one producer and its own fixed
 * slots; the owner-thread poller below consumes published snapshots. */
void isaac_vitagl_display_queue_probe(
    const IsaacVitaGlDisplayQueueProbe *event)
{
    gl_vita_first_frame_queue_slot *slot;
    uint32_t generation;
    uint32_t guest_present = UINT32_MAX;

    if (!event)
        return;
    /* Read generation first: if shutdown races this callback, the event is
     * either rejected by active==0 or published with the retired generation. */
    generation = gl_vita_first_frame_atomic_load(
        &s_first_frame_queue_generation);
    if (!gl_vita_first_frame_atomic_load(
            &s_first_frame_queue_active))
        return;
    if (event->size != sizeof *event || event->sequence == 0u)
        return;
    if (event->stage == ISAAC_VITAGL_DISPLAY_QUEUE_STAGE_ADD_RESULT) {
        uint32_t tag = gl_vita_first_frame_atomic_load(
            &s_first_frame_queue_present_tag);

        /* Loading/splash swaps never run inside the explicit game tag. */
        if (!tag)
            return;
        guest_present = tag - 1u;
        slot = &s_first_frame_queue_add[
            event->sequence & (GL_VITA_FF_QUEUE_CAPACITY - 1u)];
    } else if (event->stage ==
            ISAAC_VITAGL_DISPLAY_QUEUE_STAGE_DISPLAY_CALLBACK) {
        slot = &s_first_frame_queue_callback[
            event->sequence & (GL_VITA_FF_QUEUE_CAPACITY - 1u)];
    } else {
        return;
    }

    gl_vita_first_frame_atomic_store(&slot->published_sequence, 0u);
    slot->generation = generation;
    slot->guest_present = guest_present;
    slot->event = *event;
    if (gl_vita_first_frame_atomic_load(&s_first_frame_queue_active) &&
            gl_vita_first_frame_atomic_load(
                &s_first_frame_queue_generation) == generation) {
        gl_vita_first_frame_atomic_store(
            &slot->published_sequence, event->sequence);
    }
}

static int gl_vita_first_frame_boundary_index(uint32_t present)
{
    switch (present) {
#if defined(ISAAC_VITA_DIRECT_DEFAULT)
    case 0u: return 0;
    case 1u: return 1;
    case 119u: return 2;
    case 239u: return 3;
    case 240u: return 4;
    case 359u: return 5;
    case 360u: return 6;
#else
    case 119u: return 0;
    case 239u: return 1;
    case 240u: return 2;
    case 359u: return 3;
    case 360u: return 4;
#endif
    default: return -1;
    }
}

/* These are deliberately not first-frame readback boundaries.  The old
 * p0/p1 observations invoke glFinish/glReadPixels and create extra GXM
 * scenes.  p2..p4 are three consecutive unmodified game presents and cover
 * every member of the three-buffer rotation. */
static int gl_vita_display_lineage_boundary_index(uint32_t present)
{
    switch (present) {
    case 2u: return 0;
    case 3u: return 1;
    case 4u: return 2;
    default: return -1;
    }
}

static int gl_vita_first_frame_queue_lookup(
    uint32_t sequence, uint32_t *guest_present)
{
    gl_vita_first_frame_queue_map *entry =
        &s_first_frame_queue_map[
            sequence & (GL_VITA_FF_QUEUE_CAPACITY - 1u)];

    if (entry->sequence != sequence)
        return 0;
    *guest_present = entry->guest_present;
    return 1;
}

static void gl_vita_first_frame_queue_event(
    const IsaacVitaGlDisplayQueueProbe *event, uint32_t tagged_present)
{
    uint32_t present;
    uint32_t bit;
    int boundary;

    if (event->stage == ISAAC_VITAGL_DISPLAY_QUEUE_STAGE_ADD_RESULT) {
        gl_vita_first_frame_queue_map *entry =
            &s_first_frame_queue_map[
                event->sequence & (GL_VITA_FF_QUEUE_CAPACITY - 1u)];

        if (tagged_present == UINT32_MAX)
            return;
        entry->guest_present = tagged_present;
        entry->sequence = event->sequence;
        present = tagged_present;
    } else if (!gl_vita_first_frame_queue_lookup(
            event->sequence, &present)) {
        return;
    }
    boundary = gl_vita_display_lineage_boundary_index(present);
    if (boundary < 0)
        return;
    bit = 1u << ((unsigned)boundary * 2u +
        (event->stage == ISAAC_VITAGL_DISPLAY_QUEUE_STAGE_DISPLAY_CALLBACK));
    if (s_first_frame_queue_log_mask & bit)
        return;
    s_first_frame_queue_log_mask |= bit;

    if (event->stage == ISAAC_VITAGL_DISPLAY_QUEUE_STAGE_ADD_RESULT) {
        const int chain_ok =
            event->detail.add.begin_count == 1u &&
            event->detail.add.end_count == 1u &&
            event->detail.add.begin_context ==
                event->detail.add.end_context &&
            event->detail.add.begin_fragment_sync ==
                event->detail.add.new_sync &&
            event->detail.add.begin_color_data == event->address &&
            event->detail.add.mismatch_mask == 0u;
        const int dedicated_ok =
            event->back_index < GL_VITA_FF_DISPLAY_BUFFER_COUNT &&
            event->detail.add.back_memblock_uid >= 0 &&
            event->detail.add.back_get_base_result == 0 &&
            event->detail.add.back_map_result == 0 &&
            event->detail.add.back_dedicated == 1u;

        if (dedicated_ok)
            s_first_frame_queue_dedicated_mask |=
                1u << event->back_index;

        GL_VITA_FF_LOG(
            "KAGE VITA FIRST FRAME: bid40=%.40s phase=queue-add "
            "p=%08x seq=%08x rc=%08x front=%08x back=%08x "
            "addr=%08x old=%08x new=%08x mem=%08x/%08x/%08x "
            "ded=%u end=ff",
            ISAAC_VITA_FIRST_FRAME_BUILD_ID, (unsigned)present,
            (unsigned)event->sequence,
            (unsigned)event->detail.add.result,
            (unsigned)event->front_index, (unsigned)event->back_index,
            (unsigned)event->address,
            (unsigned)event->detail.add.old_sync,
            (unsigned)event->detail.add.new_sync,
            (unsigned)event->detail.add.back_memblock_uid,
            (unsigned)event->detail.add.back_get_base_result,
            (unsigned)event->detail.add.back_map_result,
            (unsigned)event->detail.add.back_dedicated);
        GL_VITA_FF_LOG(
            "KAGE VITA FIRST FRAME: bid40=%.40s phase=queue-scene "
            "p=%08x seq=%08x begin=%08x/%08x/%08x/%08x/%08x/"
            "%08x/%08x end=%08x/%08x/%08x mismatch=%08x chain=%u "
            "end=ff",
            ISAAC_VITA_FIRST_FRAME_BUILD_ID, (unsigned)present,
            (unsigned)event->sequence,
            (unsigned)event->detail.add.begin_count,
            (unsigned)event->detail.add.begin_context,
            (unsigned)event->detail.add.begin_render_target,
            (unsigned)event->detail.add.begin_fragment_sync,
            (unsigned)event->detail.add.begin_color_surface,
            (unsigned)event->detail.add.begin_color_data,
            (unsigned)event->detail.add.begin_result,
            (unsigned)event->detail.add.end_count,
            (unsigned)event->detail.add.end_context,
            (unsigned)event->detail.add.end_result,
            (unsigned)event->detail.add.mismatch_mask,
            (unsigned)chain_ok);
    } else {
        const int chain_ok =
            event->detail.display.size == 24u &&
            event->detail.display.base == event->address &&
            event->detail.display.pitch == 960u &&
            event->detail.display.pixel_format == 0u &&
            event->detail.display.width == 960u &&
            event->detail.display.height == 544u &&
            event->detail.display.sync == 1u;

        GL_VITA_FF_LOG(
            "KAGE VITA FIRST FRAME: bid40=%.40s phase=queue-display "
            "p=%08x seq=%08x front=%08x back=%08x addr=%08x "
            "fb=%08x/%08x/%08x/%08x/%08x/%08x/%08x/%08x chain=%u "
            "samples=%08x hash=%08x rgba=%08x,%08x,%08x,%08x,"
            "%08x,%08x,%08x,%08x end=ff",
            ISAAC_VITA_FIRST_FRAME_BUILD_ID, (unsigned)present,
            (unsigned)event->sequence, (unsigned)event->front_index,
            (unsigned)event->back_index, (unsigned)event->address,
            (unsigned)event->detail.display.size,
            (unsigned)event->detail.display.base,
            (unsigned)event->detail.display.pitch,
            (unsigned)event->detail.display.pixel_format,
            (unsigned)event->detail.display.width,
            (unsigned)event->detail.display.height,
            (unsigned)event->detail.display.sync,
            (unsigned)event->detail.display.result,
            (unsigned)chain_ok,
            (unsigned)event->detail.display.sample_count,
            (unsigned)event->detail.display.sparse_hash,
            (unsigned)event->detail.display.rgba[0],
            (unsigned)event->detail.display.rgba[1],
            (unsigned)event->detail.display.rgba[2],
            (unsigned)event->detail.display.rgba[3],
            (unsigned)event->detail.display.rgba[4],
            (unsigned)event->detail.display.rgba[5],
            (unsigned)event->detail.display.rgba[6],
            (unsigned)event->detail.display.rgba[7]);
    }
}

static void gl_vita_first_frame_queue_poll_slots(
    gl_vita_first_frame_queue_slot *slots)
{
    unsigned index;
    uint32_t generation = gl_vita_first_frame_atomic_load(
        &s_first_frame_queue_generation);

    for (index = 0u; index < GL_VITA_FF_QUEUE_CAPACITY; ++index) {
        gl_vita_first_frame_queue_slot *slot = &slots[index];
        IsaacVitaGlDisplayQueueProbe event;
        uint32_t slot_generation;
        uint32_t guest_present;
        uint32_t published = gl_vita_first_frame_atomic_load(
            &slot->published_sequence);

        if (!published)
            continue;
        slot_generation = slot->generation;
        guest_present = slot->guest_present;
        event = slot->event;
        if (gl_vita_first_frame_atomic_load(
                &slot->published_sequence) != published)
            continue;
        if (slot_generation == generation &&
                event.sequence == published &&
                gl_vita_first_frame_atomic_compare_exchange(
                    &slot->published_sequence, published, 0u))
            gl_vita_first_frame_queue_event(&event, guest_present);
    }
}

static void gl_vita_first_frame_queue_poll(uint32_t present_index)
{
    if (present_index == 0u) {
        gl_vita_first_frame_atomic_store(
            &s_first_frame_queue_active, 1u);
        return;
    }
    if (!gl_vita_first_frame_atomic_load(&s_first_frame_queue_active))
        return;

    /* ADD publishes the exact guest-present mapping before callback lookup. */
    gl_vita_first_frame_queue_poll_slots(s_first_frame_queue_add);
    gl_vita_first_frame_queue_poll_slots(s_first_frame_queue_callback);
    if (present_index >= GL_VITA_FF_QUEUE_STOP_PRESENT &&
            !s_first_frame_queue_final_logged) {
        const uint32_t expected_dedicated_mask =
            (1u << GL_VITA_FF_DISPLAY_BUFFER_COUNT) - 1u;
        const unsigned dedicated_count =
            (unsigned)((s_first_frame_queue_dedicated_mask & 1u) != 0u) +
            (unsigned)((s_first_frame_queue_dedicated_mask & 2u) != 0u) +
            (unsigned)((s_first_frame_queue_dedicated_mask & 4u) != 0u);

        gl_vita_first_frame_atomic_store(
            &s_first_frame_queue_active, 0u);
        s_first_frame_queue_final_logged = 1u;
        GL_VITA_FF_LOG(
            "KAGE VITA FIRST FRAME: bid40=%.40s "
            "phase=queue-probe-complete present=%u seen=0x%02x "
            "expected=0x3f dedicated=%u/3 dedmask=0x%02x "
            "dedgate=%u",
            ISAAC_VITA_FIRST_FRAME_BUILD_ID, (unsigned)present_index,
            (unsigned)s_first_frame_queue_log_mask, dedicated_count,
            (unsigned)s_first_frame_queue_dedicated_mask,
            (unsigned)(s_first_frame_queue_dedicated_mask ==
                expected_dedicated_mask));
    }
}

void gl_vita_backend_first_frame_queue_begin(uint32_t present_index)
{
    /* tag==0 is reserved for every loading/splash/non-game swap. */
    gl_vita_first_frame_atomic_store(
        &s_first_frame_queue_present_tag, present_index + 1u);
}

#if defined(ISAAC_VITA_KNOWN_COLOR_PROBE)
uint32_t isaac_vita_vitagl_known_color_present_index(void)
{
    uint32_t tag = gl_vita_first_frame_atomic_load(
        &s_first_frame_queue_present_tag);

    return tag ? tag - 1u : UINT32_MAX;
}

/* Synchronous owner-thread endpoint called only after QueueAddEntry returns.
 * It remains a single fixed-size copy: diagnostics are emitted by queue_end. */
void isaac_vita_vitagl_known_color_probe(
    const IsaacVitaGlKnownColorProbe *event)
{
    if (!event || event->size != sizeof *event ||
            event->present_index != ISAAC_VITAGL_KNOWN_COLOR_PRESENT_INDEX ||
            isaac_vita_vitagl_known_color_present_index() !=
                event->present_index ||
            gl_vita_first_frame_atomic_load(
                &s_known_color_transfer_valid)) {
        return;
    }
    s_known_color_transfer = *event;
    gl_vita_first_frame_atomic_store(&s_known_color_transfer_valid, 1u);
}

static void gl_vita_known_color_log_after_swap(uint32_t present_index)
{
    IsaacVitaGlKnownColorProbe transfer;
    uint32_t transfer_valid;

    if (present_index != ISAAC_VITAGL_KNOWN_COLOR_PRESENT_INDEX ||
            s_known_color_logged) {
        return;
    }
    s_known_color_logged = 1u;
    GL_VITA_FF_LOG(
        "KAGE VITA FIRST FRAME: bid40=%.40s phase=known-color-clear "
        "p=%08x rgba=ff00ff00 attempted=%u saved=%08x/%08x "
        "vp=%d,%d,%d,%d mask=%x scissor=%u error=%08x/%08x end=ff",
        ISAAC_VITA_FIRST_FRAME_BUILD_ID, (unsigned)present_index,
        (unsigned)s_known_color_clear.attempted,
        (unsigned)s_known_color_clear.saved_read_framebuffer,
        (unsigned)s_known_color_clear.saved_draw_framebuffer,
        (int)s_known_color_clear.saved_viewport[0],
        (int)s_known_color_clear.saved_viewport[1],
        (int)s_known_color_clear.saved_viewport[2],
        (int)s_known_color_clear.saved_viewport[3],
        (unsigned)s_known_color_clear.saved_color_mask,
        (unsigned)s_known_color_clear.saved_scissor,
        (unsigned)s_known_color_clear.pre_error,
        (unsigned)s_known_color_clear.post_error);

    transfer_valid = gl_vita_first_frame_atomic_load(
        &s_known_color_transfer_valid);
    if (transfer_valid)
        transfer = s_known_color_transfer;
    else
        memset(&transfer, 0, sizeof transfer);
    GL_VITA_FF_LOG(
        "KAGE VITA FIRST FRAME: bid40=%.40s phase=known-color-transfer "
        "captured=%u p=%08x seq=%08x front=%08x back=%08x "
        "addr=%08x ded=%u color=%08x fmt=%08x rect=%u,%u,%u,%u "
        "stride=%u sync=%08x/%08x gate=%08x finish=%u fill=%08x "
        "transfer_finish=%u/%08x queue=%08x end=ff",
        ISAAC_VITA_FIRST_FRAME_BUILD_ID, (unsigned)transfer_valid,
        (unsigned)transfer.present_index, (unsigned)transfer.sequence,
        (unsigned)transfer.front_index, (unsigned)transfer.back_index,
        (unsigned)transfer.address, (unsigned)transfer.dedicated,
        (unsigned)transfer.color, (unsigned)transfer.format,
        (unsigned)transfer.x, (unsigned)transfer.y,
        (unsigned)transfer.width, (unsigned)transfer.height,
        (unsigned)transfer.stride_bytes, (unsigned)transfer.sync_object,
        (unsigned)transfer.sync_flags, (unsigned)transfer.gate_mask,
        (unsigned)transfer.context_finish_called,
        (unsigned)transfer.fill_result,
        (unsigned)transfer.transfer_finish_called,
        (unsigned)transfer.transfer_finish_result,
        (unsigned)transfer.queue_result);
}
#endif

void gl_vita_backend_first_frame_queue_end(void)
{
#if defined(ISAAC_VITA_KNOWN_COLOR_PROBE)
    uint32_t tag = gl_vita_first_frame_atomic_load(
        &s_first_frame_queue_present_tag);
#endif

    gl_vita_first_frame_atomic_store(
        &s_first_frame_queue_present_tag, 0u);
#if defined(ISAAC_VITA_KNOWN_COLOR_PROBE)
    if (tag)
        gl_vita_known_color_log_after_swap(tag - 1u);
#endif
}

static uint32_t gl_vita_first_frame_hash(
    const uint32_t *pixels, size_t count)
{
    uint32_t hash = 2166136261u;
    size_t index;

    for (index = 0u; index < count; ++index) {
        uint32_t value = pixels[index];
        unsigned byte;

        for (byte = 0u; byte < 4u; ++byte) {
            hash ^= value & 0xffu;
            hash *= 16777619u;
            value >>= 8u;
        }
    }
    return hash;
}

static void gl_vita_first_frame_readback_surface(
    uint32_t present_index, const char *surface, GLuint framebuffer,
    GLint x, GLint y, int available,
    GLenum sync_pre_error, GLenum sync_error)
{
    struct {
        uint32_t before[2];
        uint32_t pixel;
        uint32_t after[2];
    } guarded;
    GLint saved_read = 0;
    GLint saved_draw = 0;
    GLenum status = available
        ? (GLenum)GL_VITA_FF_FRAMEBUFFER_COMPLETE : 0u;
    GLenum read_error = 0u;
    GLenum restore_error;
    uint32_t hash = 0u;
    unsigned canary_ok;
    int attempted = available;

    /* Native diagnostic FBO binds can replay vitaGL's physical viewport. */
    gl_vita_typed_invalidate_viewport();
    guarded.before[0] = 0x13579bdfu;
    guarded.before[1] = 0x2468ace0u;
    guarded.pixel = 0u;
    guarded.after[0] = 0x0badc0deu;
    guarded.after[1] = 0xc001d00du;
    glGetIntegerv(
        (GLenum)GL_VITA_FF_READ_FRAMEBUFFER_BINDING, &saved_read);
    glGetIntegerv((GLenum)GL_VITA_FF_FRAMEBUFFER_BINDING, &saved_draw);
    if (attempted && framebuffer) {
        status = glCheckNamedFramebufferStatus(
            framebuffer, (GLenum)GL_VITA_FF_FRAMEBUFFER);
        if (status != (GLenum)GL_VITA_FF_FRAMEBUFFER_COMPLETE)
            attempted = 0;
    }
    if (attempted) {
        glBindFramebuffer(
            (GLenum)GL_VITA_FF_READ_FRAMEBUFFER, framebuffer);
        glReadPixels(
            x, y, GL_VITA_FF_READBACK_WIDTH,
            GL_VITA_FF_READBACK_HEIGHT,
            (GLenum)GL_VITA_FF_RGBA,
            (GLenum)GL_VITA_FF_UNSIGNED_BYTE, &guarded.pixel);
        read_error = glGetError();
        hash = gl_vita_first_frame_hash(
            &guarded.pixel, 1u);
    }
    glBindFramebuffer(
        (GLenum)GL_VITA_FF_READ_FRAMEBUFFER, (GLuint)saved_read);
    glBindFramebuffer(
        (GLenum)GL_VITA_FF_DRAW_FRAMEBUFFER, (GLuint)saved_draw);
    restore_error = glGetError();
    canary_ok = guarded.before[0] == 0x13579bdfu &&
        guarded.before[1] == 0x2468ace0u &&
        guarded.after[0] == 0x0badc0deu &&
        guarded.after[1] == 0xc001d00du;

    GL_VITA_FF_LOG(
        "KAGE VITA FIRST FRAME: bid40=%.40s phase=readback "
        "present=%u surface=%s fbo=%u attach_arg=%u att_calls=%u "
        "available=%u status=0x%08x attempted=%u sync=0x%08x/0x%08x "
        "read=0x%08x restore=0x%08x canary=%u hash=0x%08x "
        "nonblack=%u rgba=%08x end=ff",
        ISAAC_VITA_FIRST_FRAME_BUILD_ID, (unsigned)present_index,
        surface, (unsigned)framebuffer,
        (unsigned)s_first_frame.manager_color_attach_arg,
        (unsigned)s_first_frame.manager_color_attach_calls,
        (unsigned)available, (unsigned)status, (unsigned)attempted,
        (unsigned)sync_pre_error, (unsigned)sync_error,
        (unsigned)read_error, (unsigned)restore_error, canary_ok,
        (unsigned)hash,
        (unsigned)((guarded.pixel & 0x00ffffffu) != 0u),
        (unsigned)guarded.pixel);
}

static void gl_vita_first_frame_readback(uint32_t present_index)
{
    GLenum pre_error;
    GLenum sync_error;

    if (gl_vita_first_frame_boundary_index(present_index) < 0)
        return;
    pre_error = glGetError();
    glFinish();
    sync_error = glGetError();
#if !defined(ISAAC_VITA_DIRECT_DEFAULT)
    gl_vita_first_frame_readback_surface(
        present_index, "manager", (GLuint)s_first_frame.manager_fbo,
        GL_VITA_FF_LOGICAL_WIDTH / 2,
        GL_VITA_FF_LOGICAL_HEIGHT / 2,
        s_first_frame.manager_fbo != 0u,
        pre_error, sync_error);
#endif
    gl_vita_first_frame_readback_surface(
        present_index, "default", 0u,
        /* vitaGL flips default-FBO readback Y, so GL row 272 is physical
         * scanout row 271 sampled by the queue callback. */
        GL_VITA_FF_DISPLAY_WIDTH / 2,
        GL_VITA_FF_DISPLAY_HEIGHT / 2,
        1,
        pre_error, sync_error);
}

#if defined(ISAAC_VITA_KNOWN_COLOR_PROBE)
static void gl_vita_known_color_clear(uint32_t present_index)
{
    gl_vita_known_color_clear_state state;
    GLint saved_read = 0;
    GLint saved_draw = 0;
    GLint viewport[4] = { 0, 0, GL_VITA_FF_DISPLAY_WIDTH,
                          GL_VITA_FF_DISPLAY_HEIGHT };
    GLboolean color_mask[4] = { 1u, 1u, 1u, 1u };
    GLfloat clear_color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    GLboolean scissor_enabled;

    gl_vita_typed_invalidate_viewport();
    memset(&state, 0, sizeof state);
    state.present_index = present_index;
    state.pre_error = (uint32_t)glGetError();
    scissor_enabled = glIsEnabled((GLenum)GL_VITA_FF_SCISSOR_TEST);
    state.saved_scissor = (uint32_t)(scissor_enabled != 0u);
    glGetIntegerv(
        (GLenum)GL_VITA_FF_READ_FRAMEBUFFER_BINDING, &saved_read);
    glGetIntegerv((GLenum)GL_VITA_FF_FRAMEBUFFER_BINDING, &saved_draw);
    state.saved_read_framebuffer = (uint32_t)saved_read;
    state.saved_draw_framebuffer = (uint32_t)saved_draw;
    if (scissor_enabled || saved_draw != 0) {
        state.post_error = (uint32_t)glGetError();
        s_known_color_clear = state;
        return;
    }

    state.attempted = 1u;
    glGetIntegerv((GLenum)GL_VITA_FF_VIEWPORT, viewport);
    glGetBooleanv((GLenum)GL_VITA_FF_COLOR_WRITEMASK, color_mask);
    glGetFloatv((GLenum)GL_VITA_FF_COLOR_CLEAR_VALUE, clear_color);

    memcpy(state.saved_viewport, viewport, sizeof viewport);
    state.saved_color_mask =
        ((uint32_t)(color_mask[0] != 0u) << 0u) |
        ((uint32_t)(color_mask[1] != 0u) << 1u) |
        ((uint32_t)(color_mask[2] != 0u) << 2u) |
        ((uint32_t)(color_mask[3] != 0u) << 3u);

    glViewport(0, 0, GL_VITA_FF_DISPLAY_WIDTH, GL_VITA_FF_DISPLAY_HEIGHT);
    glColorMask(1u, 1u, 1u, 1u);
    glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    glClear((GLbitfield)GL_VITA_FF_COLOR_BUFFER_BIT);

    glClearColor(clear_color[0], clear_color[1],
                 clear_color[2], clear_color[3]);
    glColorMask(color_mask[0], color_mask[1],
                color_mask[2], color_mask[3]);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    state.post_error = (uint32_t)glGetError();
    s_known_color_clear = state;
    gl_vita_first_frame_increment(&s_first_frame.control_clears);
}
#endif

#if !defined(ISAAC_VITA_DIRECT_DEFAULT)
static void gl_vita_first_frame_control_clear(uint32_t present_index)
{
    GLint framebuffer = 0;
    GLint viewport[4] = { 0, 0, GL_VITA_FF_DISPLAY_WIDTH,
                          GL_VITA_FF_DISPLAY_HEIGHT };
    GLboolean color_mask[4] = { 1u, 1u, 1u, 1u };
    GLfloat clear_color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    GLboolean scissor_enabled;
    GLenum pre_error;
    GLenum post_error;

    gl_vita_typed_invalidate_viewport();
    pre_error = glGetError();
    glGetIntegerv((GLenum)GL_VITA_FF_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv((GLenum)GL_VITA_FF_VIEWPORT, viewport);
    glGetBooleanv((GLenum)GL_VITA_FF_COLOR_WRITEMASK, color_mask);
    glGetFloatv((GLenum)GL_VITA_FF_COLOR_CLEAR_VALUE, clear_color);
    scissor_enabled = glIsEnabled((GLenum)GL_VITA_FF_SCISSOR_TEST);

    glBindFramebuffer((GLenum)GL_VITA_FF_FRAMEBUFFER, 0u);
    glViewport(0, 0, GL_VITA_FF_DISPLAY_WIDTH, GL_VITA_FF_DISPLAY_HEIGHT);
    if (scissor_enabled)
        glDisable((GLenum)GL_VITA_FF_SCISSOR_TEST);
    glColorMask(1u, 1u, 1u, 1u);
    glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
    glClear((GLbitfield)GL_VITA_FF_COLOR_BUFFER_BIT);
    glClearColor(clear_color[0], clear_color[1],
                 clear_color[2], clear_color[3]);
    glColorMask(color_mask[0], color_mask[1],
                color_mask[2], color_mask[3]);
    if (scissor_enabled)
        glEnable((GLenum)GL_VITA_FF_SCISSOR_TEST);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glBindFramebuffer(
        (GLenum)GL_VITA_FF_FRAMEBUFFER, (GLuint)framebuffer);
    post_error = glGetError();
    gl_vita_first_frame_increment(&s_first_frame.control_clears);

    if (present_index == 0u) {
        GL_VITA_FF_LOG(
            "KAGE VITA FIRST FRAME: bid40=%.40s phase=control-clear "
            "range=[0,%u) result=gpu-clear-fbo0 saved_fbo=%u "
            "pre_error=0x%08x post_error=0x%08x",
            ISAAC_VITA_FIRST_FRAME_BUILD_ID,
            (unsigned)ISAAC_VITA_FIRST_FRAME_CLEAR_END_PRESENT,
            (unsigned)framebuffer, (unsigned)pre_error,
            (unsigned)post_error);
    }
}

static void gl_vita_first_frame_blit(uint32_t present_index)
{
    uint32_t fbo = s_first_frame.manager_fbo;
    GLenum status;
    GLenum pre_error;
    GLenum post_error;
    GLboolean scissor_enabled;
    uint32_t result_bit;
    const char *result;

    if (!fbo) {
        gl_vita_first_frame_increment(&s_first_frame.blit_no_manager);
        result_bit = 1u;
        result = "skip-zero-manager";
        status = 0u;
        pre_error = 0u;
        post_error = 0u;
    } else {
        status = glCheckNamedFramebufferStatus(
            (GLuint)fbo, (GLenum)GL_VITA_FF_FRAMEBUFFER);
        if (status != (GLenum)GL_VITA_FF_FRAMEBUFFER_COMPLETE) {
            gl_vita_first_frame_increment(&s_first_frame.blit_incomplete);
            result_bit = 2u;
            result = "skip-incomplete";
            pre_error = 0u;
            post_error = 0u;
        } else {
            pre_error = glGetError();
            scissor_enabled = glIsEnabled(
                (GLenum)GL_VITA_FF_SCISSOR_TEST);
            if (scissor_enabled)
                glDisable((GLenum)GL_VITA_FF_SCISSOR_TEST);
            gl_vita_first_frame_increment(&s_first_frame.blit_attempts);
            glBlitNamedFramebuffer(
                (GLuint)fbo, 0u,
                0, 0, GL_VITA_FF_LOGICAL_WIDTH,
                GL_VITA_FF_LOGICAL_HEIGHT,
                0, GL_VITA_FF_VIEWPORT_Y, GL_VITA_FF_DISPLAY_WIDTH,
                GL_VITA_FF_VIEWPORT_Y + GL_VITA_FF_LOGICAL_HEIGHT,
                (GLbitfield)GL_VITA_FF_COLOR_BUFFER_BIT,
                (GLenum)GL_VITA_FF_NEAREST);
            post_error = glGetError();
            if (scissor_enabled)
                glEnable((GLenum)GL_VITA_FF_SCISSOR_TEST);
            if (post_error == (GLenum)GL_VITA_FF_NO_ERROR) {
                gl_vita_first_frame_increment(
                    &s_first_frame.blit_successes);
                result_bit = 4u;
                result = "blit-ok";
            } else {
                result_bit = 8u;
                result = "blit-error";
            }
        }
    }

    if (!(s_first_frame_result_log_mask & result_bit)) {
        s_first_frame_result_log_mask |= result_bit;
        GL_VITA_FF_LOG(
            "KAGE VITA FIRST FRAME: bid40=%.40s phase=explicit-blit "
            "present=%u result=%s manager_fbo=%u status=0x%08x "
            "pre_error=0x%08x post_error=0x%08x src=0,0,960,540 "
            "dst=0,2,960,542",
            ISAAC_VITA_FIRST_FRAME_BUILD_ID, (unsigned)present_index,
            result, (unsigned)fbo, (unsigned)status,
            (unsigned)pre_error, (unsigned)post_error);
    }
}
#endif

void gl_vita_backend_first_frame_before_present(uint32_t present_index)
{
    gl_vita_first_frame_queue_poll(present_index);
#if defined(ISAAC_VITA_KNOWN_COLOR_PROBE)
    if (present_index == ISAAC_VITAGL_KNOWN_COLOR_PRESENT_INDEX)
        gl_vita_known_color_clear(present_index);
#endif
#if defined(ISAAC_VITA_DIRECT_DEFAULT)
    if (present_index <= 1u)
        gl_vita_first_frame_log_summary(
            "true-direct-default-passive", present_index);
    gl_vita_first_frame_readback(present_index);
#else
    if (present_index < ISAAC_VITA_FIRST_FRAME_CLEAR_END_PRESENT) {
        gl_vita_first_frame_control_clear(present_index);
        gl_vita_first_frame_readback(present_index);
        return;
    }
    if (present_index < ISAAC_VITA_FIRST_FRAME_BLIT_END_PRESENT) {
        if (present_index == ISAAC_VITA_FIRST_FRAME_CLEAR_END_PRESENT)
            gl_vita_first_frame_log_summary("control-clear-complete",
                                            present_index);
        gl_vita_first_frame_blit(present_index);
        if (present_index + 1u ==
                ISAAC_VITA_FIRST_FRAME_BLIT_END_PRESENT) {
            gl_vita_first_frame_log_summary("explicit-blit-complete",
                                            present_index + 1u);
            s_first_frame_final_logged = 1u;
        }
        gl_vita_first_frame_readback(present_index);
        return;
    }
    if (!s_first_frame_final_logged) {
        gl_vita_first_frame_log_summary("explicit-blit-complete",
                                        present_index);
        s_first_frame_final_logged = 1u;
    }
    gl_vita_first_frame_readback(present_index);
#endif
}

#undef GL_VITA_FF_LOG
#endif

#if !defined(ISAAC_GL_VITA_BACKEND_ORACLE)
void isaac_vita_log(const char *format, ...);

static IsaacVitaGlDisplaySurfaceStatus s_display_surface_status;
static volatile uint32_t s_display_surface_status_valid;

void isaac_vita_vitagl_display_surface_status(
    const IsaacVitaGlDisplaySurfaceStatus *status)
{
    if (!status || status->size != sizeof(*status)) {
        memset(&s_display_surface_status, 0,
               sizeof(s_display_surface_status));
        __sync_synchronize();
        s_display_surface_status_valid = 2u;
        isaac_vita_log(
            "KAGE VITA DISPLAY SURFACE: profile=dedicated-scanout-v1 "
            "status=invalid ptr=0x%08x size=%u expected=%u",
            (unsigned)(uintptr_t)status,
            status ? (unsigned)status->size : 0u,
            (unsigned)sizeof(*status));
        return;
    }

    s_display_surface_status = *status;
    __sync_synchronize();
    s_display_surface_status_valid = 1u;
    isaac_vita_log(
        "KAGE VITA DISPLAY SURFACE: profile=dedicated-scanout-v1 "
        "size=%u count=%u dedicated=%u system=%u fail=%u/%u "
        "b0=%08x/%08x/%08x/%u/%08x/%08x "
        "b1=%08x/%08x/%08x/%u/%08x/%08x "
        "b2=%08x/%08x/%08x/%u/%08x/%08x",
        (unsigned)status->display_size, (unsigned)status->buffer_count,
        (unsigned)status->dedicated_count,
        (unsigned)status->system_app_mode,
        (unsigned)status->failure_stage,
        (unsigned)status->failure_index,
        (unsigned)status->alloc_result[0],
        (unsigned)status->get_base_result[0],
        (unsigned)status->map_result[0],
        (unsigned)status->dedicated[0],
        (unsigned)status->color_init_result[0],
        (unsigned)status->sync_create_result[0],
        (unsigned)status->alloc_result[1],
        (unsigned)status->get_base_result[1],
        (unsigned)status->map_result[1],
        (unsigned)status->dedicated[1],
        (unsigned)status->color_init_result[1],
        (unsigned)status->sync_create_result[1],
        (unsigned)status->alloc_result[2],
        (unsigned)status->get_base_result[2],
        (unsigned)status->map_result[2],
        (unsigned)status->dedicated[2],
        (unsigned)status->color_init_result[2],
        (unsigned)status->sync_create_result[2]);
}

int gl_vita_backend_get_display_surface_status(
    IsaacVitaGlDisplaySurfaceStatus *status)
{
    uint32_t valid = s_display_surface_status_valid;

    if (valid != 1u || !status)
        return 0;
    __sync_synchronize();
    *status = s_display_surface_status;
    return 1;
}

void isaac_vita_vitagl_display_surface_lifecycle_failure(
    const char *site, int32_t result)
{
    isaac_vita_log(
        "KAGE VITA DISPLAY SURFACE LIFECYCLE FAILURE: "
        "profile=dedicated-scanout-v1 site=%s result=0x%08x "
        "action=fail-closed",
        site ? site : "unknown", (unsigned)result);
}

/* Strong project-side endpoints for the weak vitaGL overlay hooks.  Every
 * current non-splash reserve/scene-reset path runs under typed guest GL
 * dispatch and becomes one exact guest fault.  If a future native path calls
 * one outside an active guest CPU, it still durable-logs and flushes the
 * profile before guest_gl_backend_fault fail-closes via host abort. */
void isaac_vita_vitagl_reserve_failure(
    const char *site, int32_t result, const void *buffer,
    uint32_t vertex_ring_bytes, uint32_t fragment_ring_bytes)
{
    isaac_vita_log(
        "KAGE VITA GXM RESERVE FAILURE: profile=gxm-io-v1 "
        "site=%s result=0x%08x "
        "buffer=0x%08x vertex_ring=%u fragment_ring=%u",
        site ? site : "unknown", (unsigned)result,
        (unsigned)(uintptr_t)buffer, (unsigned)vertex_ring_bytes,
        (unsigned)fragment_ring_bytes);
    kage_vita_io_profile_report("gxm-reserve");
    guest_gl_backend_fault(
        (uint32_t)result, "vitaGL default uniform reserve failed");
}

void isaac_vita_vitagl_scene_failure(
    const char *site, int32_t result, const void *target, uint32_t frame)
{
    isaac_vita_log(
        "KAGE VITA GXM SCENE FAILURE: profile=gxm-io-v1 "
        "site=%s result=0x%08x target=0x%08x frame=%u",
        site ? site : "unknown", (unsigned)result,
        (unsigned)(uintptr_t)target, (unsigned)frame);
    kage_vita_io_profile_report("scene-begin");
    guest_gl_backend_fault(
        (uint32_t)result, "vitaGL scene begin failed");
}
#endif

#if defined(ISAAC_GL_VITA_BACKEND_ORACLE)
void isaac_gl_vita_oracle_rt_log(const char *format, ...);
void isaac_gl_vita_oracle_rt_profile_report(const char *reason);
void isaac_gl_vita_oracle_rt_fault(uint32_t result, const char *message);
# define ISAAC_VITAGL_RT_LOG isaac_gl_vita_oracle_rt_log
# define ISAAC_VITAGL_RT_REPORT isaac_gl_vita_oracle_rt_profile_report
# define ISAAC_VITAGL_RT_FAULT isaac_gl_vita_oracle_rt_fault
#else
# define ISAAC_VITAGL_RT_LOG isaac_vita_log
# define ISAAC_VITAGL_RT_REPORT kage_vita_io_profile_report
# define ISAAC_VITAGL_RT_FAULT guest_gl_backend_fault
#endif

void isaac_vita_vitagl_render_target_event(
    const IsaacVitaGlRenderTargetTelemetry *event)
{
    uint32_t fault_result;

    if (!event) {
        ISAAC_VITAGL_RT_LOG(
            "KAGE VITA GXM RT FAILURE: profile=gxm-io-v1 "
            "site=render-target:null-event");
        ISAAC_VITAGL_RT_REPORT("render-target-acquire");
        ISAAC_VITAGL_RT_FAULT(
            0x805b0004u, "vitaGL render-target telemetry was null");
        return;
    }

#if defined(ISAAC_VITA_IO_PROFILE)
    /* The overlay reports consumption of the early reserve, and every
     * transactional failure/recovery, at the actual scene-reset FBO boundary.
     * Sample the first 1024x1024 event there without changing its recipe. */
    if (event->width == 1024u && event->height == 1024u && event->site &&
            !strncmp(event->site, "scene_reset:framebuffer:", 24u)) {
        kage_vita_backend_memory_snapshot_at_first_fbo(
            event->first_result, event->first_target);
    }
#endif

    ISAAC_VITAGL_RT_LOG(
        "KAGE VITA GXM RT %s: profile=gxm-io-v1 site=%.42s "
        "f=0x%08x/0x%08x r=0x%08x/0x%08x fin=0x%08x fc=%u del=0x%08x "
        "t=%u full=%u dup=%u inv=%u p=%u,%u,%u,%u "
        "drain=%u live=%u refs=%u "
        "wh=%ux%u frame=%u",
        event->recovered ? "RECOVERED" : "FAILURE",
        event->site ? event->site : "unknown",
        (unsigned)event->first_result,
        (unsigned)(uintptr_t)event->first_target,
        (unsigned)event->retry_result,
        (unsigned)(uintptr_t)event->retry_target,
        (unsigned)event->finish_result,
        (unsigned)event->finish_called,
        (unsigned)event->destroy_result,
        (unsigned)event->retry_attempted, (unsigned)event->pool_full,
        (unsigned)event->pending_duplicate,
        (unsigned)event->invariant_failure,
        (unsigned)event->pending[0], (unsigned)event->pending[1],
        (unsigned)event->pending[2], (unsigned)event->pending[3],
        (unsigned)event->drained_targets, (unsigned)event->live_targets,
        (unsigned)event->reference_sum, (unsigned)event->width,
        (unsigned)event->height, (unsigned)event->frame);
    if (event->recovered)
        return;

    ISAAC_VITAGL_RT_REPORT("render-target-acquire");
    if (event->pool_full) {
        fault_result = 0x805b0027u;
    } else if (event->invariant_failure || event->pending_duplicate) {
        fault_result = 0x805b0004u;
    } else if (event->retry_attempted) {
        fault_result = (uint32_t)event->retry_result;
    } else {
        fault_result = (uint32_t)event->first_result;
    }
    if (!fault_result)
        fault_result = event->finish_result ?
            (uint32_t)event->finish_result :
            (event->destroy_result ? (uint32_t)event->destroy_result :
             0x805b0004u);
    ISAAC_VITAGL_RT_FAULT(
        fault_result, "vitaGL render-target acquire failed");
}

#undef ISAAC_VITAGL_RT_LOG
#undef ISAAC_VITAGL_RT_REPORT
#undef ISAAC_VITAGL_RT_FAULT

_Static_assert(sizeof s_missing_symbols / sizeof s_missing_symbols[0] ==
               GL_VITA_MISSING_COUNT,
               "Vita GL missing-symbol inventory/count drifted");

static void gl_vita_rbo_reset(void)
{
    memset(s_renderbuffers, 0, sizeof s_renderbuffers);
    s_bound_renderbuffer = 0u;
}

static gl_vita_renderbuffer_record *gl_vita_rbo_find(guest_gl_uint name)
{
    size_t index;

    if (!name)
        return NULL;
    for (index = 0u; index < GL_VITA_RBO_CAPACITY; ++index) {
        if (s_renderbuffers[index].name == name)
            return &s_renderbuffers[index];
    }
    return NULL;
}

static gl_vita_renderbuffer_record *gl_vita_rbo_add(guest_gl_uint name)
{
    gl_vita_renderbuffer_record *record;
    size_t index;

    record = gl_vita_rbo_find(name);
    if (record) {
        record->internal_format = 0u;
        record->width = 0;
        record->height = 0;
        return record;
    }
    for (index = 0u; index < GL_VITA_RBO_CAPACITY; ++index) {
        if (!s_renderbuffers[index].name) {
            s_renderbuffers[index].name = name;
            s_renderbuffers[index].internal_format = 0u;
            s_renderbuffers[index].width = 0;
            s_renderbuffers[index].height = 0;
            return &s_renderbuffers[index];
        }
    }
    s_last_error = "Vita GL renderbuffer registry exhausted";
    return NULL;
}

static size_t gl_vita_rbo_free_count(void)
{
    size_t free_count = 0u;
    size_t index;

    for (index = 0u; index < GL_VITA_RBO_CAPACITY; ++index) {
        if (!s_renderbuffers[index].name)
            ++free_count;
    }
    return free_count;
}

static void gl_vita_rbo_remove(guest_gl_uint name)
{
    gl_vita_renderbuffer_record *record = gl_vita_rbo_find(name);

    if (record)
        memset(record, 0, sizeof *record);
    if (s_bound_renderbuffer == name)
        s_bound_renderbuffer = 0u;
}

int gl_vita_backend_register_renderbuffer(
    uint32_t renderbuffer, uint32_t internal_format,
    int32_t width, int32_t height)
{
    gl_vita_renderbuffer_record *record;

    if (!s_installed) {
        s_last_error = "Vita GL renderbuffer registration before install";
        return 0;
    }
    if (!renderbuffer || width < 0 || height < 0) {
        s_last_error = "Vita GL invalid native renderbuffer registration";
        return 0;
    }
    record = gl_vita_rbo_add((guest_gl_uint)renderbuffer);
    if (!record)
        return 0;
    record->internal_format = (guest_gl_enum)internal_format;
    record->width = (guest_gl_sizei)width;
    record->height = (guest_gl_sizei)height;
    s_bound_renderbuffer = (guest_gl_uint)renderbuffer;
    return 1;
}

void gl_vita_backend_unregister_renderbuffer(uint32_t renderbuffer)
{
    gl_vita_rbo_remove((guest_gl_uint)renderbuffer);
}

static GUEST_NORETURN void gl_vita_rbo_fault(
    uint32_t token, const char *message)
{
    s_last_error = message;
    guest_gl_backend_fault(token, message);
}

#if defined(GL_VITA_FBO_TABLE)
/* Offscreen render-target table.
 *
 * Measured rationale (Bundle35, 720x408 display raster, real gameplay; ph120.c
 * gl(c,f) against ph120.f fbo(d,o), 1494 windows): every present with
 * offscreen draws (1001 gameplay/pause windows, exactly four such draws per
 * present) binds five framebuffers and issues five glClear calls, i.e. two
 * offscreen passes around the display pass, each pass being bind(mgr) ->
 * attach(surface) -> glClear -> draws -> glClear(GL_DEPTH_BUFFER_BIT) ->
 * bind(0) (the A02 title-screen trace shows the same shape).  Presents with no
 * offscreen draws (480 menu windows) issue one clear and one bind.  KAGE owns
 * a single manager framebuffer ('KAGE VITA READY: ... fbo=<one name>') and
 * swaps its COLOR_ATTACHMENT0 per pass.  In the pinned vitaGL 73dd57a
 * glBindFramebuffer only stores a pointer; the GXM scene switch
 * (sceGxmEndScene/BeginScene = full tile store and reload of a 960x540 RGBA8
 * surface) is deferred to the next glClear or draw (gxm.c:572 scene_reset,
 * misc.c:640 glClear), and the stock-reference build has no
 * STORE_DEPTH_STENCIL, so FBO depth/stencil is neither loaded nor stored
 * across scenes (gxm.c init_depth_stencil_buffer): every scene starts at the
 * GXM background depth 1.0 / stencil 0 and a depth clear is only observable
 * by later draws of the same scene.
 *
 * Both policies are exact at the pixel level or an explicit user mode:
 *  - FBO_CLEAR_ELISION: the colour part of a guest glClear on an offscreen
 *    target is a no-op when the attached texture is known to hold exactly the
 *    current clear colour; that knowledge is keyed by texture (vitaGL's
 *    colour surface is the texture's own storage, shared.h
 *    _glFramebufferTexture2D, so it survives re-attachment and scene
 *    switches) and is forgotten by any draw through the texture, any texture
 *    upload or deletion.  The depth/stencil part is never dropped on its own:
 *    it is owed and replayed as a native glClear right before the first draw
 *    on that target (same scene, same values as the stock quad), folded into
 *    the next native clear of the target, or dropped only where stock vitaGL
 *    ends that scene anyway (clear or draw on another framebuffer, present,
 *    framebuffer deletion).  Rare calls whose scene effect is not modelled
 *    (attachment change, glReadPixels, texture upload/deletion, scissor
 *    enable) issue the owed clear first.  The guest GL surface
 *    (gl_surface_generated.h) has no glScissor, glColorMask, glDepthMask,
 *    glStencilMask, glClearStencil, glDisable, glFinish, glFlush or
 *    glDrawArrays; vitaGL's clear quad is drawn with an unblended patched
 *    fragment program (vgl.c:276), disables the fragment program for
 *    depth/stencil-only clears, and covers the whole surface unless
 *    GL_SCISSOR_TEST is enabled.  A guest glEnable(GL_SCISSOR_TEST) therefore
 *    permanently poisons the policy.  Expected effect on the measured frame:
 *    the trailing depth-only clear of each non-empty pass is dropped (gl(c)
 *    5 -> 3 per present); a pass that draws nothing into a still-clean
 *    surface costs no scene and no quad at all.
 *  - FBO_CLEAR_ELISION_DEPTH_DROP (sub-mode of the above): colour clears are
 *    always native and the colour notes are never consulted, so only the
 *    depth/stencil debt model is active; and the owed clear is dropped, not
 *    replayed, when the owing framebuffer re-attaches a non-zero level-0 2D
 *    texture at COLOR_ATTACHMENT0 (vita_glFramebufferTexture2D), because
 *    vitaGL then ends that scene at the next clear or draw and FBO depth is
 *    never stored.  Measured frame (exp-clear, 120 loops): 360 depth-only
 *    clears absorbed, 240 replayed at the attach, 120 dropped at the display
 *    clear; the sub-mode turns the 240 replays into drops (7 -> 4 native
 *    clears per frame) and ph120.e prints clear(x,r,p,a,d).
 *  - FBO_RASTER_SCALE: colour targets of at least 512x512 logical pixels that
 *    are defined without pixel data (Isaac's 960x540 surfaces) are allocated
 *    at NUM/DEN and the viewport is scaled with floor semantics while such a
 *    target is the draw framebuffer.  Composites sample through normalized
 *    texture coordinates so only the raster of that layer changes. */
# define GL_VITA_FBO_COLOR_ATTACHMENT0     0x00008ce0u
# define GL_VITA_FBO_READ_FRAMEBUFFER      0x00008ca8u
# define GL_VITA_FBO_COLOR_BUFFER_BIT      0x00004000u
# define GL_VITA_FBO_DEPTH_BUFFER_BIT      0x00000100u
# define GL_VITA_FBO_STENCIL_BUFFER_BIT    0x00000400u
# define GL_VITA_FBO_SCISSOR_TEST          0x00000c11u
# define GL_VITA_FBO_RECORDS               16u

typedef struct gl_vita_fbo_record {
    guest_gl_uint name;             /* 0 = free slot */
    guest_gl_uint color_texture;    /* level-0 2D colour attachment, 0 = unknown */
    guest_gl_uint depth_renderbuffer;
    uint8_t scaled;                 /* colour texture lives at NUM/DEN */
} gl_vita_fbo_record;

static gl_vita_fbo_record s_fbo_records[GL_VITA_FBO_RECORDS];

static gl_vita_fbo_record *gl_vita_fbo_find(guest_gl_uint name)
{
    uint32_t index;

    if (!name)
        return NULL;
    for (index = 0u; index < GL_VITA_FBO_RECORDS; ++index)
        if (s_fbo_records[index].name == name)
            return &s_fbo_records[index];
    return NULL;
}

static gl_vita_fbo_record *gl_vita_fbo_acquire(guest_gl_uint name)
{
    gl_vita_fbo_record *record = gl_vita_fbo_find(name);
    uint32_t index;

    if (record || !name)
        return record;
    for (index = 0u; index < GL_VITA_FBO_RECORDS; ++index) {
        if (!s_fbo_records[index].name) {
            record = &s_fbo_records[index];
            memset(record, 0, sizeof *record);
            record->name = name;
            return record;
        }
    }
    /* Table full: the name simply has no record and keeps stock behaviour. */
    return NULL;
}

static void gl_vita_fbo_forget(guest_gl_uint name)
{
    gl_vita_fbo_record *record = gl_vita_fbo_find(name);

    if (record)
        memset(record, 0, sizeof *record);
}

#if defined(ISAAC_VITA_FBO_CLEAR_ELISION)
/* Colour knowledge keyed by texture name: the texture is known to hold
 * exactly |bits| in every pixel.  A texture attached to several framebuffers
 * shares one note, so writes through any of them forget it. */
typedef struct gl_vita_fbo_color_note {
    guest_gl_uint texture;          /* 0 = free slot */
    uint32_t bits[4];               /* exact float bits of the held colour */
} gl_vita_fbo_color_note;

static gl_vita_fbo_color_note s_fbo_color_notes[GL_VITA_FBO_RECORDS];
static uint32_t s_fbo_clear_color_bits[4];
static uint64_t s_fbo_clear_depth_bits;
static uint8_t s_fbo_elision_poisoned;
/* Owed depth/stencil clear: the framebuffer that owes it (0 = none), the
 * buffer bits, and the clamped clear depth it has to be issued with. */
static guest_gl_uint s_fbo_owed_framebuffer;
static guest_gl_bitfield s_fbo_owed_mask;
static uint64_t s_fbo_owed_depth_bits;

static gl_vita_fbo_color_note *gl_vita_fbo_color_find(guest_gl_uint texture)
{
    uint32_t index;

    if (!texture)
        return NULL;
    for (index = 0u; index < GL_VITA_FBO_RECORDS; ++index)
        if (s_fbo_color_notes[index].texture == texture)
            return &s_fbo_color_notes[index];
    return NULL;
}

static void gl_vita_fbo_color_forget(guest_gl_uint texture)
{
    gl_vita_fbo_color_note *note = gl_vita_fbo_color_find(texture);

    if (note)
        memset(note, 0, sizeof *note);
}

static void gl_vita_fbo_color_forget_all(void)
{
    memset(s_fbo_color_notes, 0, sizeof s_fbo_color_notes);
}

/* The texture now holds the current clear colour everywhere.  A full table
 * simply leaves the texture unknown (stock behaviour). */
static void gl_vita_fbo_color_remember(guest_gl_uint texture)
{
    gl_vita_fbo_color_note *note = gl_vita_fbo_color_find(texture);
    uint32_t index;

    if (!texture)
        return;
    for (index = 0u; !note && index < GL_VITA_FBO_RECORDS; ++index)
        if (!s_fbo_color_notes[index].texture)
            note = &s_fbo_color_notes[index];
    if (!note)
        return;
    note->texture = texture;
    memcpy(note->bits, s_fbo_clear_color_bits, sizeof note->bits);
}
#endif

/* Guest names arrive in a guest array; read it only when the bounds are sane,
 * exactly like vita_glDeleteFramebuffers does. */
static int gl_vita_fbo_guest_array_valid(
    guest_gl_sizei count, guest_gl_addr names)
{
    return count > 0 && names &&
        (uint32_t)count <= (UINT32_MAX - names) / 4u;
}
#endif

#if defined(ISAAC_VITA_FBO_RASTER_SCALE)
# if !defined(ISAAC_VITA_FBO_RASTER_SCALE_NUM) || \
        !defined(ISAAC_VITA_FBO_RASTER_SCALE_DEN)
#  error "ISAAC_VITA_FBO_RASTER_SCALE needs _NUM and _DEN definitions"
# endif
_Static_assert(ISAAC_VITA_FBO_RASTER_SCALE_NUM >= 1 &&
               ISAAC_VITA_FBO_RASTER_SCALE_DEN >= 2 &&
               ISAAC_VITA_FBO_RASTER_SCALE_NUM <
                   ISAAC_VITA_FBO_RASTER_SCALE_DEN &&
               ISAAC_VITA_FBO_RASTER_SCALE_DEN <= 16,
               "FBO raster scale must be a proper fraction NUM/DEN, DEN <= 16");
/* Isaac's world surfaces are 480x270; only its screen-sized 960x540 surfaces
 * (colour modifier / render / HQX surfaces) reach this edge. */
# define GL_VITA_FBO_RASTER_MIN_EDGE   512
# define GL_VITA_SCALED_TEXTURES       32u

typedef struct gl_vita_scaled_texture {
    guest_gl_uint name;             /* 0 = free slot */
    guest_gl_int internal_format;
    guest_gl_sizei logical_width;
    guest_gl_sizei logical_height;
    guest_gl_enum format;
    guest_gl_enum type;
} gl_vita_scaled_texture;

static gl_vita_scaled_texture s_scaled_textures[GL_VITA_SCALED_TEXTURES];
void isaac_vita_log(const char *format, ...);
/* Guest texture-unit shadow: which 2D texture name glTexImage2D addresses. */
static uint32_t s_fbo_active_unit;
static guest_gl_uint s_fbo_bound_2d[GL_VITA_TEXTURE_UNITS];
static uint8_t s_fbo_raster_readback_logged;
static uint8_t s_fbo_raster_respecify_logged;

/* Scale an integer edge by NUM/DEN with mathematical floor semantics (same
 * contract as gl_vita_scale_edge_3_4).  |edge| stays below 2^33 and DEN <= 16,
 * so the product cannot overflow int64_t. */
static int64_t gl_vita_fbo_scale_edge(int64_t edge)
{
    int64_t numerator = edge * ISAAC_VITA_FBO_RASTER_SCALE_NUM;
    int64_t scaled = numerator / ISAAC_VITA_FBO_RASTER_SCALE_DEN;

    if (numerator % ISAAC_VITA_FBO_RASTER_SCALE_DEN < 0)
        --scaled;
    return scaled;
}

static guest_gl_sizei gl_vita_fbo_scale_extent(guest_gl_sizei extent)
{
    int64_t scaled = gl_vita_fbo_scale_edge((int64_t)extent);

    return scaled < 1 ? 1 : (guest_gl_sizei)scaled;
}

static gl_vita_scaled_texture *gl_vita_scaled_find(guest_gl_uint name)
{
    uint32_t index;

    if (!name)
        return NULL;
    for (index = 0u; index < GL_VITA_SCALED_TEXTURES; ++index)
        if (s_scaled_textures[index].name == name)
            return &s_scaled_textures[index];
    return NULL;
}

static gl_vita_scaled_texture *gl_vita_scaled_acquire(guest_gl_uint name)
{
    gl_vita_scaled_texture *record = gl_vita_scaled_find(name);
    uint32_t index;

    if (record || !name)
        return record;
    for (index = 0u; index < GL_VITA_SCALED_TEXTURES; ++index) {
        if (!s_scaled_textures[index].name) {
            record = &s_scaled_textures[index];
            memset(record, 0, sizeof *record);
            record->name = name;
            return record;
        }
    }
    return NULL;
}

static guest_gl_uint gl_vita_fbo_bound_texture(void)
{
    return s_fbo_active_unit < GL_VITA_TEXTURE_UNITS ?
        s_fbo_bound_2d[s_fbo_active_unit] : 0u;
}

static int gl_vita_fbo_draw_is_scaled(void)
{
    const gl_vita_fbo_record *record = gl_vita_fbo_find(s_draw_framebuffer);

    return record && record->scaled;
}

/* A texture qualifies for the reduced raster only when it is a plain level-0
 * 2D image of at least MIN_EDGE x MIN_EDGE defined without pixel data, which
 * is how KAGE creates render-target surfaces. */
static int gl_vita_fbo_raster_qualifies(
    guest_gl_enum target, guest_gl_int level,
    guest_gl_sizei width, guest_gl_sizei height,
    guest_gl_int border, guest_gl_addr pixels)
{
    return target == GL_VITA_TEXTURE_2D && level == 0 && border == 0 &&
        pixels == 0u &&
        width >= GL_VITA_FBO_RASTER_MIN_EDGE &&
        height >= GL_VITA_FBO_RASTER_MIN_EDGE;
}

static void gl_vita_apply_logical_viewport(void);

/* Propagate a texture's scaled/unscaled state to the framebuffers that
 * attach it, replaying the viewport if the draw target changed raster. */
static void gl_vita_fbo_texture_raster_changed(
    guest_gl_uint texture, uint8_t scaled)
{
    uint32_t index;
    int draw_changed = 0;

    if (!texture)
        return;
    for (index = 0u; index < GL_VITA_FBO_RECORDS; ++index) {
        gl_vita_fbo_record *record = &s_fbo_records[index];

        if (record->color_texture == texture && record->scaled != scaled) {
            record->scaled = scaled;
            if (record->name == s_draw_framebuffer)
                draw_changed = 1;
        }
    }
    if (draw_changed)
        gl_vita_apply_logical_viewport();
}

/* Give a scaled texture its full logical allocation back before the guest
 * writes pixels into it.  vitaGL zero-fills a NULL-defined image
 * (gpu_alloc_texture), so re-specifying one that was never rendered into is
 * lossless; one that already holds rendered layers loses them (the stock
 * upload would have kept them around the sub-rectangle).  The frozen PE is
 * not known to sub-image its 960x540 surfaces; count and log the first
 * occurrence so a device run can prove it. */
static void gl_vita_scaled_forget(guest_gl_uint name, int respecify)
{
    gl_vita_scaled_texture *record = gl_vita_scaled_find(name);

    if (!record)
        return;
    if (respecify) {
        GL_VITA_PHASE_COUNT(fbo_raster_respecified);
        if (!s_fbo_raster_respecify_logged) {
            s_fbo_raster_respecify_logged = 1u;
            isaac_vita_log(
                "[kage-vita] fbo-raster: glTexSubImage2D into scaled "
                "texture %u re-specified at %dx%d",
                (unsigned)name, (int)record->logical_width,
                (int)record->logical_height);
        }
        glTexImage2D(
            (GLenum)GL_VITA_TEXTURE_2D, 0, (GLint)record->internal_format,
            (GLsizei)record->logical_width, (GLsizei)record->logical_height,
            0, (GLenum)record->format, (GLenum)record->type, NULL);
    }
    memset(record, 0, sizeof *record);
    gl_vita_fbo_texture_raster_changed(name, 0u);
}

static void gl_vita_fbo_raster_reset(void)
{
    memset(s_scaled_textures, 0, sizeof s_scaled_textures);
    memset(s_fbo_bound_2d, 0, sizeof s_fbo_bound_2d);
    s_fbo_active_unit = 0u;
    s_fbo_raster_readback_logged = 0u;
    s_fbo_raster_respecify_logged = 0u;
}
#endif

#if defined(ISAAC_VITA_FBO_CLEAR_ELISION)
static void gl_vita_fbo_elision_reset(void)
{
    /* GL defaults: clear colour (0,0,0,0), clear depth 1.0. */
    double depth = 1.0;

    memset(s_fbo_clear_color_bits, 0, sizeof s_fbo_clear_color_bits);
    memcpy(&s_fbo_clear_depth_bits, &depth, sizeof depth);
    s_fbo_elision_poisoned = 0u;
    s_fbo_owed_framebuffer = 0u;
    s_fbo_owed_mask = 0u;
    s_fbo_owed_depth_bits = 0u;
    gl_vita_fbo_color_forget_all();
}

/* One native glClear of |mask| whose depth part lands at exactly
 * |depth_bits| even when the guest has since changed its clear depth (an
 * owed depth was captured under the earlier glClearDepth). */
static void gl_vita_fbo_native_clear(
    guest_gl_bitfield mask, uint64_t depth_bits)
{
    int retarget = (mask & GL_VITA_FBO_DEPTH_BUFFER_BIT) != 0u &&
        depth_bits != s_fbo_clear_depth_bits;
    double depth;

    if (retarget) {
        memcpy(&depth, &depth_bits, sizeof depth);
        glClearDepth((GLdouble)depth);
    }
    GL_VITA_PHASE_COUNT(clear);
    GL_VITA_TIME_BEGIN();
    glClear((GLbitfield)mask);
    GL_VITA_TIME_END(clear);
    if (retarget) {
        memcpy(&depth, &s_fbo_clear_depth_bits, sizeof depth);
        glClearDepth((GLdouble)depth);
    }
}

/* Forget the owed clear.  Exact only where stock vitaGL ends the owing
 * framebuffer's GXM scene (clear or draw on another framebuffer, present,
 * deletion of the framebuffer): the next scene starts at the background
 * depth/stencil no matter what the stock clear quad had written. */
static void gl_vita_fbo_owed_drop(void)
{
    s_fbo_owed_mask = 0u;
    s_fbo_owed_framebuffer = 0u;
}

#if defined(ISAAC_VITA_FBO_CLEAR_ELISION_DEPTH_DROP)
/* Scene-ending drop (ph120.e 'd'): same forget, counted when something was
 * owed.  The sub-mode expands to the plain drop when it is off so the object
 * stays byte-identical. */
# define GL_VITA_FBO_OWED_DROP_SCENE() \
    do { \
        if (s_fbo_owed_mask) \
            GL_VITA_PHASE_COUNT(clear_dropped_scene); \
        gl_vita_fbo_owed_drop(); \
    } while (0)
#else
# define GL_VITA_FBO_OWED_DROP_SCENE() gl_vita_fbo_owed_drop()
#endif

/* Issue the owed clear now.  Exact at any point before a scene-ending event,
 * because stock vitaGL kept that scene open with no other depth/stencil
 * write in between; the caller places it BEFORE the native call whose scene
 * effect it does not model (vitaGL sets dirty_framebuffer on an attachment
 * change, shared.h:1384, so the quad must precede it).  A framebuffer bound
 * away from without any clear or draw still owes: rebind natively for the
 * quad, which vitaGL treats as a pointer store. */
static void gl_vita_fbo_owed_materialize(void)
{
    int rebind;

    if (!s_fbo_owed_mask)
        return;
    rebind = s_fbo_owed_framebuffer != s_draw_framebuffer;
    if (rebind) {
        glBindFramebuffer(
            (GLenum)GL_VITA_DRAW_FRAMEBUFFER, (GLuint)s_fbo_owed_framebuffer);
    }
    gl_vita_fbo_native_clear(s_fbo_owed_mask, s_fbo_owed_depth_bits);
    if (rebind) {
        glBindFramebuffer(
            (GLenum)GL_VITA_DRAW_FRAMEBUFFER, (GLuint)s_draw_framebuffer);
    }
    GL_VITA_PHASE_COUNT(clear_replayed);
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    /* vitaGL ran this quad on the owing framebuffer: the census counts it
     * there (pass touch included; the native rebind is a pointer store for
     * vitaGL and for the census alike). */
    gl_vita_fill_census_clear(s_fbo_owed_framebuffer, s_fbo_owed_mask);
#endif
    gl_vita_fbo_owed_drop();
}

/* A clear or draw on |framebuffer| makes stock vitaGL's scene_reset end the
 * scene of any other framebuffer, taking its depth/stencil with it. */
static void gl_vita_fbo_owed_enter(guest_gl_uint framebuffer)
{
    if (s_fbo_owed_mask && s_fbo_owed_framebuffer != framebuffer)
        GL_VITA_FBO_OWED_DROP_SCENE();
}

/* Guest glClear: nonzero when the request is fully absorbed, i.e. its colour
 * part cannot change a pixel (attached texture known to hold the clear
 * colour) and its depth/stencil part is now owed.  The default framebuffer
 * and unknown, poisoned or invalid requests take the native path. */
static int gl_vita_fbo_clear_defer(guest_gl_bitfield mask)
{
    const gl_vita_fbo_record *record;

    gl_vita_fbo_owed_enter(s_draw_framebuffer);
    if (s_fbo_elision_poisoned || s_draw_framebuffer == 0u)
        return 0;
    record = gl_vita_fbo_find(s_draw_framebuffer);
    if (!record)
        return 0;
    if (mask & ~(GL_VITA_FBO_COLOR_BUFFER_BIT |
                 GL_VITA_FBO_DEPTH_BUFFER_BIT |
                 GL_VITA_FBO_STENCIL_BUFFER_BIT))
        return 0;
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION_DEPTH_DROP)
    /* Depth-only sub-mode: a request with a colour part is always native
     * (the owed bits fold into it); the colour notes are never consulted. */
    if (mask & GL_VITA_FBO_COLOR_BUFFER_BIT)
        return 0;
#else
    if (mask & GL_VITA_FBO_COLOR_BUFFER_BIT) {
        const gl_vita_fbo_color_note *note =
            gl_vita_fbo_color_find(record->color_texture);

        if (!note || memcmp(note->bits, s_fbo_clear_color_bits,
                            sizeof note->bits) != 0)
            return 0;
    }
#endif
    if (mask & GL_VITA_FBO_DEPTH_BUFFER_BIT)
        s_fbo_owed_depth_bits = s_fbo_clear_depth_bits;
    s_fbo_owed_mask |= mask & (GL_VITA_FBO_DEPTH_BUFFER_BIT |
                               GL_VITA_FBO_STENCIL_BUFFER_BIT);
    if (s_fbo_owed_mask)
        s_fbo_owed_framebuffer = s_draw_framebuffer;
    return 1;
}

/* Native path of a guest glClear: fold the owed bits of this very target
 * into the same quad (bits the guest requests again are superseded by its
 * current values) and record what the colour part left behind. */
static void gl_vita_fbo_clear_native(guest_gl_bitfield mask)
{
    gl_vita_fbo_record *record;
    guest_gl_bitfield merged = mask;
    uint64_t depth_bits = s_fbo_clear_depth_bits;

    if (mask & ~(GL_VITA_FBO_COLOR_BUFFER_BIT |
                 GL_VITA_FBO_DEPTH_BUFFER_BIT |
                 GL_VITA_FBO_STENCIL_BUFFER_BIT)) {
        /* vitaGL rejects the request with GL_INVALID_VALUE and clears
         * nothing; settle the owed quad separately, then forward as is. */
        gl_vita_fbo_owed_materialize();
        GL_VITA_PHASE_COUNT(clear);
        GL_VITA_TIME_BEGIN();
        glClear((GLbitfield)mask);
        GL_VITA_TIME_END(clear);
        return;
    }
    if (s_fbo_owed_mask && s_fbo_owed_framebuffer == s_draw_framebuffer) {
        guest_gl_bitfield owed = s_fbo_owed_mask & ~mask;

        merged |= owed;
        if (owed & GL_VITA_FBO_DEPTH_BUFFER_BIT)
            depth_bits = s_fbo_owed_depth_bits;
        gl_vita_fbo_owed_drop();
    }
    gl_vita_fbo_native_clear(merged, depth_bits);
    if (!(mask & GL_VITA_FBO_COLOR_BUFFER_BIT) || s_draw_framebuffer == 0u)
        return;
    record = gl_vita_fbo_find(s_draw_framebuffer);
    if (!record || !record->color_texture)
        return;
    if (s_fbo_elision_poisoned)
        gl_vita_fbo_color_forget(record->color_texture);
    else
        gl_vita_fbo_color_remember(record->color_texture);
}

/* Before the native draw: replay what this target owes (same scene as the
 * draw, exactly what the stock quad would have left), then forget the colour
 * of the texture being drawn into.  An untracked attachment (renderbuffer,
 * non-2D or non-level-0 texture) could alias any note: forget them all. */
static void gl_vita_fbo_note_draw(void)
{
    const gl_vita_fbo_record *record;

    gl_vita_fbo_owed_enter(s_draw_framebuffer);
    gl_vita_fbo_owed_materialize();
    if (s_draw_framebuffer == 0u)
        return;
    record = gl_vita_fbo_find(s_draw_framebuffer);
    if (record && record->color_texture)
        gl_vita_fbo_color_forget(record->color_texture);
    else
        gl_vita_fbo_color_forget_all();
}

/* vglSwapBuffers ends the current scene and forces a fresh one at the next
 * clear or draw (gxm.c:930 needs_scene_reset): an owed depth/stencil clear
 * can no longer reach any draw. */
void gl_vita_backend_fbo_present(void)
{
    GL_VITA_FBO_OWED_DROP_SCENE();
}
#endif

#if defined(GL_VITA_FBO_TABLE)
/* Attachment edges.  Colour knowledge lives with the texture, so an
 * attachment change only retargets the record; the owed clear was settled
 * before the native call (vita_glFramebufferTexture2D).  vitaGL rejects
 * attachments other than COLOR_ATTACHMENT0 for textures and any colour
 * renderbuffer (framebuffers.c), so those leave the record untouched or fail
 * closed to an untracked colour attachment. */
static void gl_vita_fbo_note_texture_attachment(
    guest_gl_enum target, guest_gl_enum attachment,
    guest_gl_enum texture_target, guest_gl_uint texture, guest_gl_int level)
{
    gl_vita_fbo_record *record;
    guest_gl_uint tracked;

    if (target != GL_VITA_FRAMEBUFFER && target != GL_VITA_DRAW_FRAMEBUFFER)
        return;
    record = gl_vita_fbo_find(s_draw_framebuffer);
    if (!record || attachment != GL_VITA_FBO_COLOR_ATTACHMENT0)
        return;
    tracked = (texture_target == GL_VITA_TEXTURE_2D && level == 0) ?
        texture : 0u;
    record->color_texture = tracked;
#if defined(ISAAC_VITA_FBO_RASTER_SCALE)
    {
        uint8_t scaled = tracked && gl_vita_scaled_find(tracked) ? 1u : 0u;

        if (record->scaled != scaled) {
            record->scaled = scaled;
            gl_vita_apply_logical_viewport();
        }
    }
#endif
}

static void gl_vita_fbo_note_renderbuffer_attachment(
    guest_gl_enum target, guest_gl_enum attachment,
    guest_gl_uint renderbuffer)
{
    gl_vita_fbo_record *record;

    if (target != GL_VITA_FRAMEBUFFER && target != GL_VITA_DRAW_FRAMEBUFFER)
        return;
    record = gl_vita_fbo_find(s_draw_framebuffer);
    if (!record)
        return;
    if (attachment == GL_VITA_FBO_COLOR_ATTACHMENT0) {
        record->color_texture = 0u;
#if defined(ISAAC_VITA_FBO_RASTER_SCALE)
        if (record->scaled) {
            record->scaled = 0u;
            gl_vita_apply_logical_viewport();
        }
#endif
        return;
    }
    record->depth_renderbuffer = renderbuffer;
}

/* Names handed out by glGenFramebuffers may recycle a deleted record slot. */
static void gl_vita_fbo_forget_guest_names(
    guest_gl_sizei count, guest_gl_addr names)
{
    guest_gl_sizei index;

    if (!gl_vita_fbo_guest_array_valid(count, names))
        return;
    for (index = 0; index < count; ++index)
        gl_vita_fbo_forget(ld32(names + (uint32_t)index * 4u));
}

static void gl_vita_fbo_shadow_reset(void)
{
    memset(s_fbo_records, 0, sizeof s_fbo_records);
#if defined(ISAAC_VITA_FBO_RASTER_SCALE)
    gl_vita_fbo_raster_reset();
#endif
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION)
    gl_vita_fbo_elision_reset();
#endif
}
#endif

#if defined(ISAAC_VITA_DISPLAY_RASTER_720)
/* Raster geometry: the build passes HEIGHT/NUM/DEN (720x408 at 3/4 or 480x272
 * at 1/2); the bare legacy gate (host oracles) means 720x408.  The default
 * framebuffer's viewport is the logical 960x540 scaled by NUM/DEN and centred
 * vertically, so its Y origin gains (HEIGHT - 540*NUM/DEN)/2 rows: one row at
 * 720x408 and at 480x272. */
# if !defined(ISAAC_VITA_DISPLAY_RASTER_WIDTH)
#  define ISAAC_VITA_DISPLAY_RASTER_WIDTH  720
#  define ISAAC_VITA_DISPLAY_RASTER_HEIGHT 408
#  define ISAAC_VITA_DISPLAY_RASTER_NUM    3
#  define ISAAC_VITA_DISPLAY_RASTER_DEN    4
# endif
# define GL_VITA_DISPLAY_RASTER_VIEWPORT_Y \
    ((ISAAC_VITA_DISPLAY_RASTER_HEIGHT - \
      540 * ISAAC_VITA_DISPLAY_RASTER_NUM / ISAAC_VITA_DISPLAY_RASTER_DEN) / 2)

/* Scale an integer edge by NUM/DEN with mathematical floor semantics.  C's
 * signed division truncates toward zero, so adjust the negative remainder
 * explicitly.  Callers promote the sum of two 32-bit guest values before this
 * function; multiplying that bounded sum by a small NUM cannot overflow
 * int64_t. */
static int64_t gl_vita_scale_edge(int64_t edge)
{
    int64_t numerator = edge * ISAAC_VITA_DISPLAY_RASTER_NUM;
    int64_t scaled = numerator / ISAAC_VITA_DISPLAY_RASTER_DEN;

    if (numerator % ISAAC_VITA_DISPLAY_RASTER_DEN < 0)
        --scaled;
    return scaled;
}
#endif

#if defined(GL_VITA_LOGICAL_VIEWPORT)
static void gl_vita_apply_logical_viewport(void)
{
    /* This is a native replay rather than a guest request.  Leave the guest
     * shadow unknown until its next exact glViewport call. */
    gl_vita_typed_invalidate_viewport();
    if (s_draw_framebuffer == 0u) {
#if defined(ISAAC_VITA_DISPLAY_RASTER_720)
        int64_t left = gl_vita_scale_edge(s_logical_viewport.x);
        int64_t bottom = gl_vita_scale_edge(s_logical_viewport.y);
        int64_t right = gl_vita_scale_edge(
            (int64_t)s_logical_viewport.x + s_logical_viewport.width);
        int64_t top = gl_vita_scale_edge(
            (int64_t)s_logical_viewport.y + s_logical_viewport.height);

        /* Scaling a single 32-bit origin and the difference between two edges
         * remains inside signed 32-bit range.  Add the raster's vertical
         * centre offset only to the default framebuffer's Y origin. */
        glViewport(
            (GLint)left, (GLint)(bottom + GL_VITA_DISPLAY_RASTER_VIEWPORT_Y),
            (GLsizei)(right - left), (GLsizei)(top - bottom));
        return;
#endif
    }
#if defined(ISAAC_VITA_FBO_RASTER_SCALE)
    else if (gl_vita_fbo_draw_is_scaled()) {
        /* The draw target itself lives at NUM/DEN: scale both edges with the
         * same floor so adjacent guest viewports still tile without gaps. */
        int64_t left = gl_vita_fbo_scale_edge(s_logical_viewport.x);
        int64_t bottom = gl_vita_fbo_scale_edge(s_logical_viewport.y);
        int64_t right = gl_vita_fbo_scale_edge(
            (int64_t)s_logical_viewport.x + s_logical_viewport.width);
        int64_t top = gl_vita_fbo_scale_edge(
            (int64_t)s_logical_viewport.y + s_logical_viewport.height);

        GL_VITA_PHASE_COUNT(fbo_raster_viewports);
        glViewport(
            (GLint)left, (GLint)bottom,
            (GLsizei)(right - left), (GLsizei)(top - bottom));
        return;
    }
#endif
    glViewport(
        (GLint)s_logical_viewport.x, (GLint)s_logical_viewport.y,
        (GLsizei)s_logical_viewport.width,
        (GLsizei)s_logical_viewport.height);
}

static void gl_vita_display_raster_reset(void)
{
    s_logical_viewport.x = 0;
    s_logical_viewport.y = 0;
    s_logical_viewport.width = 960;
    s_logical_viewport.height = 540;
}
#endif

#if defined(ISAAC_VITA_FXRAY_ALPHA_MASK)
static gl_vita_fxray_record *gl_vita_fxray_find(guest_gl_uint name)
{
    uint32_t index;

    if (!name)
        return NULL;
    for (index = 0u; index < GL_VITA_FXRAY_RECORDS; ++index)
        if (s_fxray_records[index].name == name)
            return &s_fxray_records[index];
    return NULL;
}

static gl_vita_fxray_record *gl_vita_fxray_empty_record(void)
{
    uint32_t index;

    for (index = 0u; index < GL_VITA_FXRAY_RECORDS; ++index)
        if (!s_fxray_records[index].name)
            return &s_fxray_records[index];
    return NULL;
}

static int gl_vita_fxray_has_live_record(void)
{
    uint32_t index;

    for (index = 0u; index < GL_VITA_FXRAY_RECORDS; ++index)
        if (s_fxray_records[index].name)
            return 1;
    return 0;
}

static void gl_vita_fxray_log_fallback(
    guest_gl_uint name, const char *reason)
{
    if (s_fxray_fallback_receipt)
        return;
    s_fxray_fallback_receipt = 1u;
    isaac_vita_log(
        "KAGE VITA FXRAY ALPHA: phase=fallback reason=%s name=%u "
        "fail_closed=1 end=fxray",
        reason, (unsigned)name);
}

static guest_gl_uint gl_vita_fxray_bound_texture(void)
{
    GLint binding = 0;

    glGetIntegerv(GL_VITA_TEXTURE_BINDING_2D, &binding);
    return binding > 0 ? (guest_gl_uint)(GLuint)binding : 0u;
}

static int gl_vita_fxray_parameter_is_sample_invariant(guest_gl_enum name)
{
    switch (name) {
    case GL_VITA_TEXTURE_MAG_FILTER:
    case GL_VITA_TEXTURE_MIN_FILTER:
    case GL_VITA_TEXTURE_WRAP_S:
    case GL_VITA_TEXTURE_WRAP_T:
        return 1;
    default:
        return 0;
    }
}

static int gl_vita_fxray_attachment_seen(guest_gl_uint name)
{
    uint32_t index;

    if (s_fxray_attachment_registry_saturated)
        return 1;
    for (index = 0u; index < GL_VITA_FXRAY_ATTACHMENT_RECORDS; ++index)
        if (s_fxray_attachments[index] == name)
            return 1;
    return 0;
}

static int gl_vita_fxray_extract_alpha(
    gl_vita_fxray_record *record, guest_gl_addr pixels)
{
    const uint8_t *rgba = (const uint8_t *)(uintptr_t)pixels;
    uint32_t index;

    if (!record || !pixels)
        return 0;
    for (index = 0u; index < GL_VITA_FXRAY_PIXELS; ++index) {
        uint32_t offset = index * 4u;

        if (rgba[offset] != 255u || rgba[offset + 1u] != 255u ||
                rgba[offset + 2u] != 255u)
            return 0;
    }
    /* Validate the complete source before populating the empty shadow.  A
     * near-match therefore remains an exact native RGBA upload. */
    for (index = 0u; index < GL_VITA_FXRAY_PIXELS; ++index) {
        uint32_t offset = index * 4u;

        record->alpha[index] = rgba[offset + 3u];
    }
    return 1;
}

static void gl_vita_fxray_forget(guest_gl_uint name)
{
    gl_vita_fxray_record *record = gl_vita_fxray_find(name);

    if (record)
        record->name = 0u;
}

/* Restore one optimized object without requiring a 512 KiB temporary.  A
 * single 1 KiB row is synchronously consumed by vitaGL 512 times.  This is a
 * deliberately cold fail-safe: the measured ray path never mutates or
 * attaches either image after upload. */
static void gl_vita_fxray_deopt(
    gl_vita_fxray_record *record, const char *reason)
{
    GLint previous = 0;
    uint32_t y;

    if (!record || !record->name)
        return;
    gl_vita_fxray_log_fallback(record->name, reason);
    glGetIntegerv(GL_VITA_TEXTURE_BINDING_2D, &previous);
    if ((guest_gl_uint)(GLuint)previous != record->name)
        glBindTexture(GL_VITA_TEXTURE_2D, (GLuint)record->name);
    glTexImage2D(
        GL_VITA_TEXTURE_2D, 0, GL_VITA_RGBA,
        (GLsizei)GL_VITA_FXRAY_WIDTH, (GLsizei)GL_VITA_FXRAY_HEIGHT,
        0, GL_VITA_RGBA, GL_VITA_UNSIGNED_BYTE, NULL);
    for (y = 0u; y < GL_VITA_FXRAY_HEIGHT; ++y) {
        uint32_t x;

        for (x = 0u; x < GL_VITA_FXRAY_WIDTH; ++x) {
            uint32_t rgba_offset = x * 4u;

            s_fxray_rgba_row[rgba_offset] = 255u;
            s_fxray_rgba_row[rgba_offset + 1u] = 255u;
            s_fxray_rgba_row[rgba_offset + 2u] = 255u;
            s_fxray_rgba_row[rgba_offset + 3u] =
                record->alpha[y * GL_VITA_FXRAY_WIDTH + x];
        }
        glTexSubImage2D(
            GL_VITA_TEXTURE_2D, 0, 0, (GLint)y,
            (GLsizei)GL_VITA_FXRAY_WIDTH, 1,
            GL_VITA_RGBA, GL_VITA_UNSIGNED_BYTE, s_fxray_rgba_row);
    }
    if ((guest_gl_uint)(GLuint)previous != record->name)
        glBindTexture(GL_VITA_TEXTURE_2D, (GLuint)previous);
    record->name = 0u;
}

static void gl_vita_fxray_deopt_all(const char *reason)
{
    uint32_t index;

    for (index = 0u; index < GL_VITA_FXRAY_RECORDS; ++index)
        gl_vita_fxray_deopt(&s_fxray_records[index], reason);
}

static void gl_vita_fxray_note_attachment(guest_gl_uint name)
{
    uint32_t index;
    gl_vita_fxray_record *record;

    if (!name)
        return;
    record = gl_vita_fxray_find(name);
    if (record)
        gl_vita_fxray_deopt(record, "framebuffer-attachment");
    if (gl_vita_fxray_attachment_seen(name))
        return;
    for (index = 0u; index < GL_VITA_FXRAY_ATTACHMENT_RECORDS; ++index) {
        if (!s_fxray_attachments[index]) {
            s_fxray_attachments[index] = name;
            return;
        }
    }
    /* Losing an attachment name could later misclassify a reused texture.
     * Fail closed for the rest of this backend lifetime. */
    gl_vita_fxray_deopt_all("attachment-registry-full");
    gl_vita_fxray_log_fallback(0u, "attachment-registry-full");
    s_fxray_attachment_registry_saturated = 1u;
}

static void gl_vita_fxray_forget_deleted(
    guest_gl_sizei count, guest_gl_addr textures)
{
    guest_gl_sizei index;

    if (count <= 0)
        return;
    if (!textures || (uint32_t)count > GL_VITA_TEXTURE_CAPACITY ||
            textures > UINT32_MAX - (uint32_t)count * 4u) {
        /* The native call still owns validation.  Make every live optimized
         * object ordinary RGBA first if its deletion array is not bounded. */
        gl_vita_fxray_deopt_all("unbounded-delete-array");
        gl_vita_fxray_log_fallback(0u, "unbounded-delete-array");
        s_fxray_attachment_registry_saturated = 1u;
        return;
    }
    for (index = 0; index < count; ++index) {
        guest_gl_uint deleted =
            ld32(textures + (uint32_t)index * 4u);
        uint32_t attachment;

        gl_vita_fxray_forget(deleted);
        for (attachment = 0u;
             attachment < GL_VITA_FXRAY_ATTACHMENT_RECORDS;
             ++attachment) {
            if (s_fxray_attachments[attachment] == deleted)
                s_fxray_attachments[attachment] = 0u;
        }
    }
}

static int gl_vita_fxray_try_upload(
    guest_gl_enum target, guest_gl_int level,
    guest_gl_int internal_format,
    guest_gl_sizei width, guest_gl_sizei height,
    guest_gl_int border, guest_gl_enum format,
    guest_gl_enum type, guest_gl_addr pixels)
{
    guest_gl_uint binding;
    gl_vita_fxray_record *record;

    if (s_fxray_attachment_registry_saturated ||
            target != GL_VITA_TEXTURE_2D || level != 0 ||
            internal_format != GL_VITA_RGBA ||
            width != (guest_gl_sizei)GL_VITA_FXRAY_WIDTH ||
            height != (guest_gl_sizei)GL_VITA_FXRAY_HEIGHT || border != 0 ||
            format != GL_VITA_RGBA || type != GL_VITA_UNSIGNED_BYTE ||
            !pixels)
        return 0;
    binding = gl_vita_fxray_bound_texture();
    if (!binding || gl_vita_fxray_attachment_seen(binding))
        return 0;
    /* Initial immutable uploads are the measured seam.  A second definition
     * of the same object takes the restoration path below instead of risking
     * replacement-failure ambiguity for a live alpha shadow. */
    if (gl_vita_fxray_find(binding))
        return 0;
    record = gl_vita_fxray_empty_record();
    if (!record || !gl_vita_fxray_extract_alpha(record, pixels))
        return 0;

    {
        GL_VITA_TIME_BEGIN();
        glTexImage2D(
            GL_VITA_TEXTURE_2D, 0, GL_VITA_ALPHA,
            (GLsizei)GL_VITA_FXRAY_WIDTH, (GLsizei)GL_VITA_FXRAY_HEIGHT,
            0, GL_VITA_ALPHA, GL_VITA_UNSIGNED_BYTE, record->alpha);
        GL_VITA_TIME_END(tex_upload);
    }
    record->name = binding;
    if (s_fxray_upload_receipts < GL_VITA_FXRAY_RECORDS) {
        uint32_t slot = (uint32_t)(record - s_fxray_records);

        ++s_fxray_upload_receipts;
        isaac_vita_log(
            "KAGE VITA FXRAY ALPHA: phase=upload hit=%u slot=%u name=%u "
            "size=256x512 storage=U8_R111 end=fxray",
            (unsigned)s_fxray_upload_receipts, (unsigned)slot,
            (unsigned)binding);
    }
    return 1;
}

static void gl_vita_fxray_reset(void)
{
    memset(s_fxray_records, 0, sizeof s_fxray_records);
    memset(s_fxray_attachments, 0, sizeof s_fxray_attachments);
    s_fxray_attachment_registry_saturated = 0u;
    /* Receipt counters are deliberately process-lifetime bounded. */
}
#endif

static void vita_glActiveTexture(guest_gl_enum texture)
{
    kage_vita_world_seam_diag_note_active_texture(texture);
#if defined(ISAAC_VITA_FBO_RASTER_SCALE)
    /* vitaGL rejects out-of-range units without changing state; mirror that
     * by marking the unit unknown so no texture qualifies through it. */
    s_fbo_active_unit = (texture >= GL_VITA_TEXTURE0 &&
        texture < GL_VITA_TEXTURE0 + GL_VITA_TEXTURE_UNITS) ?
        texture - GL_VITA_TEXTURE0 : GL_VITA_TEXTURE_UNITS;
#endif
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    gl_vita_texture_profile_note_active(texture);
#endif
#if defined(ISAAC_VITA_PHASE_PROFILE)
    if (texture >= GL_VITA_TEXTURE0 &&
            texture < GL_VITA_TEXTURE0 + GL_VITA_TEXTURE_UNITS)
        s_fusion_profile.active_texture =
            (uint8_t)(texture - GL_VITA_TEXTURE0);
#endif
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    if (texture < GL_VITA_TEXTURE0 ||
            texture >= GL_VITA_TEXTURE0 + GL_VITA_TEXTURE_UNITS) {
        GL_VITA_PHASE_COUNT(typed_state_reject_invalid);
        GL_VITA_TIME_BEGIN();
        glActiveTexture((GLenum)texture);
        GL_VITA_TIME_END(state);
        s_gl_typed_state.active_texture_known = 0u;
        s_gl_typed_state.texture_known = 0u;
        return;
    }
    if (s_gl_typed_state.active_texture_known &&
            s_gl_typed_state.active_texture ==
                (uint8_t)(texture - GL_VITA_TEXTURE0)) {
        GL_VITA_PHASE_COUNT(typed_state_hit_active_texture);
        return;
    }
#endif
    GL_VITA_TIME_BEGIN();
    glActiveTexture((GLenum)texture);
    GL_VITA_TIME_END(state);
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    s_gl_typed_state.active_texture =
        (uint8_t)(texture - GL_VITA_TEXTURE0);
    s_gl_typed_state.active_texture_known = 1u;
    GL_VITA_PHASE_COUNT(typed_state_miss);
#endif
}

static void vita_glAlphaFunc(guest_gl_enum function, guest_gl_float reference)
{
    glAlphaFunc((GLenum)function, (GLfloat)reference);
}

static void vita_glAttachShader(
    guest_gl_uint program, guest_gl_uint shader)
{
    /* Pinned vitaGL rebinds p->vshader/p->fshader here, which is what its
     * glGetAttribLocation/glGetUniformLocation read: every cached location
     * of this generation is stale. */
    gl_vita_location_cache_invalidate();
    glAttachShader((GLuint)program, (GLuint)shader);
}

static void vita_glBindFramebuffer(
    guest_gl_enum target, guest_gl_uint framebuffer)
{
    KAGE_VITA_DEEP_SCOPE(KVD_GL_FBO);
    kage_vita_world_seam_diag_note_bind_framebuffer(target, framebuffer);
#if defined(ISAAC_VITA_PHASE_PROFILE)
    if (target == GL_VITA_FRAMEBUFFER || target == GL_VITA_DRAW_FRAMEBUFFER)
        s_fusion_profile.framebuffer = framebuffer;
#endif
#if defined(GL_VITA_DRAW_FRAMEBUFFER_SHADOW)
    guest_gl_uint previous_draw = s_draw_framebuffer;
    int changes_draw = target == GL_VITA_FRAMEBUFFER ||
        target == GL_VITA_DRAW_FRAMEBUFFER;
#endif
#if defined(ISAAC_VITA_FIRST_FRAME_PROBE)
    if (target == GL_VITA_FF_FRAMEBUFFER ||
            target == GL_VITA_FF_DRAW_FRAMEBUFFER)
        s_first_frame.current_guest_fbo = framebuffer;
    gl_vita_first_frame_increment(
        framebuffer ? &s_first_frame.bind_nonzero :
                      &s_first_frame.bind_zero);
#endif
    GL_VITA_PHASE_COUNT(bind_framebuffer);
    GL_VITA_TIME_BEGIN();
    glBindFramebuffer((GLenum)target, (GLuint)framebuffer);
    GL_VITA_TIME_END(bind_fb);
#if defined(GL_VITA_DRAW_FRAMEBUFFER_SHADOW)
    if (changes_draw) {
        s_draw_framebuffer = framebuffer;
# if defined(GL_VITA_FBO_TABLE)
        (void)gl_vita_fbo_acquire(framebuffer);
# endif
# if defined(GL_VITA_LOGICAL_VIEWPORT)
        if (framebuffer != previous_draw)
            gl_vita_apply_logical_viewport();
# else
        (void)previous_draw;
# endif
    }
#endif
    gl_vita_typed_invalidate_viewport();
}

static void vita_glBindTexture(guest_gl_enum target, guest_gl_uint texture)
{
    kage_vita_world_seam_diag_note_bind_texture(target, texture);
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    gl_vita_texture_profile_note_bind(target, texture);
#endif
#if defined(ISAAC_VITA_PHASE_PROFILE)
    if (target == GL_VITA_TEXTURE_2D &&
            s_fusion_profile.active_texture < GL_VITA_TEXTURE_UNITS)
        s_fusion_profile.textures[s_fusion_profile.active_texture] = texture;
#endif
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    if (target != GL_VITA_TEXTURE_2D) {
        GL_VITA_PHASE_COUNT(typed_state_reject_unknown);
    } else if (texture >= GL_VITA_TEXTURE_CAPACITY) {
        GL_VITA_PHASE_COUNT(typed_state_reject_invalid);
        if (s_gl_typed_state.active_texture_known) {
            s_gl_typed_state.texture_known &= (uint16_t)~(
                UINT16_C(1) << s_gl_typed_state.active_texture);
        } else {
            s_gl_typed_state.texture_known = 0u;
        }
    } else if (!s_gl_typed_state.active_texture_known) {
        GL_VITA_PHASE_COUNT(typed_state_reject_unknown);
        s_gl_typed_state.texture_known = 0u;
    } else {
        uint16_t bit = (uint16_t)(
            UINT16_C(1) << s_gl_typed_state.active_texture);
        if ((s_gl_typed_state.texture_known & bit) &&
                s_gl_typed_state.texture_2d[
                    s_gl_typed_state.active_texture] == texture) {
            GL_VITA_PHASE_COUNT(typed_state_hit_bind_texture);
            return;
        }
    }
#endif
    GL_VITA_PHASE_COUNT(bind_texture);
    GL_VITA_TIME_BEGIN();
    glBindTexture((GLenum)target, (GLuint)texture);
    GL_VITA_TIME_END(state);
#if defined(ISAAC_VITA_FBO_RASTER_SCALE)
    if (target == GL_VITA_TEXTURE_2D &&
            s_fbo_active_unit < GL_VITA_TEXTURE_UNITS)
        s_fbo_bound_2d[s_fbo_active_unit] = texture;
#endif
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    if (target == GL_VITA_TEXTURE_2D &&
            texture < GL_VITA_TEXTURE_CAPACITY &&
            s_gl_typed_state.active_texture_known) {
        uint16_t bit = (uint16_t)(
            UINT16_C(1) << s_gl_typed_state.active_texture);
        s_gl_typed_state.texture_2d[
            s_gl_typed_state.active_texture] = texture;
        s_gl_typed_state.texture_known |= bit;
        GL_VITA_PHASE_COUNT(typed_state_miss);
    }
#endif
}

static void vita_glBlendFuncSeparate(
    guest_gl_enum source_rgb, guest_gl_enum destination_rgb,
    guest_gl_enum source_alpha, guest_gl_enum destination_alpha)
{
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    int valid = gl_vita_typed_valid_blend_factor(source_rgb) &&
        gl_vita_typed_valid_blend_factor(destination_rgb) &&
        gl_vita_typed_valid_blend_factor(source_alpha) &&
        gl_vita_typed_valid_blend_factor(destination_alpha);

    if (!valid) {
        GL_VITA_PHASE_COUNT(typed_state_reject_invalid);
        s_gl_typed_state.blend_known = 0u;
    } else if (s_gl_typed_state.blend_known &&
            s_gl_typed_state.blend[0] == source_rgb &&
            s_gl_typed_state.blend[1] == destination_rgb &&
            s_gl_typed_state.blend[2] == source_alpha &&
            s_gl_typed_state.blend[3] == destination_alpha) {
        GL_VITA_PHASE_COUNT(typed_state_hit_blend);
        return;
    }
#endif
#if defined(ISAAC_VITA_PHASE_PROFILE)
    s_fusion_profile.blend_source_rgb = source_rgb;
    s_fusion_profile.blend_destination_rgb = destination_rgb;
#endif
    GL_VITA_PHASE_COUNT(state);
    GL_VITA_TIME_BEGIN();
    glBlendFuncSeparate(
        (GLenum)source_rgb, (GLenum)destination_rgb,
        (GLenum)source_alpha, (GLenum)destination_alpha);
    GL_VITA_TIME_END(state);
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    if (valid) {
        s_gl_typed_state.blend[0] = source_rgb;
        s_gl_typed_state.blend[1] = destination_rgb;
        s_gl_typed_state.blend[2] = source_alpha;
        s_gl_typed_state.blend[3] = destination_alpha;
        s_gl_typed_state.blend_known = 1u;
        GL_VITA_PHASE_COUNT(typed_state_miss);
    }
#endif
}

static guest_gl_enum vita_glCheckFramebufferStatus(guest_gl_enum target)
{
    return (guest_gl_enum)glCheckFramebufferStatus((GLenum)target);
}

static void vita_glClear(guest_gl_bitfield mask)
{
    KAGE_VITA_DEEP_SCOPE(KVD_GL_CLEAR);
    gl_vita_backend_attrib_sync();
    kage_vita_world_seam_diag_note_clear(mask);
#if defined(ISAAC_VITA_FIRST_FRAME_PROBE)
    gl_vita_first_frame_increment(
        s_first_frame.current_guest_fbo ?
            &s_first_frame.clear_offscreen :
            &s_first_frame.clear_default);
#endif
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION)
    if (gl_vita_fbo_clear_defer(mask)) {
        /* No colour pixel can change and the depth/stencil part is owed to
         * the first draw; not touching vitaGL here also leaves its deferred
         * GXM scene switch unissued. */
        GL_VITA_PHASE_COUNT(clear_suppressed);
        return;
    }
    gl_vita_fbo_clear_native(mask);
#else
    GL_VITA_PHASE_COUNT(clear);
    GL_VITA_TIME_BEGIN();
    glClear((GLbitfield)mask);
    GL_VITA_TIME_END(clear);
#endif
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    /* After the elision decision: only clears vitaGL received are counted. */
    gl_vita_fill_census_clear(s_fusion_profile.framebuffer, mask);
#endif
}

static void vita_glClearColor(
    guest_gl_float red, guest_gl_float green,
    guest_gl_float blue, guest_gl_float alpha)
{
    kage_vita_world_seam_diag_note_clear_color(red, green, blue, alpha);
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION)
    {
        GLfloat native[4];

        native[0] = (GLfloat)red;
        native[1] = (GLfloat)green;
        native[2] = (GLfloat)blue;
        native[3] = (GLfloat)alpha;
        memcpy(s_fbo_clear_color_bits, native, sizeof native);
    }
#endif
    GL_VITA_TIME_BEGIN();
    glClearColor((GLfloat)red, (GLfloat)green,
                 (GLfloat)blue, (GLfloat)alpha);
    GL_VITA_TIME_END(state);
}

static void vita_glClearDepth(guest_gl_double depth)
{
    /* Desktop OpenGL defines this argument as clampd.  vitaGL 73dd57a
     * stores it verbatim, so Isaac's deliberate -1000.0 clear depth would
     * turn into an out-of-clip clear quad instead of the required 0.0. */
    GLdouble clamped_depth = (GLdouble)depth;

    if (clamped_depth < 0.0)
        clamped_depth = 0.0;
    else if (clamped_depth > 1.0)
        clamped_depth = 1.0;
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION)
    memcpy(&s_fbo_clear_depth_bits, &clamped_depth, sizeof clamped_depth);
#endif
    GL_VITA_TIME_BEGIN();
    glClearDepth(clamped_depth);
    GL_VITA_TIME_END(state);
}

static void vita_glCompileShader(guest_gl_uint shader)
{
    /* A recompiled shader replaces the GXP an attached program's location
     * queries read through p->vshader->prog. */
    gl_vita_location_cache_invalidate();
    glCompileShader((GLuint)shader);
}

static guest_gl_uint vita_glCreateProgram(void)
{
    guest_gl_uint program;

    /* Program names are recycled from the lowest free vitaGL slot. */
    gl_vita_location_cache_invalidate();
    program = (guest_gl_uint)glCreateProgram();
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    gl_vita_program_created(program);
#endif
    return program;
}

static guest_gl_uint vita_glCreateShader(guest_gl_enum type)
{
    return (guest_gl_uint)glCreateShader((GLenum)type);
}

static void vita_glCullFace(guest_gl_enum mode)
{
    GL_VITA_PHASE_COUNT(state);
    GL_VITA_TIME_BEGIN();
    glCullFace((GLenum)mode);
    GL_VITA_TIME_END(state);
}

static void vita_glDeleteFramebuffers(
    guest_gl_sizei count, guest_gl_addr framebuffers)
{
#if defined(GL_VITA_DRAW_FRAMEBUFFER_SHADOW) || \
        defined(ISAAC_VITA_PHASE_PROFILE)
# if defined(GL_VITA_DRAW_FRAMEBUFFER_SHADOW)
    int deletes_draw = 0;
# endif
# if defined(ISAAC_VITA_PHASE_PROFILE)
    int deletes_profile = 0;
# endif

    if (count > 0 && framebuffers &&
            (uint32_t)count <= (UINT32_MAX - framebuffers) / 4u) {
        guest_gl_sizei index;

        for (index = 0; index < count; ++index) {
            guest_gl_uint deleted =
                ld32(framebuffers + (uint32_t)index * 4u);
# if defined(GL_VITA_DRAW_FRAMEBUFFER_SHADOW)
            if (deleted ==
                    s_draw_framebuffer) {
                deletes_draw = s_draw_framebuffer != 0u;
            }
# endif
# if defined(GL_VITA_FBO_TABLE)
            gl_vita_fbo_forget(deleted);
# endif
# if defined(ISAAC_VITA_FBO_CLEAR_ELISION)
            if (deleted && deleted == s_fbo_owed_framebuffer)
                GL_VITA_FBO_OWED_DROP_SCENE();
# endif
# if defined(ISAAC_VITA_PHASE_PROFILE)
            if (deleted == s_fusion_profile.framebuffer)
                deletes_profile = s_fusion_profile.framebuffer != 0u;
# endif
        }
    }
#endif
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    gl_vita_fill_note_delete_framebuffers(count, framebuffers);
#endif
    glDeleteFramebuffers(
        (GLsizei)count, (const GLuint *)(uintptr_t)framebuffers);
#if defined(ISAAC_VITA_WORLD_SEAM_DIAG)
    if (count > 0 && framebuffers &&
            (uint32_t)count <= (UINT32_MAX - framebuffers) / 4u) {
        guest_gl_sizei index;
        for (index = 0; index < count; ++index) {
            kage_vita_world_seam_diag_note_delete_framebuffer(
                ld32(framebuffers + (uint32_t)index * 4u));
        }
    }
#endif
#if defined(GL_VITA_DRAW_FRAMEBUFFER_SHADOW)
    if (deletes_draw) {
        s_draw_framebuffer = 0u;
# if defined(GL_VITA_LOGICAL_VIEWPORT)
        gl_vita_apply_logical_viewport();
# endif
    }
#endif
#if defined(ISAAC_VITA_PHASE_PROFILE)
    if (deletes_profile)
        s_fusion_profile.framebuffer = 0u;
#endif
    gl_vita_typed_invalidate_viewport();
}

static void vita_glDeleteProgram(guest_gl_uint program)
{
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    gl_vita_fill_note_program_forget(program);
#endif
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    gl_vita_program_mutated(program, 1);
#endif
    gl_vita_location_cache_invalidate();
    glDeleteProgram((GLuint)program);
}

static void vita_glGenFramebuffers(
    guest_gl_sizei count, guest_gl_addr framebuffers)
{
    glGenFramebuffers((GLsizei)count, (GLuint *)(uintptr_t)framebuffers);
#if defined(GL_VITA_FBO_TABLE)
    gl_vita_fbo_forget_guest_names(count, framebuffers);
#endif
}

static void vita_glBindRenderbuffer(
    guest_gl_enum target, guest_gl_uint renderbuffer)
{
    if (target != GL_VITA_RENDERBUFFER)
        gl_vita_rbo_fault(
            GL_VITA_TOKEN_BIND_RENDERBUFFER,
            "Vita GL glBindRenderbuffer received an invalid target");
    if (renderbuffer && !gl_vita_rbo_find(renderbuffer))
        gl_vita_rbo_fault(
            GL_VITA_TOKEN_BIND_RENDERBUFFER,
            "Vita GL glBindRenderbuffer received an unknown renderbuffer");
    glBindRenderbuffer((GLenum)target, (GLuint)renderbuffer);
    s_bound_renderbuffer = renderbuffer;
}

static void vita_glDeleteRenderbuffers(
    guest_gl_sizei count, guest_gl_addr renderbuffers)
{
    guest_gl_sizei index;

    if (count < 0 || (uint32_t)count > GL_VITA_RBO_CAPACITY ||
            (count > 0 && (!renderbuffers ||
             renderbuffers > UINT32_MAX - (uint32_t)count * 4u)))
        gl_vita_rbo_fault(
            GL_VITA_TOKEN_DELETE_RENDERBUFFERS,
            "Vita GL glDeleteRenderbuffers received an invalid array");
    glDeleteRenderbuffers(
        (GLsizei)count, (const GLuint *)(uintptr_t)renderbuffers);
    for (index = 0; index < count; ++index)
        gl_vita_rbo_remove(ld32(renderbuffers + (uint32_t)index * 4u));
}

static void vita_glDeleteShader(guest_gl_uint shader)
{
    gl_vita_location_cache_invalidate();
    glDeleteShader((GLuint)shader);
}

static void vita_glDeleteTextures(
    guest_gl_sizei count, guest_gl_addr textures)
{
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    uint64_t native_started_at;
    uint64_t native_ended_at;
    uint64_t post_started_at;
#endif
#if defined(ISAAC_VITA_FXRAY_ALPHA_MASK)
    gl_vita_fxray_forget_deleted(count, textures);
#endif
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    /* Do not inspect the guest array for this cache.  vitaGL turns every
     * deleted bound name into zero, so forgetting all 16 bindings is exact,
     * bounded, and hostile-pointer safe at this layer. */
    s_gl_typed_state.texture_known = 0u;
#endif
#if defined(ISAAC_VITA_PHASE_PROFILE)
    if (count > 0 && textures &&
            (uint32_t)count <= (UINT32_MAX - textures) / 4u) {
        guest_gl_sizei index;

        for (index = 0; index < count; ++index) {
            guest_gl_uint deleted = ld32(textures + (uint32_t)index * 4u);
            uint32_t unit;

            for (unit = 0u; unit < GL_VITA_TEXTURE_UNITS; ++unit)
                if (s_fusion_profile.textures[unit] == deleted)
                    s_fusion_profile.textures[unit] = 0u;
        }
    }
#endif
#if defined(GL_VITA_FBO_TABLE)
    /* Deleting any texture may free an attached colour surface: settle the
     * owed clear in the current scene first, then forget every colour note.
     * Scaled records (replaying the viewport if the draw target changes
     * raster) and the unit shadow drop the names. */
# if defined(ISAAC_VITA_FBO_CLEAR_ELISION)
    gl_vita_fbo_owed_materialize();
    gl_vita_fbo_color_forget_all();
# endif
    if (gl_vita_fbo_guest_array_valid(count, textures)) {
        guest_gl_sizei index;

        for (index = 0; index < count; ++index) {
            guest_gl_uint deleted = ld32(textures + (uint32_t)index * 4u);
            uint32_t slot;

# if defined(ISAAC_VITA_FBO_RASTER_SCALE)
            gl_vita_scaled_forget(deleted, 0);
            for (slot = 0u; slot < GL_VITA_TEXTURE_UNITS; ++slot)
                if (s_fbo_bound_2d[slot] == deleted)
                    s_fbo_bound_2d[slot] = 0u;
# endif
            for (slot = 0u; slot < GL_VITA_FBO_RECORDS; ++slot)
                if (deleted && s_fbo_records[slot].color_texture == deleted)
                    s_fbo_records[slot].color_texture = 0u;
        }
    }
#endif
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    /* Keep the native interval independent of the pre-call FXRay/state work. */
    native_started_at = sceKernelGetProcessTimeWide();
#endif
    glDeleteTextures((GLsizei)count,
                     (const GLuint *)(uintptr_t)textures);
#if defined(ISAAC_VITA_WORLD_SEAM_DIAG)
    if (count > 0 && textures &&
            (uint32_t)count <= (UINT32_MAX - textures) / 4u) {
        guest_gl_sizei index;
        for (index = 0; index < count; ++index) {
            kage_vita_world_seam_diag_note_delete_texture(
                ld32(textures + (uint32_t)index * 4u));
        }
    }
#endif
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    native_ended_at = sceKernelGetProcessTimeWide();
    post_started_at = sceKernelGetProcessTimeWide();
    gl_vita_texture_profile_note_delete(count, textures);
    gl_vita_texture_profile_note_delete_timing(
        native_started_at, native_ended_at,
        post_started_at, sceKernelGetProcessTimeWide());
#endif
}

static void vita_glDepthFunc(guest_gl_enum function)
{
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    int valid = gl_vita_typed_valid_depth_function(function);

    if (!valid) {
        GL_VITA_PHASE_COUNT(typed_state_reject_invalid);
        s_gl_typed_state.depth_known = 0u;
    } else if (s_gl_typed_state.depth_known &&
            s_gl_typed_state.depth == function) {
        GL_VITA_PHASE_COUNT(typed_state_hit_depth);
        return;
    }
#endif
    GL_VITA_PHASE_COUNT(state);
    GL_VITA_TIME_BEGIN();
    glDepthFunc((GLenum)function);
    GL_VITA_TIME_END(state);
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    if (valid) {
        s_gl_typed_state.depth = function;
        s_gl_typed_state.depth_known = 1u;
        GL_VITA_PHASE_COUNT(typed_state_miss);
    }
#endif
}

static void vita_glDisableVertexAttribArray(guest_gl_uint index)
{
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    gl_vita_fill_note_attrib_toggle(index, 0);
#endif
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    if (index >= GL_VITA_VERTEX_ATTRIBS) {
        /* NO_DEBUG native invalid shifts can still mutate the mask. Preserve
         * all earlier valid operations before forwarding that exact call. */
        gl_vita_backend_attrib_sync();
        GL_VITA_PHASE_COUNT(typed_state_reject_invalid);
        s_gl_typed_state.attrib_enable_known = 0u;
    } else {
        uint16_t bit = (uint16_t)(UINT16_C(1) << index);
        if ((s_gl_typed_state.attrib_enable_known & bit) &&
                !(s_gl_typed_state.attrib_enabled & bit)) {
            GL_VITA_PHASE_COUNT(typed_state_hit_attrib_toggle);
            return;
        }
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
        if (s_gl_typed_state.attrib_enable_known & bit) {
            gl_vita_attrib_defer(bit);
            return;
        }
#endif
    }
#endif
    GL_VITA_PHASE_COUNT(attrib_toggle);
    GL_VITA_TIME_BEGIN();
    glDisableVertexAttribArray((GLuint)index);
    GL_VITA_TIME_END(state);
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    if (index < GL_VITA_VERTEX_ATTRIBS) {
        uint16_t bit = (uint16_t)(UINT16_C(1) << index);
        s_gl_typed_state.attrib_enabled &= (uint16_t)~bit;
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
        s_gl_typed_state.attrib_applied &= (uint16_t)~bit;
#endif
        s_gl_typed_state.attrib_enable_known |= bit;
        GL_VITA_PHASE_COUNT(typed_state_miss);
    }
#endif
}

static void vita_glDrawElements(
    guest_gl_enum mode, guest_gl_sizei count,
    guest_gl_enum type, guest_gl_addr indices)
{
    KAGE_VITA_DEEP_SCOPE(KVD_GL_DRAW);
    GL_VITA_DRAW_SPLIT_ENTER();
    gl_vita_backend_attrib_sync();
    GL_VITA_DRAW_SPLIT_MARK(SYNC);
    kage_vita_world_seam_diag_note_draw();
#if defined(ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY)
    KageVitaCanonicalQuadResult canonical =
        kage_vita_canonical_quad_classify(
            guest_gl_backend_return_rva(), mode, count, type, indices);
#endif
#if defined(ISAAC_VITA_FIRST_FRAME_PROBE)
    gl_vita_first_frame_increment(
        s_first_frame.current_guest_fbo ?
            &s_first_frame.draw_offscreen :
            &s_first_frame.draw_default);
    s_first_frame.last_draw_fbo = s_first_frame.current_guest_fbo;
#endif
    GL_VITA_DRAW_SPLIT_MARK(CLASSIFY);
    GL_VITA_PHASE_COUNT(draw_elements);
    gl_vita_fusion_profile_draw(count, type);
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    gl_vita_fill_census_draw(
        mode, count, type, indices,
# if defined(ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY)
        canonical == KAGE_VITA_CANONICAL_QUAD_OK);
# else
        0);
# endif
#endif
    GL_VITA_DRAW_SPLIT_MARK(CENSUS);
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION)
    gl_vita_fbo_note_draw();
#endif
#if defined(ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY)
    switch (canonical) {
    case KAGE_VITA_CANONICAL_QUAD_OK: {
        int drawn;

        GL_VITA_TIME_BEGIN();
        GL_VITA_DRAW_SPLIT_AT(FBO, gl_vita_time_started_at);
        drawn = vglIsaacDrawCanonicalQuads((GLsizei)count);
        GL_VITA_TIME_END(draw);
        GL_VITA_DRAW_SPLIT_BODY_DONE();
        if (drawn) {
            GL_VITA_DRAW_SPLIT_CANONICAL();
            GL_VITA_PHASE_COUNT(canonical_quad_hits);
            GL_VITA_PHASE_ADD(
                canonical_quad_index_bytes_saved, (uint32_t)count * 2u);
            return;
        }
        GL_VITA_PHASE_COUNT(canonical_quad_driver_fallback);
        break;
    }
    case KAGE_VITA_CANONICAL_QUAD_REJECT_CALLSITE:
        GL_VITA_PHASE_COUNT(canonical_quad_reject_callsite);
        break;
    case KAGE_VITA_CANONICAL_QUAD_REJECT_SHAPE:
        GL_VITA_PHASE_COUNT(canonical_quad_reject_shape);
        break;
    case KAGE_VITA_CANONICAL_QUAD_REJECT_BOUNDS:
        GL_VITA_PHASE_COUNT(canonical_quad_reject_bounds);
        break;
    case KAGE_VITA_CANONICAL_QUAD_REJECT_POINTER:
        GL_VITA_PHASE_COUNT(canonical_quad_reject_pointer);
        break;
    }
#endif
    GL_VITA_TIME_BEGIN();
    GL_VITA_DRAW_SPLIT_AT(FBO, gl_vita_time_started_at);
    glDrawElements((GLenum)mode, (GLsizei)count, (GLenum)type,
                   (const void *)(uintptr_t)indices);
    GL_VITA_TIME_END(draw);
    GL_VITA_DRAW_SPLIT_BODY_DONE();
}

static void vita_glEnable(guest_gl_enum capability)
{
#if defined(ISAAC_VITA_PHASE_PROFILE)
    if (capability == GL_VITA_BLEND)
        s_fusion_profile.blend_enabled = 1u;
#endif
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION)
    /* The guest surface has no glScissor/glDisable, so a scissor enable would
     * clip every later clear to an unknown box: fail closed for good. */
    if (capability == GL_VITA_FBO_SCISSOR_TEST && !s_fbo_elision_poisoned) {
        gl_vita_fbo_owed_materialize();
        s_fbo_elision_poisoned = 1u;
        gl_vita_fbo_color_forget_all();
        GL_VITA_PHASE_COUNT(fbo_elision_poison);
    }
#endif
    GL_VITA_TIME_BEGIN();
    glEnable((GLenum)capability);
    GL_VITA_TIME_END(state);
}

static void vita_glEnableVertexAttribArray(guest_gl_uint index)
{
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    gl_vita_fill_note_attrib_toggle(index, 1);
#endif
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    if (index >= GL_VITA_VERTEX_ATTRIBS) {
        gl_vita_backend_attrib_sync();
        GL_VITA_PHASE_COUNT(typed_state_reject_invalid);
        s_gl_typed_state.attrib_enable_known = 0u;
    } else {
        uint16_t bit = (uint16_t)(UINT16_C(1) << index);
        if ((s_gl_typed_state.attrib_enable_known & bit) &&
                (s_gl_typed_state.attrib_enabled & bit)) {
            GL_VITA_PHASE_COUNT(typed_state_hit_attrib_toggle);
            return;
        }
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
        if (s_gl_typed_state.attrib_enable_known & bit) {
            gl_vita_attrib_defer(bit);
            return;
        }
#endif
    }
#endif
    GL_VITA_PHASE_COUNT(attrib_toggle);
    GL_VITA_TIME_BEGIN();
    glEnableVertexAttribArray((GLuint)index);
    GL_VITA_TIME_END(state);
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    if (index < GL_VITA_VERTEX_ATTRIBS) {
        uint16_t bit = (uint16_t)(UINT16_C(1) << index);
        s_gl_typed_state.attrib_enabled |= bit;
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
        s_gl_typed_state.attrib_applied |= bit;
#endif
        s_gl_typed_state.attrib_enable_known |= bit;
        GL_VITA_PHASE_COUNT(typed_state_miss);
    }
#endif
}

static void vita_glGenRenderbuffers(
    guest_gl_sizei count, guest_gl_addr renderbuffers)
{
    guest_gl_sizei index;

    if (count < 0 || (size_t)count > gl_vita_rbo_free_count() ||
            (count > 0 && (!renderbuffers ||
             renderbuffers > UINT32_MAX - (uint32_t)count * 4u)))
        gl_vita_rbo_fault(
            GL_VITA_TOKEN_GEN_RENDERBUFFERS,
            "Vita GL glGenRenderbuffers received an invalid output array");
    glGenRenderbuffers((GLsizei)count, (GLuint *)(uintptr_t)renderbuffers);
    for (index = 0; index < count; ++index) {
        guest_gl_uint name = ld32(
            renderbuffers + (uint32_t)index * 4u);
        guest_gl_sizei prior;
        int duplicate = 0;

        for (prior = 0; prior < index; ++prior) {
            if (ld32(renderbuffers + (uint32_t)prior * 4u) == name) {
                duplicate = 1;
                break;
            }
        }
        if (!name || duplicate || gl_vita_rbo_find(name)) {
            glDeleteRenderbuffers(
                (GLsizei)count,
                (const GLuint *)(uintptr_t)renderbuffers);
            gl_vita_rbo_fault(
                GL_VITA_TOKEN_GEN_RENDERBUFFERS,
                "Vita GL glGenRenderbuffers returned an invalid name");
        }
    }
    for (index = 0; index < count; ++index) {
        guest_gl_uint name = ld32(
            renderbuffers + (uint32_t)index * 4u);
        if (!gl_vita_rbo_add(name)) {
            guest_gl_sizei prior;

            for (prior = 0; prior < index; ++prior) {
                gl_vita_rbo_remove(ld32(
                    renderbuffers + (uint32_t)prior * 4u));
            }
            glDeleteRenderbuffers(
                (GLsizei)count,
                (const GLuint *)(uintptr_t)renderbuffers);
            gl_vita_rbo_fault(
                GL_VITA_TOKEN_GEN_RENDERBUFFERS,
                "Vita GL renderbuffer registry exhausted");
        }
    }
}

static void vita_glFramebufferTexture2D(
    guest_gl_enum target, guest_gl_enum attachment,
    guest_gl_enum texture_target, guest_gl_uint texture,
    guest_gl_int level)
{
    kage_vita_world_seam_diag_note_framebuffer_texture(
        target, attachment, texture_target, texture, level);
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    gl_vita_fill_note_framebuffer_texture(target, attachment, texture);
#endif
#if defined(ISAAC_VITA_FXRAY_ALPHA_MASK)
    /* An alpha-only mask is sampling-equivalent but is not promised to be a
     * color-renderable vitaGL target.  Restore it before any attachment edge,
     * and permanently exclude names seen on this API. */
    gl_vita_fxray_note_attachment(texture);
#endif
#if defined(ISAAC_VITA_FIRST_FRAME_PROBE)
    if ((target == GL_VITA_FF_FRAMEBUFFER ||
            target == GL_VITA_FF_DRAW_FRAMEBUFFER) &&
            attachment == GL_VITA_FF_COLOR_ATTACHMENT0 &&
            s_first_frame.current_guest_fbo != 0u &&
            s_first_frame.current_guest_fbo == s_first_frame.manager_fbo) {
        s_first_frame.manager_color_attach_arg = texture;
        gl_vita_first_frame_increment(
            &s_first_frame.manager_color_attach_calls);
    }
#endif
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION_DEPTH_DROP)
    /* vitaGL flags the in-use framebuffer dirty on a colour attachment of a
     * non-zero texture (shared.h _glFramebufferTexture2D: the tex_id == 0
     * detach returns before dirty_framebuffer), so the next clear or draw on
     * it ends the owing scene, and FBO depth/stencil is neither loaded nor
     * stored across scenes (gxm.c init_depth_stencil_buffer, no
     * STORE_DEPTH_STENCIL): an owed depth/stencil clear of the framebuffer
     * being re-attached here can reach no draw and is dropped exactly, one
     * full-surface depth-only quad cheaper than replaying it.  Only the
     * tracked shape (COLOR_ATTACHMENT0, GL_TEXTURE_2D level 0, a tabled
     * framebuffer, the guest's current draw framebuffer) qualifies; a
     * detach, another framebuffer's debt, an untracked attachment or an
     * untabled name replays as before.  The exactness argument rests on the
     * dirty flag alone (vitaGL sets it for any non-zero texture, whatever
     * was attached before); the table lookup is a sanity gate that keeps
     * the drop on names the FBO table has seen, not part of that argument.
     * In the production archive (NO_DEBUG=1 => SKIP_ERROR_HANDLING) vitaGL's
     * glFramebufferTexture2D ignores textarget and never reads level, so the
     * GL_TEXTURE_2D / level 0 legs only make the drop more conservative: a
     * level-1 or cube attach is replayed, which is correct, not a missed
     * drop. */
    if (s_fbo_owed_mask && s_fbo_owed_framebuffer == s_draw_framebuffer &&
            (target == GL_VITA_FRAMEBUFFER ||
             target == GL_VITA_DRAW_FRAMEBUFFER) &&
            attachment == GL_VITA_FBO_COLOR_ATTACHMENT0 &&
            texture != 0u && texture_target == GL_VITA_TEXTURE_2D &&
            level == 0 && gl_vita_fbo_find(s_draw_framebuffer) != NULL) {
        GL_VITA_PHASE_COUNT(clear_dropped_attach);
        gl_vita_fbo_owed_drop();
    } else {
        gl_vita_fbo_owed_materialize();
    }
#elif defined(ISAAC_VITA_FBO_CLEAR_ELISION)
    /* vitaGL flags the in-use framebuffer dirty on a colour attachment
     * change (shared.h:1384) so the next clear or draw opens a new scene:
     * anything still owed must land in the current one first. */
    gl_vita_fbo_owed_materialize();
#endif
    GL_VITA_TIME_BEGIN();
    glFramebufferTexture2D(
        (GLenum)target, (GLenum)attachment, (GLenum)texture_target,
        (GLuint)texture, (GLint)level);
    GL_VITA_TIME_END(bind_fb);
#if defined(GL_VITA_FBO_TABLE)
    gl_vita_fbo_note_texture_attachment(
        target, attachment, texture_target, texture, level);
#endif
}

static void vita_glGenTextures(
    guest_gl_sizei count, guest_gl_addr textures)
{
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    int value_before_valid = count == 1 && textures &&
        textures <= UINT32_MAX - 3u;
    uint32_t value_before = value_before_valid ? ld32(textures) : 0u;
    uint64_t started_at = sceKernelGetProcessTimeWide();
#endif
    glGenTextures((GLsizei)count, (GLuint *)(uintptr_t)textures);
#if defined(ISAAC_VITA_WORLD_SEAM_DIAG)
    if (count > 0 && textures &&
            (uint32_t)count <= (UINT32_MAX - textures) / 4u) {
        guest_gl_sizei index;
        for (index = 0; index < count; ++index) {
            kage_vita_world_seam_diag_note_gen_texture(
                ld32(textures + (uint32_t)index * 4u));
        }
    }
#endif
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    gl_vita_texture_profile_note_gen(
        count, textures, value_before, value_before_valid,
        started_at, sceKernelGetProcessTimeWide());
#endif
}

static guest_gl_int vita_glGetAttribLocation(
    guest_gl_uint program, guest_gl_addr name)
{
    guest_gl_int location;
    gl_vita_location_cache_key key;

    GL_VITA_PHASE_COUNT(attrib_location);
    if (gl_vita_location_cache_lookup(
            GL_VITA_LOCATION_KIND_ATTRIB, program, name, &key, &location)) {
        GL_VITA_PHASE_COUNT(location_cache_hit);
#if defined(ISAAC_VITA_GL_LOCATION_CACHE_VERIFY)
        /* Device A/B variant: every hit also asks vitaGL and counts a
         * disagreement into ph120.a loc(...,m); the cached answer is still
         * what the guest receives so both variants render identically. */
        if (location != (guest_gl_int)glGetAttribLocation(
                (GLuint)program, (const GLchar *)(uintptr_t)name))
            GL_VITA_PHASE_COUNT(location_cache_mismatch);
#endif
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
        gl_vita_fill_note_attrib_location(program, name, location);
#endif
        return location;
    }
    {
        GL_VITA_TIME_BEGIN();
        location = (guest_gl_int)glGetAttribLocation(
            (GLuint)program, (const GLchar *)(uintptr_t)name);
        GL_VITA_TIME_END(state);
    }
    gl_vita_location_cache_store(&key, location);
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    gl_vita_fill_note_attrib_location(program, name, location);
#endif
    return location;
}

static void vita_glGetIntegerv(guest_gl_enum name, guest_gl_addr value)
{
    gl_vita_backend_attrib_sync();
#if defined(GL_VITA_LOGICAL_VIEWPORT)
    if (name == GL_VITA_VIEWPORT) {
        st32(value + 0u, (uint32_t)s_logical_viewport.x);
        st32(value + 4u, (uint32_t)s_logical_viewport.y);
        st32(value + 8u, (uint32_t)s_logical_viewport.width);
        st32(value + 12u, (uint32_t)s_logical_viewport.height);
        return;
    }
#endif
    glGetIntegerv((GLenum)name, (GLint *)(uintptr_t)value);
}

static void vita_glGetProgramInfoLog(
    guest_gl_uint program, guest_gl_sizei buffer_size,
    guest_gl_addr length, guest_gl_addr info_log)
{
    glGetProgramInfoLog(
        (GLuint)program, (GLsizei)buffer_size,
        (GLsizei *)(uintptr_t)length, (GLchar *)(uintptr_t)info_log);
}

static void vita_glGetProgramiv(
    guest_gl_uint program, guest_gl_enum name, guest_gl_addr parameters)
{
    glGetProgramiv((GLuint)program, (GLenum)name,
                   (GLint *)(uintptr_t)parameters);
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    gl_vita_program_link_status(program, name);
#endif
}

static void vita_glGetRenderbufferParameteriv(
    guest_gl_enum target, guest_gl_enum name, guest_gl_addr parameters)
{
    gl_vita_renderbuffer_record *record;
    guest_gl_int value;

    if (target != GL_VITA_RENDERBUFFER)
        gl_vita_rbo_fault(
            GL_VITA_TOKEN_GET_RBO_PARAMETER,
            "Vita GL glGetRenderbufferParameteriv received an invalid target");
    if (name != GL_VITA_RENDERBUFFER_WIDTH &&
            name != GL_VITA_RENDERBUFFER_HEIGHT)
        gl_vita_rbo_fault(
            GL_VITA_TOKEN_GET_RBO_PARAMETER,
            "Vita GL glGetRenderbufferParameteriv received an unsupported pname");
    if (!parameters)
        gl_vita_rbo_fault(
            GL_VITA_TOKEN_GET_RBO_PARAMETER,
            "Vita GL glGetRenderbufferParameteriv received a null output");
    if (!s_bound_renderbuffer)
        gl_vita_rbo_fault(
            GL_VITA_TOKEN_GET_RBO_PARAMETER,
            "Vita GL glGetRenderbufferParameteriv has no bound renderbuffer");
    record = gl_vita_rbo_find(s_bound_renderbuffer);
    if (!record)
        gl_vita_rbo_fault(
            GL_VITA_TOKEN_GET_RBO_PARAMETER,
            "Vita GL glGetRenderbufferParameteriv found an unknown bound renderbuffer");
    value = name == GL_VITA_RENDERBUFFER_WIDTH
        ? record->width : record->height;
    st32(parameters, (uint32_t)value);
}

static void vita_glGetShaderInfoLog(
    guest_gl_uint shader, guest_gl_sizei buffer_size,
    guest_gl_addr length, guest_gl_addr info_log)
{
    glGetShaderInfoLog(
        (GLuint)shader, (GLsizei)buffer_size,
        (GLsizei *)(uintptr_t)length, (GLchar *)(uintptr_t)info_log);
}

static void vita_glGetShaderiv(
    guest_gl_uint shader, guest_gl_enum name, guest_gl_addr parameters)
{
    glGetShaderiv((GLuint)shader, (GLenum)name,
                  (GLint *)(uintptr_t)parameters);
}

static guest_gl_addr vita_glGetString(guest_gl_enum name)
{
    return (guest_gl_addr)(uintptr_t)glGetString((GLenum)name);
}

static guest_gl_addr vita_glGetStringi(
    guest_gl_enum name, guest_gl_uint index)
{
    return (guest_gl_addr)(uintptr_t)
        glGetStringi((GLenum)name, (GLuint)index);
}

static guest_gl_int vita_glGetUniformLocation(
    guest_gl_uint program, guest_gl_addr name)
{
    guest_gl_int location;
    gl_vita_location_cache_key key;

    GL_VITA_PHASE_COUNT(uniform_location);
    if (gl_vita_location_cache_lookup(
            GL_VITA_LOCATION_KIND_UNIFORM, program, name, &key,
            &location)) {
        GL_VITA_PHASE_COUNT(location_cache_hit);
#if defined(ISAAC_VITA_GL_LOCATION_CACHE_VERIFY)
        if (location != (guest_gl_int)glGetUniformLocation(
                (GLuint)program, (const GLchar *)(uintptr_t)name))
            GL_VITA_PHASE_COUNT(location_cache_mismatch);
#endif
    } else {
        {
            GL_VITA_TIME_BEGIN();
            location = (guest_gl_int)glGetUniformLocation(
                (GLuint)program, (const GLchar *)(uintptr_t)name);
            GL_VITA_TIME_END(state);
        }
        gl_vita_location_cache_store(&key, location);
    }
    /* Observed on hits too: the redundancy cache's direct-mapped location
     * slot evolves exactly as it did when every request reached vitaGL. */
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    gl_vita_location_observed(program, location);
#endif
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    gl_vita_fill_note_uniform_location(program, name, location);
#endif
    return location;
}

#if defined(ISAAC_VITA_COLOROFFSET_FS_PROBE) && \
    !defined(ISAAC_GL_VITA_BACKEND_ORACLE) && \
    !defined(ISAAC_GL_VITA_FIRST_FRAME_ORACLE)
void isaac_vita_log(const char *format, ...);
/* DIAGNOSTIC: one receipt per exact ColorOffset program link, right after
 * glLinkProgram returned on the context-owning thread, so a session proves
 * which fragment shader the probe registered for which GL program (or why it
 * failed closed to the stock one) before the first ph120.kp window.  Links of
 * every other program leave link_attempts unchanged and print nothing. */
static void gl_vita_backend_coloroffset_fs_probe_link_receipt(
    guest_gl_uint program)
{
    static uint32_t reported_attempts;
    vglIsaacColorOffsetFsProbeStats stats;

    memset(&stats, 0, sizeof stats);
    vglGetIsaacColorOffsetFsProbeStats(&stats);
    if (stats.link_attempts == reported_attempts)
        return;
    reported_attempts = stats.link_attempts;
    isaac_vita_log(
        "KAGE VITA COLOROFFSET FS PROBE LINK: prog=%u mode=%u "
        "link(a,r)=%u,%u lfail(s,c,x,k,r)=%u,%u,%u,%u,%u "
        "gxp=%u/%08x src=%u/%08x",
        (unsigned)program, (unsigned)stats.mode,
        (unsigned)stats.link_attempts, (unsigned)stats.link_ready,
        (unsigned)stats.fail_source, (unsigned)stats.fail_compiler,
        (unsigned)stats.fail_compile, (unsigned)stats.fail_check,
        (unsigned)stats.fail_register, (unsigned)stats.gxp_size,
        (unsigned)stats.gxp_fnv1a, (unsigned)stats.source_size,
        (unsigned)stats.source_fnv1a);
}
#endif

static void vita_glLinkProgram(guest_gl_uint program)
{
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    /* Locations may move across a relink; the guest re-queries them. */
    gl_vita_fill_note_program_forget(program);
#endif
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    gl_vita_program_mutated(program, 0);
#endif
    gl_vita_location_cache_invalidate();
#if !defined(ISAAC_GL_VITA_BACKEND_ORACLE) && \
    !defined(ISAAC_GL_VITA_FIRST_FRAME_ORACLE)
    /* This is the context-owning thread and runs immediately before a
     * potentially long native compile or persistent-cache lookup. */
    kage_vita_loading_note_shader();
#endif
    glLinkProgram((GLuint)program);
#if defined(ISAAC_VITA_COLOROFFSET_FS_PROBE) && \
    !defined(ISAAC_GL_VITA_BACKEND_ORACLE) && \
    !defined(ISAAC_GL_VITA_FIRST_FRAME_ORACLE)
    gl_vita_backend_coloroffset_fs_probe_link_receipt(program);
#endif
}

#if defined(ISAAC_VITA_VITAGL_SHADER_CACHE)
void gl_vita_backend_shader_cache_report(void)
{
    static int reported;
    vglIsaacShaderCacheStats stats;

    /* This is a cold-start milestone, not a per-shader logger.  The native
     * Vita logger opens the diagnostic file for each record, so report the
     * complete startup aggregate exactly once at the first game present. */
    if (reported)
        return;
    reported = 1;
    memset(&stats, 0, sizeof stats);
    vglGetIsaacShaderCacheStats(&stats);
    isaac_vita_log(
        "KAGE VITA SHADER CACHE: serial=%llu attempts=%llu hits=%llu "
        "misses=%llu corrupt=%llu native=%llu register_fail=%llu "
        "write=%llu/%llu",
        (unsigned long long)stats.serial,
        (unsigned long long)stats.attempts,
        (unsigned long long)stats.hits,
        (unsigned long long)stats.misses,
        (unsigned long long)stats.corrupt,
        (unsigned long long)stats.native_compiles,
        (unsigned long long)stats.register_failures,
        (unsigned long long)stats.write_ok,
        (unsigned long long)stats.write_failures);
    isaac_vita_log(
        "KAGE VITA SHADER CACHE IO: read=%lluB write=%lluB "
        "read=%lluus hash=%lluus register=%lluus compile=%lluus "
        "write=%lluus read_fail=%llu alloc_fail=%llu oversize=%llu",
        (unsigned long long)stats.bytes_read,
        (unsigned long long)stats.bytes_written,
        (unsigned long long)stats.read_us,
        (unsigned long long)stats.hash_us,
        (unsigned long long)stats.register_us,
        (unsigned long long)stats.compile_us,
        (unsigned long long)stats.write_us,
        (unsigned long long)stats.read_failures,
        (unsigned long long)stats.alloc_failures,
        (unsigned long long)stats.oversize);
    isaac_vita_log(
        "KAGE VITA SHADER CACHE PUBLISH: attempts=%llu block_list=%llu "
        "ready=%llu block_records=%llu shape=%llu semantics=%llu "
        "matrices=%llu blocks=%llu incomplete=%llu alloc=%llu",
        (unsigned long long)stats.publish_attempts,
        (unsigned long long)stats.publish_with_block_list,
        (unsigned long long)stats.publish_ready,
        (unsigned long long)stats.publish_block_records,
        (unsigned long long)stats.publish_reject_shape,
        (unsigned long long)stats.publish_reject_semantics,
        (unsigned long long)stats.publish_reject_matrices,
        (unsigned long long)stats.publish_reject_blocks,
        (unsigned long long)stats.publish_reject_incomplete,
        (unsigned long long)stats.publish_reject_alloc);
}
#endif

static void vita_glReadPixels(
    guest_gl_int x, guest_gl_int y,
    guest_gl_sizei width, guest_gl_sizei height,
    guest_gl_enum format, guest_gl_enum type,
    guest_gl_addr pixels)
{
    gl_vita_backend_attrib_sync();
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    gl_vita_fill_note_read_pixels();
#endif
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION)
    /* vitaGL may end and finish the in-use scene here (framebuffers.c:674);
     * the owed quad belongs to that scene. */
    gl_vita_fbo_owed_materialize();
#endif
#if defined(ISAAC_VITA_FBO_RASTER_SCALE)
    /* Pixel-exact readback of a scaled target is not modelled; the frozen PE
     * only reads the default framebuffer.  Log the first occurrence. */
    if (gl_vita_fbo_draw_is_scaled() && !s_fbo_raster_readback_logged) {
        s_fbo_raster_readback_logged = 1u;
        isaac_vita_log(
            "[kage-vita] fbo-raster: glReadPixels on scaled target %u",
            (unsigned)s_draw_framebuffer);
    }
#endif
    glReadPixels((GLint)x, (GLint)y, (GLsizei)width, (GLsizei)height,
                 (GLenum)format, (GLenum)type,
                 (void *)(uintptr_t)pixels);
}

static void vita_glRenderbufferStorage(
    guest_gl_enum target, guest_gl_enum format,
    guest_gl_sizei width, guest_gl_sizei height)
{
    gl_vita_renderbuffer_record *record;

    if (target != GL_VITA_RENDERBUFFER || width < 0 || height < 0)
        gl_vita_rbo_fault(
            GL_VITA_TOKEN_RBO_STORAGE,
            "Vita GL glRenderbufferStorage received invalid dimensions or target");
    if (!s_bound_renderbuffer ||
            !(record = gl_vita_rbo_find(s_bound_renderbuffer)))
        gl_vita_rbo_fault(
            GL_VITA_TOKEN_RBO_STORAGE,
            "Vita GL glRenderbufferStorage has no tracked bound renderbuffer");
    glRenderbufferStorage(
        (GLenum)target, (GLenum)format, (GLsizei)width, (GLsizei)height);
    record->internal_format = format;
    record->width = width;
    record->height = height;
}

static void vita_glFramebufferRenderbuffer(
    guest_gl_enum target, guest_gl_enum attachment,
    guest_gl_enum renderbuffer_target, guest_gl_uint renderbuffer)
{
    glFramebufferRenderbuffer(
        (GLenum)target, (GLenum)attachment,
        (GLenum)renderbuffer_target, (GLuint)renderbuffer);
#if defined(GL_VITA_FBO_TABLE)
    gl_vita_fbo_note_renderbuffer_attachment(target, attachment, renderbuffer);
#endif
}

static void vita_glShaderSource(
    guest_gl_uint shader, guest_gl_sizei count,
    guest_gl_addr strings, guest_gl_addr lengths)
{
    gl_vita_location_cache_invalidate();
#if defined(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
    IsaacColorOffsetSourceSignature signature =
        isaac_coloroffset_production_signature();
    IsaacColorOffsetSourceMatch match;
    GLint shader_type = 0;
    int selected;

    glGetShaderiv((GLuint)shader, GL_SHADER_TYPE, &shader_type);
    selected = isaac_coloroffset_match_source(
        (uint32_t)shader_type, (int32_t)count,
        (const char *const *)(uintptr_t)strings,
        (const int32_t *)(uintptr_t)lengths, &signature, &match);
    /* Source selection is observational.  Keep the original count, lengths,
     * partition and bytes on the only glShaderSource call. */
    glShaderSource(
        (GLuint)shader, (GLsizei)count,
        (const GLchar *const *)(uintptr_t)strings,
        (const GLint *)(uintptr_t)lengths);
    if (selected) {
        /* vitaGL recomputes both signatures from its owned concatenation
         * before it records the exact stock shader generation. */
        vglIsaacMarkColorOffsetShader(
            (GLuint)shader, match.source_fnv1a,
            match.source_first512_fnv1a, match.source_size);
    }
#else
    glShaderSource(
        (GLuint)shader, (GLsizei)count,
        (const GLchar *const *)(uintptr_t)strings,
        (const GLint *)(uintptr_t)lengths);
#endif
}

static void vita_glTexImage2D(
    guest_gl_enum target, guest_gl_int level,
    guest_gl_int internal_format,
    guest_gl_sizei width, guest_gl_sizei height,
    guest_gl_int border, guest_gl_enum format,
    guest_gl_enum type, guest_gl_addr pixels)
{
    KAGE_VITA_DEEP_SCOPE(KVD_TEX_UPLOAD);
    kage_vita_world_seam_diag_note_tex_image(
        target, level, width, height);
    guest_gl_sizei native_width = width;
    guest_gl_sizei native_height = height;
#if defined(ISAAC_VITA_FBO_RASTER_SCALE)
    int raster_scaled = 0;
#endif
#if defined(ISAAC_VITA_FXRAY_ALPHA_MASK)
    int fxray_uploaded = 0;
#endif
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    uint64_t started_at = sceKernelGetProcessTimeWide();
#endif
#if defined(ISAAC_VITA_IO_PROFILE)
    kage_vita_io_profile_texture_begin(
        0U, width, height);
#endif
    GL_VITA_PHASE_COUNT(tex_image);
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    gl_vita_fill_note_tex_image(target, level, width, height);
#endif
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION)
    /* Any image definition may replace an attached colour surface: the owed
     * quad must precede it (stock order), and no colour stays known. */
    gl_vita_fbo_owed_materialize();
    gl_vita_fbo_color_forget_all();
#endif
#if defined(ISAAC_VITA_FBO_RASTER_SCALE)
    {
        guest_gl_uint bound = gl_vita_fbo_bound_texture();

        if (target == GL_VITA_TEXTURE_2D && level == 0 && bound) {
            gl_vita_scaled_texture *scaled = gl_vita_scaled_find(bound);

            if (gl_vita_fbo_raster_qualifies(
                    target, level, width, height, border, pixels)) {
                if (!scaled)
                    scaled = gl_vita_scaled_acquire(bound);
                if (scaled) {
                    scaled->internal_format = internal_format;
                    scaled->logical_width = width;
                    scaled->logical_height = height;
                    scaled->format = format;
                    scaled->type = type;
                    native_width = gl_vita_fbo_scale_extent(width);
                    native_height = gl_vita_fbo_scale_extent(height);
                    raster_scaled = 1;
                }
            } else if (scaled) {
                /* Redefined with data or below the edge: plain texture. */
                gl_vita_scaled_forget(bound, 0);
            }
        }
    }
#endif
#if defined(ISAAC_VITA_FXRAY_ALPHA_MASK)
    fxray_uploaded = gl_vita_fxray_try_upload(
        target, level, internal_format, width, height, border,
        format, type, pixels);
    if (!fxray_uploaded && target == GL_VITA_TEXTURE_2D &&
            gl_vita_fxray_has_live_record()) {
        gl_vita_fxray_record *record =
            gl_vita_fxray_find(gl_vita_fxray_bound_texture());

        /* Any other image definition might alter a mip or replace level zero.
         * Make the old object ordinary RGBA first, so even a rejected native
         * call leaves exactly the original sampled contents. */
        if (record)
            gl_vita_fxray_deopt(record, "tex-image");
    }
    if (!fxray_uploaded)
#endif
    {
        GL_VITA_TIME_BEGIN();
        glTexImage2D(
            (GLenum)target, (GLint)level, (GLint)internal_format,
            (GLsizei)native_width, (GLsizei)native_height, (GLint)border,
            (GLenum)format, (GLenum)type, (const void *)(uintptr_t)pixels);
        GL_VITA_TIME_END(tex_upload);
    }
#if defined(ISAAC_VITA_FBO_RASTER_SCALE)
    if (raster_scaled) {
        GL_VITA_PHASE_COUNT(fbo_raster_textures);
        gl_vita_fbo_texture_raster_changed(gl_vita_fbo_bound_texture(), 1u);
    }
#endif
#if defined(ISAAC_VITA_IO_PROFILE)
    kage_vita_io_profile_texture_end();
#endif
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    gl_vita_texture_profile_note_image(
        target, level, internal_format, width, height, border, format, type,
# if defined(ISAAC_VITA_FXRAY_ALPHA_MASK)
        !fxray_uploaded,
# else
        1,
# endif
        started_at, sceKernelGetProcessTimeWide());
#endif
}

static void vita_glTexParameteri(
    guest_gl_enum target, guest_gl_enum name, guest_gl_int parameter)
{
#if defined(ISAAC_VITA_FXRAY_ALPHA_MASK)
    /* The frozen KAGE uploader sets only filter/wrap state.  Those operations
     * sample the same texels in either representation.  Fail back to RGBA for
     * every other pname, including any future texture-swizzle exposure. */
    if (target == GL_VITA_TEXTURE_2D &&
            !gl_vita_fxray_parameter_is_sample_invariant(name) &&
            gl_vita_fxray_has_live_record()) {
        gl_vita_fxray_record *record =
            gl_vita_fxray_find(gl_vita_fxray_bound_texture());

        if (record)
            gl_vita_fxray_deopt(record, "tex-parameter");
    }
#endif
    GL_VITA_TIME_BEGIN();
    glTexParameteri((GLenum)target, (GLenum)name, (GLint)parameter);
    GL_VITA_TIME_END(state);
}

static void vita_glTexSubImage2D(
    guest_gl_enum target, guest_gl_int level,
    guest_gl_int x_offset, guest_gl_int y_offset,
    guest_gl_sizei width, guest_gl_sizei height,
    guest_gl_enum format, guest_gl_enum type,
    guest_gl_addr pixels)
{
    KAGE_VITA_DEEP_SCOPE(KVD_TEX_SUBUPLOAD);
#if defined(ISAAC_VITA_FXRAY_ALPHA_MASK)
    if (target == GL_VITA_TEXTURE_2D && gl_vita_fxray_has_live_record()) {
        gl_vita_fxray_record *record =
            gl_vita_fxray_find(gl_vita_fxray_bound_texture());

        if (record)
            gl_vita_fxray_deopt(record, "tex-sub-image");
    }
#endif
#if defined(ISAAC_VITA_IO_PROFILE)
    kage_vita_io_profile_texture_begin(
        1U, width, height);
#endif
    GL_VITA_PHASE_COUNT(tex_sub_image);
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION)
    gl_vita_fbo_owed_materialize();
    gl_vita_fbo_color_forget_all();
#endif
#if defined(ISAAC_VITA_FBO_RASTER_SCALE)
    /* The guest writes pixels: this is not a pure render target after all.
     * Restore the full logical allocation first so the upload lands 1:1. */
    if (target == GL_VITA_TEXTURE_2D)
        gl_vita_scaled_forget(gl_vita_fbo_bound_texture(), 1);
#endif
    GL_VITA_TIME_BEGIN();
    glTexSubImage2D(
        (GLenum)target, (GLint)level, (GLint)x_offset, (GLint)y_offset,
        (GLsizei)width, (GLsizei)height, (GLenum)format, (GLenum)type,
        (const void *)(uintptr_t)pixels);
    GL_VITA_TIME_END(tex_upload);
#if defined(ISAAC_VITA_IO_PROFILE)
    kage_vita_io_profile_texture_end();
#endif
}

static void vita_glUniform1fv(
    guest_gl_int location, guest_gl_sizei count, guest_gl_addr value)
{
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    if (gl_vita_uniform_skip(
            GL_VITA_UNIFORM_1FV, location, count, 0u, value, NULL,
            sizeof(guest_gl_float)))
        return;
#endif
    GL_VITA_PHASE_COUNT(uniform);
    GL_VITA_TIME_BEGIN();
    glUniform1fv((GLint)location, (GLsizei)count,
                 (const GLfloat *)(uintptr_t)value);
    GL_VITA_TIME_END(state);
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    gl_vita_uniform_forwarded(
        GL_VITA_UNIFORM_1FV, location, count, 0u, value, NULL,
        sizeof(guest_gl_float));
#endif
}

static void vita_glUniform1i(guest_gl_int location, guest_gl_int value)
{
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    if (gl_vita_uniform_skip(
            GL_VITA_UNIFORM_1I, location, 1, 0u, 0u, &value,
            sizeof value))
        return;
#endif
    GL_VITA_PHASE_COUNT(uniform);
    GL_VITA_TIME_BEGIN();
    glUniform1i((GLint)location, (GLint)value);
    GL_VITA_TIME_END(state);
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    gl_vita_uniform_forwarded(
        GL_VITA_UNIFORM_1I, location, 1, 0u, 0u, &value,
        sizeof value);
#endif
}

static void vita_glUniform1iv(
    guest_gl_int location, guest_gl_sizei count, guest_gl_addr value)
{
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    if (gl_vita_uniform_skip(
            GL_VITA_UNIFORM_1IV, location, count, 0u, value, NULL,
            sizeof(guest_gl_int)))
        return;
#endif
    GL_VITA_PHASE_COUNT(uniform);
    GL_VITA_TIME_BEGIN();
    glUniform1iv((GLint)location, (GLsizei)count,
                 (const GLint *)(uintptr_t)value);
    GL_VITA_TIME_END(state);
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    gl_vita_uniform_forwarded(
        GL_VITA_UNIFORM_1IV, location, count, 0u, value, NULL,
        sizeof(guest_gl_int));
#endif
}

static void vita_glUniform2fv(
    guest_gl_int location, guest_gl_sizei count, guest_gl_addr value)
{
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    if (gl_vita_uniform_skip(
            GL_VITA_UNIFORM_2FV, location, count, 0u, value, NULL,
            2u * sizeof(guest_gl_float)))
        return;
#endif
    GL_VITA_PHASE_COUNT(uniform);
    GL_VITA_TIME_BEGIN();
    glUniform2fv((GLint)location, (GLsizei)count,
                 (const GLfloat *)(uintptr_t)value);
    GL_VITA_TIME_END(state);
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    gl_vita_uniform_forwarded(
        GL_VITA_UNIFORM_2FV, location, count, 0u, value, NULL,
        2u * sizeof(guest_gl_float));
#endif
}

static void vita_glUniform2iv(
    guest_gl_int location, guest_gl_sizei count, guest_gl_addr value)
{
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    if (gl_vita_uniform_skip(
            GL_VITA_UNIFORM_2IV, location, count, 0u, value, NULL,
            2u * sizeof(guest_gl_int)))
        return;
#endif
    GL_VITA_PHASE_COUNT(uniform);
    GL_VITA_TIME_BEGIN();
    glUniform2iv((GLint)location, (GLsizei)count,
                 (const GLint *)(uintptr_t)value);
    GL_VITA_TIME_END(state);
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    gl_vita_uniform_forwarded(
        GL_VITA_UNIFORM_2IV, location, count, 0u, value, NULL,
        2u * sizeof(guest_gl_int));
#endif
}

static void vita_glUniform3fv(
    guest_gl_int location, guest_gl_sizei count, guest_gl_addr value)
{
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    if (gl_vita_uniform_skip(
            GL_VITA_UNIFORM_3FV, location, count, 0u, value, NULL,
            3u * sizeof(guest_gl_float)))
        return;
#endif
    GL_VITA_PHASE_COUNT(uniform);
    GL_VITA_TIME_BEGIN();
    glUniform3fv((GLint)location, (GLsizei)count,
                 (const GLfloat *)(uintptr_t)value);
    GL_VITA_TIME_END(state);
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    gl_vita_uniform_forwarded(
        GL_VITA_UNIFORM_3FV, location, count, 0u, value, NULL,
        3u * sizeof(guest_gl_float));
#endif
}

static void vita_glUniform3iv(
    guest_gl_int location, guest_gl_sizei count, guest_gl_addr value)
{
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    if (gl_vita_uniform_skip(
            GL_VITA_UNIFORM_3IV, location, count, 0u, value, NULL,
            3u * sizeof(guest_gl_int)))
        return;
#endif
    GL_VITA_PHASE_COUNT(uniform);
    GL_VITA_TIME_BEGIN();
    glUniform3iv((GLint)location, (GLsizei)count,
                 (const GLint *)(uintptr_t)value);
    GL_VITA_TIME_END(state);
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    gl_vita_uniform_forwarded(
        GL_VITA_UNIFORM_3IV, location, count, 0u, value, NULL,
        3u * sizeof(guest_gl_int));
#endif
}

static void vita_glUniform4fv(
    guest_gl_int location, guest_gl_sizei count, guest_gl_addr value)
{
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    if (gl_vita_uniform_skip(
            GL_VITA_UNIFORM_4FV, location, count, 0u, value, NULL,
            4u * sizeof(guest_gl_float)))
        return;
#endif
    GL_VITA_PHASE_COUNT(uniform);
    GL_VITA_TIME_BEGIN();
    glUniform4fv((GLint)location, (GLsizei)count,
                 (const GLfloat *)(uintptr_t)value);
    GL_VITA_TIME_END(state);
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    gl_vita_uniform_forwarded(
        GL_VITA_UNIFORM_4FV, location, count, 0u, value, NULL,
        4u * sizeof(guest_gl_float));
#endif
}

static void vita_glUniform4iv(
    guest_gl_int location, guest_gl_sizei count, guest_gl_addr value)
{
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    if (gl_vita_uniform_skip(
            GL_VITA_UNIFORM_4IV, location, count, 0u, value, NULL,
            4u * sizeof(guest_gl_int)))
        return;
#endif
    GL_VITA_PHASE_COUNT(uniform);
    GL_VITA_TIME_BEGIN();
    glUniform4iv((GLint)location, (GLsizei)count,
                 (const GLint *)(uintptr_t)value);
    GL_VITA_TIME_END(state);
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    gl_vita_uniform_forwarded(
        GL_VITA_UNIFORM_4IV, location, count, 0u, value, NULL,
        4u * sizeof(guest_gl_int));
#endif
}

static void vita_glUniformMatrix2fv(
    guest_gl_int location, guest_gl_sizei count,
    guest_gl_boolean transpose, guest_gl_addr value)
{
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    if (gl_vita_uniform_skip(
            GL_VITA_UNIFORM_MATRIX2FV, location, count, transpose,
            value, NULL, 4u * sizeof(guest_gl_float)))
        return;
#endif
    GL_VITA_PHASE_COUNT(uniform);
    GL_VITA_TIME_BEGIN();
    glUniformMatrix2fv(
        (GLint)location, (GLsizei)count, (GLboolean)transpose,
        (const GLfloat *)(uintptr_t)value);
    GL_VITA_TIME_END(state);
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    gl_vita_uniform_forwarded(
        GL_VITA_UNIFORM_MATRIX2FV, location, count, transpose,
        value, NULL, 4u * sizeof(guest_gl_float));
#endif
}

static void vita_glUniformMatrix3fv(
    guest_gl_int location, guest_gl_sizei count,
    guest_gl_boolean transpose, guest_gl_addr value)
{
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    if (gl_vita_uniform_skip(
            GL_VITA_UNIFORM_MATRIX3FV, location, count, transpose,
            value, NULL, 9u * sizeof(guest_gl_float)))
        return;
#endif
    GL_VITA_PHASE_COUNT(uniform);
    GL_VITA_TIME_BEGIN();
    glUniformMatrix3fv(
        (GLint)location, (GLsizei)count, (GLboolean)transpose,
        (const GLfloat *)(uintptr_t)value);
    GL_VITA_TIME_END(state);
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    gl_vita_uniform_forwarded(
        GL_VITA_UNIFORM_MATRIX3FV, location, count, transpose,
        value, NULL, 9u * sizeof(guest_gl_float));
#endif
}

static void vita_glUniformMatrix4fv(
    guest_gl_int location, guest_gl_sizei count,
    guest_gl_boolean transpose, guest_gl_addr value)
{
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    /* Before the redundancy skip: the shadow follows the requested matrix. */
    gl_vita_fill_note_uniform_matrix(location, count, transpose, value);
#endif
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    if (gl_vita_uniform_skip(
            GL_VITA_UNIFORM_MATRIX4FV, location, count, transpose,
            value, NULL, 16u * sizeof(guest_gl_float)))
        return;
#endif
    GL_VITA_PHASE_COUNT(uniform);
    GL_VITA_TIME_BEGIN();
    glUniformMatrix4fv(
        (GLint)location, (GLsizei)count, (GLboolean)transpose,
        (const GLfloat *)(uintptr_t)value);
    GL_VITA_TIME_END(state);
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    gl_vita_uniform_forwarded(
        GL_VITA_UNIFORM_MATRIX4FV, location, count, transpose,
        value, NULL, 16u * sizeof(guest_gl_float));
#endif
}

static void vita_glUseProgram(guest_gl_uint program)
{
    kage_vita_world_seam_diag_note_use_program(program);
#if defined(ISAAC_VITA_PHASE_PROFILE)
    s_fusion_profile.program = program;
#endif
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    if (gl_vita_use_program_skip(program))
        return;
#endif
    GL_VITA_PHASE_COUNT(use_program);
    GL_VITA_TIME_BEGIN();
    glUseProgram((GLuint)program);
    GL_VITA_TIME_END(state);
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    gl_vita_use_program_forwarded(program);
#endif
}

static void vita_glVertexAttribPointer(
    guest_gl_uint index, guest_gl_int size, guest_gl_enum type,
    guest_gl_boolean normalized, guest_gl_sizei stride,
    guest_gl_addr pointer)
{
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    gl_vita_fill_note_attrib_pointer(index, size, type, stride, pointer);
#endif
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    int valid = index < GL_VITA_VERTEX_ATTRIBS &&
        size >= 1 && size <= 4 && stride >= 0 &&
        gl_vita_typed_valid_attrib_type(type);

    if (!valid) {
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
        /* Do not reorder across invalid native pointer/config writes in
         * NO_DEBUG. Do not read the pointer to decide this predicate. */
        gl_vita_backend_attrib_external_begin();
#endif
        GL_VITA_PHASE_COUNT(typed_state_reject_invalid);
        s_gl_typed_state.attrib_pointer_known = 0u;
    } else {
        uint16_t bit = (uint16_t)(UINT16_C(1) << index);
        if ((s_gl_typed_state.attrib_pointer_known & bit) &&
                gl_vita_typed_attrib_pointer_equal(
                    &s_gl_typed_state.attrib_pointer[index],
                    size, type, normalized, stride, pointer)) {
            GL_VITA_PHASE_COUNT(typed_state_hit_attrib_pointer);
            return;
        }
    }
#endif
    GL_VITA_PHASE_COUNT(attrib_pointer);
    GL_VITA_TIME_BEGIN();
    glVertexAttribPointer(
        (GLuint)index, (GLint)size, (GLenum)type,
        (GLboolean)normalized, (GLsizei)stride,
        (const void *)(uintptr_t)pointer);
    GL_VITA_TIME_END(state);
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    if (valid) {
        gl_vita_typed_attrib_pointer *state =
            &s_gl_typed_state.attrib_pointer[index];
        state->pointer = pointer;
        state->type = type;
        state->size = size;
        state->normalized = normalized;
        state->stride = stride;
        s_gl_typed_state.attrib_pointer_known |=
            (uint16_t)(UINT16_C(1) << index);
        GL_VITA_PHASE_COUNT(typed_state_miss);
    }
#endif
}

#if defined(ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE)
/* ---- ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE begin -----------------------
 * One native call per Shader::EnableAttribs / DisableAttribs replay.  The
 * per-attribute bodies below are vita_glEnableVertexAttribArray,
 * vita_glVertexAttribPointer and vita_glDisableVertexAttribArray statement
 * for statement (keep them in step: the *-direct modes of
 * test_gl_vita_backend.ps1 compare both paths' vitaGL call trace, typed
 * state shadow and phase counters), with the pointer wrapper's argument
 * validation reduced to what the replay has not already proven: size is
 * 1..4 (checked once per batch), the type is GL_FLOAT (a constant of the
 * frozen bodies), so only `index < 16` and `stride >= 0` remain.  Exactness
 * argument: the batch issues the same wrapper bodies in the same order with
 * the same arguments, so vitaGL sees the per-call sequence, and the
 * typed-state shadow, coalescer masks, fill census and location cache are
 * written by the same statements in the same order, so every later
 * draw/sync/census decision is the per-call one; the only things removed
 * are the table-indirect calls and the redundant per-call validation.  The
 * sampler word is published at the per-call positions (get_location around
 * the lookup, toggle around the enable/disable body, pointer around the
 * pointer body, the enclosing value in between), so ph120.hot attribution
 * is unchanged too. */
# if defined(ISAAC_VITA_GUEST_SAMPLER)
#  include "kage_vita_guest_sampler.h"
#  define GL_VITA_ATTRIB_REPLAY_PUBLISH(token) \
    (g_kage_guest_last_indirect_target = (token))
# else
#  define GL_VITA_ATTRIB_REPLAY_PUBLISH(token) ((void)(token))
# endif

/* == vita_glEnableVertexAttribArray(index). */
static void gl_vita_attribs_replay_enable_one(guest_gl_uint index)
{
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    gl_vita_fill_note_attrib_toggle(index, 1);
#endif
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    if (index >= GL_VITA_VERTEX_ATTRIBS) {
        gl_vita_backend_attrib_sync();
        GL_VITA_PHASE_COUNT(typed_state_reject_invalid);
        s_gl_typed_state.attrib_enable_known = 0u;
    } else {
        uint16_t bit = (uint16_t)(UINT16_C(1) << index);
        if ((s_gl_typed_state.attrib_enable_known & bit) &&
                (s_gl_typed_state.attrib_enabled & bit)) {
            GL_VITA_PHASE_COUNT(typed_state_hit_attrib_toggle);
            return;
        }
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
        if (s_gl_typed_state.attrib_enable_known & bit) {
            gl_vita_attrib_defer(bit);
            return;
        }
#endif
    }
#endif
    GL_VITA_PHASE_COUNT(attrib_toggle);
    {
        GL_VITA_TIME_BEGIN();
        glEnableVertexAttribArray((GLuint)index);
        GL_VITA_TIME_END(state);
    }
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    if (index < GL_VITA_VERTEX_ATTRIBS) {
        uint16_t bit = (uint16_t)(UINT16_C(1) << index);
        s_gl_typed_state.attrib_enabled |= bit;
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
        s_gl_typed_state.attrib_applied |= bit;
#endif
        s_gl_typed_state.attrib_enable_known |= bit;
        GL_VITA_PHASE_COUNT(typed_state_miss);
    }
#endif
}

/* == vita_glVertexAttribPointer(index, size, GL_FLOAT, 0, stride, pointer)
 * with `valid` reduced to `index < 16 && stride >= 0` (size 1..4 and the
 * GL_FLOAT type are proven by the caller). */
static void gl_vita_attribs_replay_pointer_one(
    guest_gl_uint index, guest_gl_int size, guest_gl_sizei stride,
    int stride_valid, guest_gl_addr pointer)
{
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    int valid;
#endif

#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    gl_vita_fill_note_attrib_pointer(index, size, GL_VITA_FLOAT, stride,
                                     pointer);
#endif
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    valid = index < GL_VITA_VERTEX_ATTRIBS && stride_valid;
    if (!valid) {
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
        gl_vita_backend_attrib_external_begin();
#endif
        GL_VITA_PHASE_COUNT(typed_state_reject_invalid);
        s_gl_typed_state.attrib_pointer_known = 0u;
    } else {
        uint16_t bit = (uint16_t)(UINT16_C(1) << index);
        if ((s_gl_typed_state.attrib_pointer_known & bit) &&
                gl_vita_typed_attrib_pointer_equal(
                    &s_gl_typed_state.attrib_pointer[index],
                    size, GL_VITA_FLOAT, 0u, stride, pointer)) {
            GL_VITA_PHASE_COUNT(typed_state_hit_attrib_pointer);
            return;
        }
    }
#else
    (void)stride_valid;
#endif
    GL_VITA_PHASE_COUNT(attrib_pointer);
    {
        GL_VITA_TIME_BEGIN();
        glVertexAttribPointer(
            (GLuint)index, (GLint)size, (GLenum)GL_VITA_FLOAT,
            (GLboolean)0u, (GLsizei)stride,
            (const void *)(uintptr_t)pointer);
        GL_VITA_TIME_END(state);
    }
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    if (valid) {
        gl_vita_typed_attrib_pointer *state =
            &s_gl_typed_state.attrib_pointer[index];
        state->pointer = pointer;
        state->type = GL_VITA_FLOAT;
        state->size = size;
        state->normalized = 0u;
        state->stride = stride;
        s_gl_typed_state.attrib_pointer_known |=
            (uint16_t)(UINT16_C(1) << index);
        GL_VITA_PHASE_COUNT(typed_state_miss);
    }
#endif
}

/* == vita_glDisableVertexAttribArray(index). */
static void gl_vita_attribs_replay_disable_one(guest_gl_uint index)
{
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    gl_vita_fill_note_attrib_toggle(index, 0);
#endif
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    if (index >= GL_VITA_VERTEX_ATTRIBS) {
        gl_vita_backend_attrib_sync();
        GL_VITA_PHASE_COUNT(typed_state_reject_invalid);
        s_gl_typed_state.attrib_enable_known = 0u;
    } else {
        uint16_t bit = (uint16_t)(UINT16_C(1) << index);
        if ((s_gl_typed_state.attrib_enable_known & bit) &&
                !(s_gl_typed_state.attrib_enabled & bit)) {
            GL_VITA_PHASE_COUNT(typed_state_hit_attrib_toggle);
            return;
        }
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
        if (s_gl_typed_state.attrib_enable_known & bit) {
            gl_vita_attrib_defer(bit);
            return;
        }
#endif
    }
#endif
    GL_VITA_PHASE_COUNT(attrib_toggle);
    {
        GL_VITA_TIME_BEGIN();
        glDisableVertexAttribArray((GLuint)index);
        GL_VITA_TIME_END(state);
    }
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    if (index < GL_VITA_VERTEX_ATTRIBS) {
        uint16_t bit = (uint16_t)(UINT16_C(1) << index);
        s_gl_typed_state.attrib_enabled &= (uint16_t)~bit;
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
        s_gl_typed_state.attrib_applied &= (uint16_t)~bit;
#endif
        s_gl_typed_state.attrib_enable_known |= bit;
        GL_VITA_PHASE_COUNT(typed_state_miss);
    }
#endif
}

/* The table checks and the entry points follow gl_vita_backend_install
 * (after the oracle fake-name undefs: they read backend members by name). */
#endif

static void vita_glViewport(
    guest_gl_int x, guest_gl_int y,
    guest_gl_sizei width, guest_gl_sizei height)
{
    kage_vita_world_seam_diag_note_viewport(x, y, width, height);
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    gl_vita_fill_note_viewport(x, y, width, height);
#endif
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    int valid = width >= 0 && height >= 0;

    if (!valid) {
        GL_VITA_PHASE_COUNT(typed_state_reject_invalid);
        gl_vita_typed_invalidate_viewport();
    } else if (s_gl_typed_state.viewport_known &&
            s_gl_typed_state.viewport_x == x &&
            s_gl_typed_state.viewport_y == y &&
            s_gl_typed_state.viewport_width == width &&
            s_gl_typed_state.viewport_height == height) {
        GL_VITA_PHASE_COUNT(typed_state_hit_viewport);
        return;
    }
#endif
    GL_VITA_PHASE_COUNT(state);
#if defined(GL_VITA_LOGICAL_VIEWPORT)
    /* OpenGL rejects negative dimensions without changing viewport state.
     * Preserve both the exact invalid native call and the saved logical state. */
    if (width < 0 || height < 0) {
        GL_VITA_TIME_BEGIN();
        glViewport((GLint)x, (GLint)y, (GLsizei)width, (GLsizei)height);
        GL_VITA_TIME_END(state);
        return;
    }
    s_logical_viewport.x = x;
    s_logical_viewport.y = y;
    s_logical_viewport.width = width;
    s_logical_viewport.height = height;
    {
        /* The scale arithmetic inside is a handful of integer ops. */
        GL_VITA_TIME_BEGIN();
        gl_vita_apply_logical_viewport();
        GL_VITA_TIME_END(state);
    }
#else
    {
        GL_VITA_TIME_BEGIN();
        glViewport((GLint)x, (GLint)y, (GLsizei)width, (GLsizei)height);
        GL_VITA_TIME_END(state);
    }
#endif
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    if (valid) {
        s_gl_typed_state.viewport_x = x;
        s_gl_typed_state.viewport_y = y;
        s_gl_typed_state.viewport_width = width;
        s_gl_typed_state.viewport_height = height;
        s_gl_typed_state.viewport_known = 1u;
        GL_VITA_PHASE_COUNT(typed_state_miss);
    }
#endif
}

#if defined(ISAAC_GL_VITA_FIRST_FRAME_ORACLE)
# include "gl_vita_first_frame_oracle_vitagl_undef.h"
#elif defined(ISAAC_GL_VITA_BACKEND_ORACLE)
# include "gl_vita_backend_test_vitagl_undef.h"
#endif

int gl_vita_backend_install(void)
{
    guest_gl_backend backend;

    if (!kage_vita_backend_ready())
        return 0;
    gl_vita_backend_attrib_sync();
    gl_vita_texture_profile_reset_all();
    gl_vita_texture_profile_calibrate_clock();
#if defined(ISAAC_VITA_FXRAY_ALPHA_MASK)
    /* A repeated install may retain objects from the preceding backend
     * lifetime.  Never discard the only alpha shadows before restoring them. */
    gl_vita_fxray_deopt_all("backend-install");
    gl_vita_fxray_reset();
#endif
    gl_vita_fusion_profile_reset();
    kage_vita_world_seam_diag_reset();
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    gl_vita_redundancy_reset();
#endif
    gl_vita_location_cache_reset();
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    gl_vita_typed_state_reset();
#endif
#if defined(ISAAC_VITA_FIRST_FRAME_PROBE)
    gl_vita_first_frame_reset();
#endif
#if defined(GL_VITA_DRAW_FRAMEBUFFER_SHADOW)
    s_draw_framebuffer = 0u;
#endif
#if defined(GL_VITA_FBO_TABLE)
    gl_vita_fbo_shadow_reset();
#endif
#if defined(GL_VITA_LOGICAL_VIEWPORT)
    gl_vita_display_raster_reset();
    gl_vita_apply_logical_viewport();
#endif
    gl_vita_rbo_reset();
    s_last_error = s_partial;
    memset(&backend, 0, sizeof backend);
    backend.glActiveTexture = vita_glActiveTexture;
    backend.glAlphaFunc = vita_glAlphaFunc;
    backend.glAttachShader = vita_glAttachShader;
    backend.glBindFramebuffer = vita_glBindFramebuffer;
    backend.glBindRenderbuffer = vita_glBindRenderbuffer;
    backend.glBindTexture = vita_glBindTexture;
    backend.glBlendFuncSeparate = vita_glBlendFuncSeparate;
    backend.glCheckFramebufferStatus = vita_glCheckFramebufferStatus;
    /* glClampColorARB is absent from vitaGL. */
    backend.glClear = vita_glClear;
    backend.glClearColor = vita_glClearColor;
    backend.glClearDepth = vita_glClearDepth;
    backend.glCompileShader = vita_glCompileShader;
    backend.glCreateProgram = vita_glCreateProgram;
    backend.glCreateShader = vita_glCreateShader;
    backend.glCullFace = vita_glCullFace;
    backend.glDeleteFramebuffers = vita_glDeleteFramebuffers;
    backend.glDeleteProgram = vita_glDeleteProgram;
    backend.glDeleteRenderbuffers = vita_glDeleteRenderbuffers;
    backend.glDeleteShader = vita_glDeleteShader;
    backend.glDeleteTextures = vita_glDeleteTextures;
    backend.glDepthFunc = vita_glDepthFunc;
    backend.glDisableVertexAttribArray = vita_glDisableVertexAttribArray;
    backend.glDrawElements = vita_glDrawElements;
    backend.glEnable = vita_glEnable;
    backend.glEnableVertexAttribArray = vita_glEnableVertexAttribArray;
    backend.glFramebufferRenderbuffer = vita_glFramebufferRenderbuffer;
    backend.glFramebufferTexture2D = vita_glFramebufferTexture2D;
    backend.glGenFramebuffers = vita_glGenFramebuffers;
    backend.glGenRenderbuffers = vita_glGenRenderbuffers;
    backend.glGenTextures = vita_glGenTextures;
    backend.glGetAttribLocation = vita_glGetAttribLocation;
    backend.glGetIntegerv = vita_glGetIntegerv;
    backend.glGetProgramInfoLog = vita_glGetProgramInfoLog;
    backend.glGetProgramiv = vita_glGetProgramiv;
    /* vitaGL lacks this query, so answer it from the bounded lifecycle above. */
    backend.glGetRenderbufferParameteriv =
        vita_glGetRenderbufferParameteriv;
    backend.glGetShaderInfoLog = vita_glGetShaderInfoLog;
    backend.glGetShaderiv = vita_glGetShaderiv;
    backend.glGetString = vita_glGetString;
    backend.glGetStringi = vita_glGetStringi;
    backend.glGetUniformLocation = vita_glGetUniformLocation;
    backend.glLinkProgram = vita_glLinkProgram;
    backend.glReadPixels = vita_glReadPixels;
    backend.glRenderbufferStorage = vita_glRenderbufferStorage;
    backend.glShaderSource = vita_glShaderSource;
    backend.glTexImage2D = vita_glTexImage2D;
    backend.glTexParameteri = vita_glTexParameteri;
    backend.glTexSubImage2D = vita_glTexSubImage2D;
    backend.glUniform1fv = vita_glUniform1fv;
    backend.glUniform1i = vita_glUniform1i;
    backend.glUniform1iv = vita_glUniform1iv;
    /* vitaGL has no unsigned-vector uniform entry points. */
    backend.glUniform2fv = vita_glUniform2fv;
    backend.glUniform2iv = vita_glUniform2iv;
    backend.glUniform3fv = vita_glUniform3fv;
    backend.glUniform3iv = vita_glUniform3iv;
    backend.glUniform4fv = vita_glUniform4fv;
    backend.glUniform4iv = vita_glUniform4iv;
    backend.glUniformMatrix2fv = vita_glUniformMatrix2fv;
    /* vitaGL has no non-square matrix uniform entry points. */
    backend.glUniformMatrix3fv = vita_glUniformMatrix3fv;
    backend.glUniformMatrix4fv = vita_glUniformMatrix4fv;
    backend.glUseProgram = vita_glUseProgram;
    backend.glVertexAttribPointer = vita_glVertexAttribPointer;
    backend.glViewport = vita_glViewport;
    guest_gl_install_backend(&backend);
    s_installed = 1;
    return 1;
}

void gl_vita_backend_uninstall(void)
{
    gl_vita_backend_attrib_sync();
#if defined(ISAAC_VITA_FXRAY_ALPHA_MASK)
    gl_vita_fxray_deopt_all("backend-uninstall");
    gl_vita_fxray_reset();
#endif
    guest_gl_install_backend(NULL);
    gl_vita_fusion_profile_reset();
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    gl_vita_fill_census_reset();
#endif
#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    gl_vita_redundancy_reset();
#endif
    gl_vita_location_cache_reset();
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    gl_vita_typed_state_reset();
#endif
#if defined(ISAAC_VITA_FIRST_FRAME_PROBE)
    gl_vita_first_frame_reset();
#endif
#if defined(GL_VITA_DRAW_FRAMEBUFFER_SHADOW)
    s_draw_framebuffer = 0u;
#endif
#if defined(GL_VITA_FBO_TABLE)
    gl_vita_fbo_shadow_reset();
#endif
#if defined(GL_VITA_LOGICAL_VIEWPORT)
    gl_vita_display_raster_reset();
#endif
    gl_vita_rbo_reset();
    gl_vita_texture_profile_reset_all();
    s_installed = 0;
    s_last_error = s_partial;
}

int gl_vita_backend_installed(void) { return s_installed; }

#if defined(ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE)
/* ---- ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE (entry points) ------------- */
/* Shared decline checks: nothing has happened when this returns 0.  The
 * table members must be this backend's own wrappers - the bodies above are
 * their statements - so a partially installed or foreign table (an oracle
 * recording backend, for instance) sends the replay down its per-call path. */
static int gl_vita_attribs_replay_accept(
    const guest_gl_backend *backend, uint32_t count,
    const guest_gl_int *locations, const IsaacVitaAttribReplayTokens *tokens,
    int with_pointer)
{
    if (!backend || !locations || !tokens || count > GL_VITA_VERTEX_ATTRIBS)
        return 0;
    if (backend->glGetAttribLocation != vita_glGetAttribLocation)
        return 0;
    if (with_pointer)
        return backend->glEnableVertexAttribArray ==
                   vita_glEnableVertexAttribArray &&
               backend->glVertexAttribPointer == vita_glVertexAttribPointer;
    return backend->glDisableVertexAttribArray ==
           vita_glDisableVertexAttribArray;
}

int gl_vita_backend_attribs_replay_enable(
    const guest_gl_backend *backend, guest_gl_uint program,
    uint32_t count, const guest_gl_addr *names, guest_gl_int *locations,
    const uint8_t *components, guest_gl_sizei stride, guest_gl_addr base,
    const IsaacVitaAttribReplayTokens *tokens)
{
    uint32_t i;
    int stride_valid = stride >= 0;

    if (!components ||
            !gl_vita_attribs_replay_accept(backend, count, locations, tokens,
                                           1))
        return 0;
    for (i = 0u; i < count; ++i)
        if (components[i] < 1u || components[i] > 4u)
            return 0;
    for (i = 0u; i < count; ++i) {
        guest_gl_int location;

        if (names) {
            GL_VITA_ATTRIB_REPLAY_PUBLISH(tokens->get_location);
            location = vita_glGetAttribLocation(program, names[i]);
            GL_VITA_ATTRIB_REPLAY_PUBLISH(tokens->enclosing);
            locations[i] = location;
        } else {
            location = locations[i];
        }
        GL_VITA_ATTRIB_REPLAY_PUBLISH(tokens->toggle);
        gl_vita_attribs_replay_enable_one((guest_gl_uint)location);
        GL_VITA_ATTRIB_REPLAY_PUBLISH(tokens->enclosing);
        GL_VITA_ATTRIB_REPLAY_PUBLISH(tokens->pointer);
        gl_vita_attribs_replay_pointer_one(
            (guest_gl_uint)location, (guest_gl_int)components[i], stride,
            stride_valid, base);
        GL_VITA_ATTRIB_REPLAY_PUBLISH(tokens->enclosing);
        base += (guest_gl_addr)components[i] * 4u;      /* add ebx, eax */
    }
    return 1;
}

int gl_vita_backend_attribs_replay_disable(
    const guest_gl_backend *backend, guest_gl_uint program,
    uint32_t count, const guest_gl_addr *names, guest_gl_int *locations,
    const IsaacVitaAttribReplayTokens *tokens)
{
    uint32_t i;

    if (!gl_vita_attribs_replay_accept(backend, count, locations, tokens, 0))
        return 0;
    for (i = 0u; i < count; ++i) {
        guest_gl_int location;

        if (names) {
            GL_VITA_ATTRIB_REPLAY_PUBLISH(tokens->get_location);
            location = vita_glGetAttribLocation(program, names[i]);
            GL_VITA_ATTRIB_REPLAY_PUBLISH(tokens->enclosing);
            locations[i] = location;
        } else {
            location = locations[i];
        }
        GL_VITA_ATTRIB_REPLAY_PUBLISH(tokens->toggle);
        gl_vita_attribs_replay_disable_one((guest_gl_uint)location);
        GL_VITA_ATTRIB_REPLAY_PUBLISH(tokens->enclosing);
    }
    return 1;
}

# if defined(ISAAC_GL_VITA_BACKEND_ORACLE)
size_t gl_vita_backend_oracle_typed_state(void *out, size_t capacity)
{
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    if (capacity < sizeof s_gl_typed_state)
        return 0u;
    memcpy(out, &s_gl_typed_state, sizeof s_gl_typed_state);
    return sizeof s_gl_typed_state;
#else
    (void)out;
    (void)capacity;
    return 0u;
#endif
}
# endif
#undef GL_VITA_ATTRIB_REPLAY_PUBLISH
/* ---- ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE end ------------------------- */
#endif

size_t gl_vita_backend_resolved_count(void)
{
    return s_installed ? GL_VITA_RESOLVED_COUNT : 0u;
}
size_t gl_vita_backend_missing_count(void)
{
    return s_installed ? GL_VITA_MISSING_COUNT
                       : GUEST_GL_SURFACE_COUNT;
}
const char *gl_vita_backend_missing_symbol(size_t index)
{
    return index < GL_VITA_MISSING_COUNT
        ? s_missing_symbols[index] : NULL;
}
const char *gl_vita_backend_last_error(void)
{
    return s_last_error ? s_last_error : s_partial;
}
