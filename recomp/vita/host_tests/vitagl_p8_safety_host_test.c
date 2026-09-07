/* Native helper test, not a GXM/filtering model or part of the Vita build. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

typedef unsigned GLenum;
typedef int GLint;
typedef int GLsizei;
typedef int GLboolean;
enum { GL_FALSE, GL_TRUE };
/* Distinct stand-in tokens: no hardware ABI is simulated here. */
enum { GL_NO_ERROR, GL_INVALID_VALUE, GL_INVALID_OPERATION, GL_OUT_OF_MEMORY,
    GL_TEXTURE_2D, GL_RGBA, GL_UNSIGNED_BYTE, TEX_UNINITIALIZED, TEX_VALID,
    SCE_GXM_TEXTURE_FORMAT_P8_ABGR, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR,
    GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER, GL_TEXTURE_WRAP_S,
    GL_TEXTURE_WRAP_T, GL_NEAREST, GL_LINEAR, GL_TEXTURE_LOD_BIAS,
    SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_TEXTURE_FILTER_POINT,
    SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_BLEND_FUNC_ADD,
    SCE_GXM_COLOR_MASK_ALL, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL, SCE_GXM_BLEND_FACTOR_ONE,
    SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, SCE_GXM_BLEND_FACTOR_SRC_ALPHA,
    GL_CLAMP_TO_EDGE, GL_CLAMP, GL_REPEAT, GL_MIRRORED_REPEAT, GL_MIRROR_CLAMP_EXT,
    SCE_GXM_TEXTURE_LINEAR, SCE_GXM_TEXTURE_SWIZZLED,
    SCE_GXM_TEXTURE_SWIZZLED_ARBITRARY, SCE_GXM_TEXTURE_ADDR_CLAMP,
    SCE_GXM_TEXTURE_ADDR_REPEAT, SCE_GXM_TEXTURE_ADDR_MIRROR,
    SCE_GXM_TEXTURE_ADDR_MIRROR_CLAMP, SCE_GXM_TEXTURE_MIP_FILTER_DISABLED };
typedef unsigned SceGxmTextureFilter;
typedef unsigned SceGxmPrimitiveType;
typedef unsigned SceGxmTextureAddrMode;
#define GXM_TEX_MAX_SIZE 4096
#define SCE_GXM_PALETTE_ALIGNMENT 64
#define OBJ_NOT_USED UINT32_MAX
typedef struct {
    uint32_t width, height, format, mip_count, layout;
    void *data, *palette;
    unsigned u_mode, v_mode, min_filter, mag_filter, mip_filter, lod_bias;
} SceGxmTexture;
typedef struct {
    unsigned status, mip_count, ref_counter, overridden, use_mips, last_frame;
    unsigned faces_counter;
    unsigned format, u_mode, v_mode, min_filter, mag_filter, mip_filter, lod_bias;
    void *data, *palette_data;
    void (*write_cb)(void *, uint32_t);
    SceGxmTexture gxm_tex;
} texture;
#define HAVE_ISAAC_P8_SAFE_UPLOAD 1
#define HAVE_ISAAC_LASER_ATLAS_P8 1
#define HAVE_ISAAC_LASER_LIGHT_HALO_CLIP 1
#define HAVE_ISAAC_COLOROFFSET_STAGING_PROOF 1
#define HAVE_ISAAC_COLOROFFSET_PLAIN_FASTPATH 1
#define TEXTURES_NUM 8
#define COMBINED_TEXTURE_IMAGE_UNITS_NUM 2
static texture texture_slots[TEXTURES_NUM];
static struct { unsigned tex_id[3]; } texture_units[COMBINED_TEXTURE_IMAGE_UNITS_NUM];
static void *samplers[COMBINED_TEXTURE_IMAGE_UNITS_NUM];
void isaacLaserP8ReleaseBackup(texture *tex);
static char halo_success_line[512];
static unsigned halo_success_lines;
static unsigned cap_success_lines;
static GLboolean is_fbo_float;
static unsigned atlas_success_lines;
static int isaac_test_printf(const char *format, ...)
{
    char line[512];
    va_list arguments;
    va_start(arguments, format);
    int size = vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    assert(size >= 0 && (size_t)size < sizeof line);
    if (strstr(line, "Isaac light halo: atlas=") == line) {
        memcpy(halo_success_line, line, (size_t)size + 1u);
        ++halo_success_lines;
    }
    if (strstr(line, "Isaac red cap side: atlas=") == line) {
        assert(strstr(line, " v=unchanged filter=point depth-test="));
        ++cap_success_lines;
    }
    if (strstr(line, "Isaac laser atlas: nearest=point applied=1 restored=1") == line)
        ++atlas_success_lines;
    fputs(line, stdout);
    return size;
}
#define sceClibPrintf isaac_test_printf

static unsigned allocations, fail_at, immediate_frees, retired, retired_frame;
static void *gpu_alloc_mapped_for_gpu(size_t size)
{
    if (++allocations == fail_at)
        return NULL;
    void *result = malloc(size);
    assert(result);
    return result;
}
static void *gpu_alloc_mapped_aligned_for_gpu(size_t alignment, size_t size)
{
    assert(alignment == SCE_GXM_PALETTE_ALIGNMENT);
    return gpu_alloc_mapped_for_gpu(size);
}
static void vgl_free(void *value)
{
    assert(value);
    ++immediate_frees;
    free(value);
}
static void gpu_free_texture_data(texture *tex)
{
    isaacLaserP8ReleaseBackup(tex);
    retired_frame = tex->last_frame;
    if (tex->data) { ++retired; free(tex->data); tex->data = NULL; }
    if (tex->palette_data) {
        ++retired; free(tex->palette_data); tex->palette_data = NULL;
    }
}
#define vgl_fast_memcpy memcpy
#define vgl_memset memset
static uint32_t read_rgba8888(const void *value)
{
    const uint8_t *p = value;
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
        ((uint32_t)p[1] << 8) | p[0];
}
static void write_rgba8888(void *value, uint32_t color)
{ memcpy(value, &color, sizeof color); }
static void vglInitLinearTexture(SceGxmTexture *t, void *data, unsigned format,
    unsigned width, unsigned height, unsigned mip_count)
{
    memset(t, 0, sizeof *t);
    t->width = width; t->height = height; t->format = format;
    t->data = data; t->mip_count = mip_count;
    t->layout = SCE_GXM_TEXTURE_LINEAR;
}
static void vglSetTexPalette(SceGxmTexture *t, void *palette)
{ t->palette = palette; }
static unsigned gxm_calls, gxm_fail_at;
static int sceGxmTextureInitLinear(SceGxmTexture *t, const void *data,
    unsigned format, unsigned width, unsigned height, unsigned mip_count)
{
    if (++gxm_calls == gxm_fail_at)
        return -1;
    vglInitLinearTexture(t, (void *)data, format, width, height, mip_count);
    return 0;
}
static int sceGxmTextureSetPalette(SceGxmTexture *t, const void *palette)
{
    if (++gxm_calls == gxm_fail_at)
        return -1;
    vglSetTexPalette(t, (void *)palette);
    return 0;
}
static void vglGetTexSizes(SceGxmTexture *t, uint32_t *w, uint32_t *h)
{ *w = t->width; *h = t->height; }
static unsigned vglGetTexFormat(SceGxmTexture *t) { return t->format; }
#define SETTER(name, field) \
    static void name(SceGxmTexture *t, unsigned value) { t->field = value; }
SETTER(vglSetTexUMode, u_mode)
SETTER(vglSetTexVMode, v_mode)
SETTER(vglSetTexMinFilter, min_filter)
SETTER(vglSetTexMagFilter, mag_filter)
SETTER(vglSetTexMipFilter, mip_filter)
SETTER(vglSetTexLodBias, lod_bias)
SETTER(vglSetTexMipmapCount, mip_count)

#if defined(HAVE_ISAAC_LASER_P8_SWIZZLE) || defined(HAVE_ISAAC_LASER_LIGHT_NEAREST) || defined(HAVE_ISAAC_LASER_ATLAS_NEAREST)
#define CHECKED_SETTER(name, field) \
    static int name(SceGxmTexture *t, unsigned value) { \
        if (++gxm_calls == gxm_fail_at) return -1; t->field = value; return 0; }
CHECKED_SETTER(sceGxmTextureSetMinFilter, min_filter)
CHECKED_SETTER(sceGxmTextureSetMagFilter, mag_filter)
#endif

#ifdef HAVE_ISAAC_LASER_P8_SWIZZLE
/* A deliberately NON-Morton permutation mock verifies the caller never
 * assumes linear indices. This does NOT execute/test the pinned NEON
 * SwizzleTexData8Bpp implementation or native GXM sampling. */
#define SUPPORT_SMALL_FMT 1
#define TEXTURE_SWIZZLER_H_ 1
static unsigned swizzle_calls;
static void SwizzleTexData8Bpp(uint8_t *dst, uint8_t *src, uint32_t x,
        uint32_t y, uint32_t width, uint32_t height, uint32_t stride,
        uint32_t tile_size)
{
    assert(dst != src && x == 0 && y == 0 && height == 64);
    assert((width == 448 || width == 512) && stride == width && tile_size == 64);
    ++swizzle_calls;
    for (unsigned i = 0; i < width * height; ++i)
        dst[i ^ 63U] = src[i];
}
static int mock_swizzled_init(SceGxmTexture *t, const void *data,
        unsigned format, unsigned width, unsigned height, unsigned mips,
        unsigned layout)
{
    if (++gxm_calls == gxm_fail_at)
        return -1;
    assert(height == 64 && mips == 1 && format == SCE_GXM_TEXTURE_FORMAT_P8_ABGR);
    assert(width == (layout == SCE_GXM_TEXTURE_SWIZZLED ? 512U : 448U));
    vglInitLinearTexture(t, (void *)data, format, width, height, mips);
    t->layout = layout;
    return 0;
}
static int sceGxmTextureInitSwizzled(SceGxmTexture *t, const void *data,
        unsigned format, unsigned width, unsigned height, unsigned mips)
{ return mock_swizzled_init(t, data, format, width, height, mips, SCE_GXM_TEXTURE_SWIZZLED); }
static int sceGxmTextureInitSwizzledArbitrary(SceGxmTexture *t, const void *data,
        unsigned format, unsigned width, unsigned height, unsigned mips)
{ return mock_swizzled_init(t, data, format, width, height, mips, SCE_GXM_TEXTURE_SWIZZLED_ARBITRARY); }
static unsigned sceGxmTextureGetType(const SceGxmTexture *t) { return t->layout; }
CHECKED_SETTER(sceGxmTextureSetMipFilter, mip_filter)
CHECKED_SETTER(sceGxmTextureSetLodBias, lod_bias)
CHECKED_SETTER(sceGxmTextureSetMipmapCount, mip_count)
static int mock_addr_mode(SceGxmTexture *t, unsigned value, unsigned *field)
{
    if (++gxm_calls == gxm_fail_at ||
            (t->layout == SCE_GXM_TEXTURE_SWIZZLED_ARBITRARY &&
             value == SCE_GXM_TEXTURE_ADDR_MIRROR))
        return -1;
    *field = value;
    return 0;
}
static int sceGxmTextureSetUAddrMode(SceGxmTexture *t, unsigned value)
{ return mock_addr_mode(t, value, &t->u_mode); }
static int sceGxmTextureSetVAddrMode(SceGxmTexture *t, unsigned value)
{ return mock_addr_mode(t, value, &t->v_mode); }
#endif

#include "../vitagl-stock-reference/isaac_p8_texture_safety.h"
#include "../vitagl-stock-reference/isaac_laser_p8.h"

typedef struct {
    unsigned primitive; uint16_t *indices; GLsizei count;
#if defined(HAVE_ISAAC_LASER_LIGHT_NEAREST) || defined(HAVE_ISAAC_LASER_ATLAS_NEAREST)
    SceGxmTexture nearest_original, nearest_point;
    GLboolean nearest_ready;
#endif
#ifdef HAVE_ISAAC_LASER_ATLAS_NEAREST
    GLboolean atlas_nearest_ready;
#endif
} IsaacLightHaloDraw;
#if defined(HAVE_ISAAC_LASER_LIGHT_NEAREST) || defined(HAVE_ISAAC_LASER_ATLAS_NEAREST) || \
    (defined(HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP) && HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP == 1)
static unsigned sceGxmTextureGetMinFilter(const SceGxmTexture *t) { return t->min_filter; }
static unsigned sceGxmTextureGetMagFilter(const SceGxmTexture *t) { return t->mag_filter; }
#endif
#if defined(HAVE_ISAAC_LASER_LIGHT_NEAREST) || defined(HAVE_ISAAC_LASER_ATLAS_NEAREST)
typedef int SceGxmContext;
enum { SCE_GXM_INDEX_FORMAT_U16 = 9999 };
static unsigned nearest_bind_calls, nearest_draw_calls, nearest_bind_fail_mask;
static unsigned nearest_expected_count = 12;
static const void *nearest_drawn_indices;
static int nearest_draw_result;
static SceGxmTexture nearest_bound, nearest_drawn;
static int sceGxmSetFragmentTexture(SceGxmContext *context, unsigned unit,
        const SceGxmTexture *descriptor)
{
    assert(context && unit == 0);
    ++nearest_bind_calls;
    if (nearest_bind_fail_mask & (1u << nearest_bind_calls)) return -7;
    nearest_bound = *descriptor;
    return 0;
}
static int sceGxmDraw(SceGxmContext *context, unsigned primitive,
        unsigned format, const void *indices, unsigned count)
{
    assert(context && primitive == SCE_GXM_PRIMITIVE_TRIANGLES &&
        format == SCE_GXM_INDEX_FORMAT_U16 && indices && count == nearest_expected_count);
    ++nearest_draw_calls;
    nearest_drawn = nearest_bound;
    nearest_drawn_indices = indices;
    return nearest_draw_result;
}
#include "../vitagl-stock-reference/isaac_laser_light_nearest.h"
#endif

#ifdef HAVE_ISAAC_LASER_ATLAS_NEAREST
enum { UNIFORM_SAMPLER = 193 };
typedef struct { int type, sampler_index; } atlas_test_uniform;
typedef struct {
    unsigned isaac_coloroffset_link_vertex_exact;
    unsigned max_frag_texunit_idx, max_vert_texunit_idx;
    atlas_test_uniform *frag_texunits[1];
} program;
static unsigned sceGxmTextureGetMipFilter(const SceGxmTexture *t) { return t->mip_filter; }
#include "../vitagl-stock-reference/isaac_laser_atlas_nearest.h"
#endif
static int depth_test_state, stencil_test_state, blend_state = 1;
static unsigned polygon_mode_front = SCE_GXM_POLYGON_MODE_TRIANGLE_FILL;
static unsigned polygon_mode_back = SCE_GXM_POLYGON_MODE_TRIANGLE_FILL;
static unsigned blend_func_rgb = SCE_GXM_BLEND_FUNC_ADD;
static unsigned blend_func_a = SCE_GXM_BLEND_FUNC_ADD;
static unsigned blend_color_mask = SCE_GXM_COLOR_MASK_ALL;
static unsigned blend_dfactor_rgb = SCE_GXM_BLEND_FACTOR_ONE;
static unsigned blend_dfactor_a = SCE_GXM_BLEND_FACTOR_ONE;
static unsigned blend_sfactor_rgb = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
static unsigned blend_sfactor_a = SCE_GXM_BLEND_FACTOR_ONE;
static void *halo_pool[4];
static size_t halo_pool_bytes[4];
static unsigned halo_allocations, halo_fail_at;
static unsigned halo_gc_attack, halo_gc_mark, halo_gc_clean, halo_gc_visits;
static void *halo_gc_pending[4], *halo_gc_retired_first;
static unsigned halo_cpu_recovery_at, halo_recovery_resets;
static GLboolean halo_fb_float, halo_attach_pending, halo_dirty_fb;
static void *halo_mutate_vertices;
static uint16_t *halo_mutate_indices;
#if defined(HAVE_ISAAC_LASER_HALO_PROVEN_EMIT) && HAVE_ISAAC_LASER_HALO_PROVEN_EMIT == 1
static unsigned halo_proven_calls, halo_force_emit_failure;
#endif
/* Source-order witness, not SDK emulation. The existing fresh
 * prove_metadata_once checks these exact production anchors in scene_reset,
 * _glFramebufferTexture2D, glFinish and unsafe CPU allocator recovery. */
static void halo_recovery_reset(void)
{
    is_fbo_float = halo_fb_float; /* snapshot BEFORE the lazy attachment */
    halo_dirty_fb = GL_FALSE;
    if (halo_attach_pending) {
        halo_fb_float = GL_TRUE; /* different-backed RGBA16F respec */
        halo_attach_pending = GL_FALSE;
        halo_dirty_fb = GL_TRUE;
    }
    ++halo_recovery_resets;
}
static void *gpu_alloc_mapped_temp(size_t bytes)
{
    assert(halo_allocations < 4u);
    ++halo_allocations;
    if (halo_allocations == halo_cpu_recovery_at) {
        /* Pool overflow -> CPU backing allocation's first malloc fails ->
         * glFinish marks dirty and resets the scene -> GC -> retry succeeds. */
        assert(!is_fbo_float && halo_fb_float && halo_dirty_fb && halo_recovery_resets == 1u);
        halo_dirty_fb = GL_TRUE;
        halo_recovery_reset();
        assert(is_fbo_float && !halo_dirty_fb && halo_recovery_resets == 2u);
    }
    if (halo_allocations == 1u && halo_mutate_vertices) {
        memset(halo_mutate_vertices, 0, 4u * 88u);
        memset(halo_mutate_indices, 0, 6u * sizeof(uint16_t));
    }
    if (halo_allocations == halo_fail_at)
        return NULL;
    if (halo_gc_attack && halo_allocations == 2u) {
        /* Real bucket progression: first output markedP, clean startsP+1;
         * the second CPU allocation fails until its fourth GC retry.
         * Quarantine the logical free until teardown so the negative control
         * can detect a retired output without performing host use-after-free. */
        for (unsigned retry = 0; retry < 4u; ++retry) {
            if (halo_gc_pending[halo_gc_clean] == halo_pool[0])
                halo_gc_retired_first = halo_pool[0];
            halo_gc_pending[halo_gc_clean] = NULL;
            halo_gc_clean = (halo_gc_clean + 1u) % 4u;
            halo_gc_mark = (halo_gc_mark + 1u) % 4u;
            ++halo_gc_visits;
        }
    }
    void *result = malloc(bytes);
    halo_pool[halo_allocations - 1u] = result;
    halo_pool_bytes[halo_allocations - 1u] = bytes;
    if (halo_gc_attack) halo_gc_pending[halo_gc_mark] = result;
    return result;
}
static void halo_pool_retire(void)
{
    for (unsigned i = 0; i < 4u; ++i) {
        free(halo_pool[i]); halo_pool[i] = NULL;
        halo_pool_bytes[i] = 0u;
        halo_gc_pending[i] = NULL;
    }
    halo_allocations = halo_fail_at = halo_cpu_recovery_at = halo_recovery_resets = 0u;
    halo_fb_float = halo_attach_pending = halo_dirty_fb = GL_FALSE;
    is_fbo_float = GL_FALSE;
    halo_mutate_vertices = NULL;
    halo_mutate_indices = NULL;
    halo_gc_attack = halo_gc_mark = halo_gc_visits = 0u;
    halo_gc_clean = 1u;
    halo_gc_retired_first = NULL;
#if defined(HAVE_ISAAC_LASER_HALO_PROVEN_EMIT) && HAVE_ISAAC_LASER_HALO_PROVEN_EMIT == 1
    halo_proven_calls = halo_force_emit_failure = 0u;
#endif
}
#if defined(HAVE_ISAAC_LASER_HALO_PROVEN_EMIT) && HAVE_ISAAC_LASER_HALO_PROVEN_EMIT == 1
#include "../vitagl-stock-reference/isaac_laser_light_halo_policy.h"
/* Interpose only the native helper's private entry, not the checked API.
 * A forced emitter failure must retain the existing unpublished fallback. */
static inline int halo_fixture_emit_proven(
        isaac_light_halo_vertex *output, uint32_t vertex_capacity,
        uint16_t *output_indices, uint32_t index_capacity,
        const isaac_light_halo_vertex *vertices, const uint16_t *indices,
        uint32_t index_count, uint32_t width, uint32_t crop)
{
    ++halo_proven_calls;
    if (halo_force_emit_failure) return 0;
    return isaac_light_halo_emit_proven(output, vertex_capacity, output_indices,
        index_capacity, vertices, indices, index_count, width, crop);
}
#define isaac_light_halo_emit_proven halo_fixture_emit_proven
#endif
#if defined(HAVE_ISAAC_LASER_HALO_PROFILE) && HAVE_ISAAC_LASER_HALO_PROFILE
#define isaac_light_halo_try_stage isaac_light_halo_try_stage_observed
#endif
#include "../vitagl-stock-reference/isaac_laser_light_halo_vitagl.h"
#if defined(HAVE_ISAAC_LASER_HALO_PROVEN_EMIT) && HAVE_ISAAC_LASER_HALO_PROVEN_EMIT == 1
#undef isaac_light_halo_emit_proven
#endif
#if defined(HAVE_ISAAC_LASER_HALO_PROFILE) && HAVE_ISAAC_LASER_HALO_PROFILE
#undef isaac_light_halo_try_stage
static uint32_t halo_last_reason;
/* Existing helper tests enter after metadata admission. This adapter mimics
 * only the owner commit; the freshly patched owner block is tested elsewhere. */
static GLboolean isaac_light_halo_try_stage(texture *tex, uint32_t sampler,
        const void *source, uint32_t vertices, const uint16_t *indices,
        GLsizei count, IsaacLightHaloDraw *submission, void **destination,
        uint32_t *new_vertices)
{
    uint32_t reason = UINT32_MAX;
    vglIsaacLaserHaloStats before = isaac_halo_stats;
    GLboolean staged = isaac_light_halo_try_stage_observed(tex, sampler,
        source, vertices, indices, count, submission, destination, new_vertices, &reason);
    assert(reason < ISAAC_HALO_OUTCOMES || reason == ISAAC_HALO_TOO_BIG);
    assert(!memcmp(&before, &isaac_halo_stats, sizeof before)); /* no helper commits */
    isaac_halo_profile_commit(reason);
    halo_last_reason = reason;
    return staged;
}
#endif

static uint32_t *laser_rgba_palette(unsigned width, const uint32_t *palette)
{
    uint32_t *rgba = calloc(width * 64U, sizeof(uint32_t));
    assert(rgba);
    unsigned pixel = 0;
    for (unsigned run = 0; run < sizeof isaac_laser_p8_runs /
            sizeof isaac_laser_p8_runs[0]; ++run)
        for (unsigned n = 0; n < (isaac_laser_p8_runs[run] >> 8); ++n, ++pixel)
            rgba[pixel / 448U * width + pixel % 448U] =
                palette[isaac_laser_p8_runs[run] & 255U];
    assert(pixel == 448U * 64U);
    return rgba;
}

static uint32_t *laser_rgba(unsigned width)
{ return laser_rgba_palette(width, isaac_laser_p8_palette); }

static texture laser_empty(void)
{
    texture tex;
    memset(&tex, 0, sizeof tex);
    tex.status = TEX_UNINITIALIZED;
    tex.last_frame = OBJ_NOT_USED;
    tex.u_mode = 3; tex.v_mode = 4; tex.min_filter = 5;
    tex.mag_filter = 6; tex.mip_filter = 7; tex.lod_bias = 8;
    return tex;
}

static int laser_upload(texture *tex, unsigned width, const void *rgba)
{
    return isaac_laser_p8_try_upload(tex, GL_TEXTURE_2D, 0, GL_RGBA,
        (GLsizei)width, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba, 0);
}

static uint32_t laser_pixel(const texture *tex, unsigned index)
{
#ifdef HAVE_ISAAC_LASER_P8_SWIZZLE
    if (tex->gxm_tex.layout != SCE_GXM_TEXTURE_LINEAR)
        index ^= 63U; /* Invert only the explicit mock permutation above. */
#endif
    return ((uint32_t *)tex->palette_data)[((uint8_t *)tex->data)[index]];
}

#ifdef HAVE_ISAAC_LASER_ATLAS_NEAREST
static void test_atlas_nearest(void)
{
    uint16_t original_indices[12] = {0}, halo_indices[12] = {0};
    atlas_test_uniform sampler = {UNIFORM_SAMPLER, 0};
    program p = {1, 1, 0, {&sampler}};
    uint32_t *rgba = laser_rgba(448);
    texture tex = laser_empty();
    tex.min_filter = tex.mag_filter = SCE_GXM_TEXTURE_FILTER_LINEAR;
    tex.mip_filter = SCE_GXM_TEXTURE_MIP_FILTER_DISABLED;
    gxm_fail_at = 0;
    assert(laser_upload(&tex, 448, rgba));
    texture unchanged = tex;
    {
        IsaacLightHaloDraw submission = {.primitive = SCE_GXM_PRIMITIVE_TRIANGLES};
        sampler.sampler_index = 1; /* GL texture unit is NOT native fragment slot. */
        isaac_laser_atlas_nearest_prepare(&p, GL_TRUE, &tex, &submission);
        assert(submission.atlas_nearest_ready);
        sampler.sampler_index = 0;
    }
    for (unsigned scenario = 0; scenario < 20; ++scenario) {
        IsaacLightHaloDraw submission = {.primitive = SCE_GXM_PRIMITIVE_TRIANGLES};
        program test = p;
        atlas_test_uniform test_sampler = sampler;
        test.frag_texunits[0] = &test_sampler;
        texture foreign = tex;
        texture *selected = &tex;
        GLboolean exact = GL_TRUE;
        switch (scenario) {
        case 1: exact = GL_FALSE; break;
        case 2: test.isaac_coloroffset_link_vertex_exact = 0; break;
        case 3: test.max_frag_texunit_idx = 2; break;
        case 4: test.max_vert_texunit_idx = 1; break;
        case 5: test.frag_texunits[0] = NULL; break;
        case 6: test_sampler.type = 99; break;
        case 7: test_sampler.sampler_index = -1; break;
        case 8: samplers[0] = &p; break;
        case 9: tex.overridden = 1; break;
        case 10: tex.use_mips = 1; break;
        case 11: tex.mip_count = 2; break;
        case 12: selected = &foreign; break;
        case 13: tex.gxm_tex.min_filter = 99; break;
        case 14: tex.gxm_tex.mag_filter = 99; break;
        case 15: tex.gxm_tex.mip_filter = 99; break;
        case 16: is_fbo_float = GL_TRUE; break;
        case 17: submission.primitive = 99; break;
        case 18: tex.gxm_tex.min_filter = tex.gxm_tex.mag_filter = SCE_GXM_TEXTURE_FILTER_POINT; break;
        case 19: selected = NULL; break;
        }
        isaac_laser_atlas_nearest_prepare(&test, exact, selected, &submission);
        assert(submission.atlas_nearest_ready == (scenario == 0));
        assert(!submission.indices && submission.count == 0);
        samplers[0] = NULL; is_fbo_float = GL_FALSE; tex = unchanged;
    }
    for (unsigned failure = 1; failure <= 2; ++failure) {
        IsaacLightHaloDraw submission = {.primitive = SCE_GXM_PRIMITIVE_TRIANGLES};
        gxm_fail_at = gxm_calls + failure;
        isaac_laser_atlas_nearest_prepare(&p, GL_TRUE, &tex, &submission);
        assert(!submission.atlas_nearest_ready && !submission.nearest_ready);
        assert(!memcmp(&tex, &unchanged, sizeof tex));
    }
    gxm_fail_at = 0;
    {
        IsaacLightHaloDraw old = {.primitive = SCE_GXM_PRIMITIVE_TRIANGLES,
            .nearest_ready = GL_TRUE};
        old.nearest_original = tex.gxm_tex;
        old.nearest_point = tex.gxm_tex;
        IsaacLightHaloDraw saved = old;
        isaac_laser_atlas_nearest_prepare(NULL, GL_TRUE, &tex, &old);
        isaac_laser_atlas_nearest_prepare(&p, GL_TRUE, &tex, NULL);
        isaac_laser_atlas_nearest_prepare(&p, GL_FALSE, &tex, &old);
        assert(!memcmp(&old, &saved, sizeof old)); /* Separate old option survives refusal. */
    }
    for (unsigned halo = 0; halo < 2; ++halo) {
        IsaacLightHaloDraw submission = {.primitive = SCE_GXM_PRIMITIVE_TRIANGLES};
        if (halo) { submission.indices = halo_indices; submission.count = 12; }
        isaac_laser_atlas_nearest_prepare(&p, GL_TRUE, &tex, &submission);
        assert(submission.atlas_nearest_ready && submission.nearest_ready);
        for (unsigned leg = 0; leg < 6; ++leg) {
            unsigned scenario = (leg + 1u) % 6u; /* Failure paths before first success. */
            SceGxmContext context = 1;
            memset(&isaac_light_nearest_state, 0, sizeof isaac_light_nearest_state);
            nearest_expected_count = halo ? 12 : 6144; /* No new batch cap or vertex scan. */
            nearest_bound = tex.gxm_tex;
            nearest_bind_calls = nearest_draw_calls = 0;
            nearest_bind_fail_mask = scenario == 1 ? 2u : scenario == 2 ? 6u : scenario == 3 ? 4u : 0u;
            nearest_draw_result = scenario == 4 ? -9 : 0;
            if (scenario == 5) isaac_light_nearest_state.disabled = GL_TRUE;
            isaac_laser_atlas_draw(&context, &submission, SCE_GXM_PRIMITIVE_TRIANGLES,
                original_indices, 6144);
            assert(nearest_bind_calls == (scenario == 5 ? 0u : 2u));
            assert(nearest_draw_calls == (scenario == 2 ? 0u : 1u));
            if (nearest_draw_calls)
                assert(nearest_drawn_indices == (halo ? halo_indices : original_indices));
            if (scenario != 2 && scenario != 3)
                assert(!memcmp(&nearest_bound, &tex.gxm_tex, sizeof nearest_bound));
            if (scenario == 1 || scenario == 5)
                assert(!memcmp(&nearest_drawn, &tex.gxm_tex, sizeof nearest_drawn));
            if (scenario == 0) {
                assert(nearest_drawn.min_filter == SCE_GXM_TEXTURE_FILTER_POINT);
                assert(nearest_drawn.mag_filter == SCE_GXM_TEXTURE_FILTER_POINT);
            }
            if (halo == 0 && scenario != 0) assert(atlas_success_lines == 0u);
            assert(!memcmp(&tex, &unchanged, sizeof tex));
        }
    }
    assert(atlas_success_lines == 1u);
    nearest_expected_count = 12;
    memset(&isaac_light_nearest_state, 0, sizeof isaac_light_nearest_state);
    nearest_bind_fail_mask = 0; nearest_draw_result = 0;
    gpu_free_texture_data(&tex);
    free(rgba);
    puts("Exact atlas POINT caller MOCK: immutable-proof rejection, sampler/live ownership/descriptor guards, checked setter1/2, original large-batch GPU indices or halo indices, bind/draw/restore failures and shared disable PASS; no GXM sampling/FPS proof");
}
#endif

/* The frozen producer TU uses strict floating point. Volatile intermediate
 * stores model its individual f32 operations even with native -ffast-math.
 * This is shared by the exact logical-448 body and cap fixture populations. */
static float laser_atlas_producer_u(unsigned x, unsigned storage_width)
{
    volatile float logical = 448.0f, storage = (float)storage_width;
    volatile float inverse = 1.0f / logical;
    volatile float normalized = (float)x * inverse;
    volatile float scale = logical / storage;
    volatile float result = normalized * scale;
    return result;
}
#if defined(HAVE_ISAAC_LASER_LIGHT_PRODUCER_UV) && HAVE_ISAAC_LASER_LIGHT_PRODUCER_UV == 1
#define TEST_LIGHT_PRODUCER_ENABLED 1
#else
#define TEST_LIGHT_PRODUCER_ENABLED 0
#endif

static void test_light_halo(void)
{
    isaac_light_halo_vertex input[4] = {0}, output[8];
    uint16_t indices[] = {0, 2, 1, 1, 2, 3}, emitted[12];
    for (unsigned i = 0; i < 4u; ++i) {
        float x = (float)(i & 1u) * 32.0f;
        float y = (float)(i >> 1u) * 64.0f;
        input[i].lane[0] = x; input[i].lane[1] = y;
        input[i].lane[3] = x / 32.0f; input[i].lane[4] = y / 64.0f;
        input[i].lane[5] = 1.0f; input[i].lane[6] = 1.0f;
        input[i].lane[7] = (416.0f + x) / 512.0f;
        input[i].lane[8] = y / 64.0f;
    }
    assert(isaac_light_halo_emit(output, 8, emitted, 12, input, 4, indices, 6, 512));
    float area = 0.0f;
    for (unsigned i = 0; i < 12u; i += 3u) {
        float *a = output[emitted[i]].lane, *b = output[emitted[i+1]].lane;
        float *c = output[emitted[i+2]].lane;
        area += fabsf((b[0]-a[0])*(c[1]-a[1]) - (b[1]-a[1])*(c[0]-a[0])) * 0.5f;
    }
    assert(fabsf(area - 23.0f * 64.0f) < 0.001f);
    for (unsigned i = 0; i < 8u; ++i) {
        assert(fabsf(output[i].lane[3] - output[i].lane[0]/32.0f) < 0.00001f);
        assert(fabsf(output[i].lane[4] - output[i].lane[1]/64.0f) < 0.00001f);
        assert(output[i].lane[0] >= 5.0f && output[i].lane[0] <= 28.0f);
        assert(output[i].lane[7] >= 421.0f/512.0f && output[i].lane[7] <= 444.0f/512.0f);
    }
    /* A non-affine quad color catches a bilinear remap across its diagonal:
     * each four-vertex output group must retain its own original plane. */
    isaac_light_halo_vertex bent[4];
    memcpy(bent, input, sizeof bent);
    bent[0].lane[3] = 0.1f; bent[1].lane[3] = 0.3f;
    bent[2].lane[3] = 0.6f; bent[3].lane[3] = 0.9f;
    for (unsigned width = 448; width <= 512; width += 64) {
        for (unsigned i = 0; i < 4u; ++i)
            bent[i].lane[7] = TEST_LIGHT_PRODUCER_ENABLED ?
                laser_atlas_producer_u(416u + (i & 1u)*32u, width) :
                (416.0f + bent[i].lane[0]) / (float)width;
        for (unsigned reverse = 0; reverse < 2u; ++reverse) {
            uint16_t winding[6];
            memcpy(winding, indices, sizeof winding);
            if (reverse) for (unsigned t = 0; t < 6u; t += 3u) {
                uint16_t swap = winding[t]; winding[t] = winding[t+1];
                winding[t+1] = swap;
            }
            assert(isaac_light_halo_emit(output, 8, emitted, 12,
                bent, 4, winding, 6, width));
            for (unsigned i = 0; i < 8u; ++i) {
                float x = output[i].lane[0] / 32.0f;
                float y = output[i].lane[1] / 64.0f;
                float expected = i < 4u ? 0.1f + 0.2f*x + 0.5f*y : 0.3f*x + 0.6f*y;
                assert(fabsf(output[i].lane[3] - expected) < 0.00001f);
            }
        }
    }
    assert(!isaac_light_halo_emit(output, 7, emitted, 12, input, 4, indices, 6, 512));
    assert(!isaac_light_halo_emit(output, 8, emitted, 11, input, 4, indices, 6, 512));
    input[0].lane[7] = 415.0f / 512.0f;
    assert(!isaac_light_halo_input_is_exact(input, 4, indices, 6, 512));
    input[0].lane[7] = 416.0f / 512.0f;
    uint32_t nan_bits = 0x7fc00000u;
    memcpy(&input[0].lane[0], &nan_bits, sizeof nan_bits);
    assert(!isaac_light_halo_input_is_exact(input, 4, indices, 6, 512));
    input[0].lane[0] = 0.0f;
    input[3].lane[13] = 0.001f;
    assert(!isaac_light_halo_input_is_exact(input, 4, indices, 6, 512));
    input[3].lane[13] = 0.0f;
    indices[5] = 4;
    assert(!isaac_light_halo_input_is_exact(input, 4, indices, 6, 512));
    indices[5] = 3;
    isaac_light_halo_vertex original[4];
    memcpy(original, input, sizeof original);
    for (unsigned premul = 0; premul < 2u; ++premul) {
        uint32_t *rgba = laser_rgba_palette(512, premul ?
            isaac_laser_p8_premul_linear : isaac_laser_p8_palette);
        texture tex = laser_empty();
        tex.min_filter = tex.mag_filter = SCE_GXM_TEXTURE_FILTER_LINEAR;
        allocations = 0; fail_at = gxm_fail_at = 0;
        assert(laser_upload(&tex, 512, rgba));
        uint32_t actual_premul = 99u;
        assert(isaacLaserLightHaloAtlasInfo(&tex, &actual_premul) == 512u);
        assert(actual_premul == premul);
        for (unsigned failed = 1; failed <= (TEST_LIGHT_PRODUCER_ENABLED ? 1u : 2u); ++failed) {
            IsaacLightHaloDraw submission = {.primitive = SCE_GXM_PRIMITIVE_TRIANGLES};
            void *destination = NULL; uint32_t vertices = 4;
            halo_fail_at = failed;
            assert(!isaac_light_halo_try_stage(&tex, 0, input, 4, indices, 6,
                &submission, &destination, &vertices));
            assert(!submission.indices && !destination && vertices == 4u);
            halo_pool_retire();
        }
        /* Exercise the ACTUAL caller at both depth states. ON changes only
         * admission: the emitted vertices/indices and native texture remain
         * byte-identical to the depth-OFF case. OFF retains the original veto. */
        isaac_light_halo_vertex expected_vertices[8];
        uint16_t expected_indices[12];
        assert(isaac_light_halo_emit(expected_vertices, 8, expected_indices, 12,
            input, 4, indices, 6, 512));
        for (int depth = 1; depth >= 0; --depth) {
            IsaacLightHaloDraw depth_submission = {.primitive = SCE_GXM_PRIMITIVE_TRIANGLES};
            void *depth_destination = NULL;
            uint32_t depth_vertices = 4;
            texture unchanged = tex;
            depth_test_state = depth;
            int expected = !depth;
#if defined(HAVE_ISAAC_LASER_LIGHT_HALO_DEPTH_APPROX) && HAVE_ISAAC_LASER_LIGHT_HALO_DEPTH_APPROX == 1
            expected = 1;
#endif
            assert(isaac_light_halo_try_stage(&tex, 0, input, 4, indices, 6,
                &depth_submission, &depth_destination, &depth_vertices) == expected);
            assert(depth_test_state == depth && memcmp(&tex, &unchanged, sizeof tex) == 0);
            if (expected) {
                assert(depth_vertices == 8u && depth_submission.count == 12);
                assert(memcmp(depth_destination, expected_vertices, sizeof expected_vertices) == 0);
                assert(memcmp(depth_submission.indices, expected_indices, sizeof expected_indices) == 0);
            } else {
                assert(!depth_submission.indices && !depth_destination && depth_vertices == 4u);
                assert(halo_allocations == 0u);
            }
            halo_pool_retire();
        }
        IsaacLightHaloDraw submission = {.primitive = SCE_GXM_PRIMITIVE_TRIANGLES};
        void *destination = NULL; uint32_t vertices = 4;
#if defined(HAVE_ISAAC_LASER_LIGHT_HALO_DEPTH_APPROX) && HAVE_ISAAC_LASER_LIGHT_HALO_DEPTH_APPROX == 1
        depth_test_state = 1; /* Other guards and nearest still apply with depth ON. */
#else
        depth_test_state = 0;
#endif
        stencil_test_state = 1;
        assert(!isaac_light_halo_try_stage(&tex, 0, input, 4, indices, 6,
            &submission, &destination, &vertices));
        stencil_test_state = 0; polygon_mode_back = 999u;
        assert(!isaac_light_halo_try_stage(&tex, 0, input, 4, indices, 6,
            &submission, &destination, &vertices));
        polygon_mode_back = SCE_GXM_POLYGON_MODE_TRIANGLE_FILL;
        samplers[0] = (void *)(uintptr_t)1;
        assert(!isaac_light_halo_try_stage(&tex, 0, input, 4, indices, 6,
            &submission, &destination, &vertices));
        samplers[0] = NULL; tex.use_mips = 1;
        assert(!isaac_light_halo_try_stage(&tex, 0, input, 4, indices, 6,
            &submission, &destination, &vertices));
        tex.use_mips = 0; tex.min_filter = 999u;
        assert(!isaac_light_halo_try_stage(&tex, 0, input, 4, indices, 6,
            &submission, &destination, &vertices));
        tex.min_filter = SCE_GXM_TEXTURE_FILTER_LINEAR;
        assert(halo_allocations == 0u && !submission.indices && !destination);
        blend_sfactor_rgb = SCE_GXM_BLEND_FACTOR_ONE;
        int accepted = isaac_light_halo_try_stage(&tex, 0, input, 4, indices, 6,
            &submission, &destination, &vertices);
        assert(accepted == (int)premul);
        if (accepted) assert(vertices == 8u && submission.count == 12);
#ifdef HAVE_ISAAC_LASER_LIGHT_NEAREST
        if (accepted) {
            assert(submission.nearest_ready);
            assert(memcmp(&submission.nearest_original, &tex.gxm_tex, sizeof tex.gxm_tex) == 0);
            SceGxmTexture expected = tex.gxm_tex;
            expected.min_filter = expected.mag_filter = SCE_GXM_TEXTURE_FILTER_POINT;
            assert(memcmp(&submission.nearest_point, &expected, sizeof expected) == 0);
            for (unsigned scenario = 0; scenario < 5u; ++scenario) {
                SceGxmContext context = 1;
                memset(&isaac_light_nearest_state, 0, sizeof isaac_light_nearest_state);
                nearest_bound = tex.gxm_tex;
                nearest_bind_calls = nearest_draw_calls = 0;
                nearest_bind_fail_mask = scenario == 1 ? 2u :
                    scenario == 2 ? 6u : scenario == 3 ? 4u : 0u;
                nearest_draw_result = scenario == 4 ? -9 : 0;
                isaac_light_halo_draw(&context, &submission);
                assert(nearest_bind_calls == 2u);
                assert(nearest_draw_calls == (scenario == 2 ? 0u : 1u));
                assert(isaac_light_nearest_state.logged_selection == (scenario == 0));
                assert(isaac_light_nearest_state.disabled == (scenario >= 1 && scenario <= 3));
                assert(memcmp(&tex.gxm_tex, &submission.nearest_original, sizeof tex.gxm_tex) == 0);
                if (scenario != 2 && scenario != 3)
                    assert(memcmp(&nearest_bound, &tex.gxm_tex, sizeof nearest_bound) == 0);
                if (scenario == 1)
                    assert(memcmp(&nearest_drawn, &tex.gxm_tex, sizeof nearest_drawn) == 0);
            }
            memset(&isaac_light_nearest_state, 0, sizeof isaac_light_nearest_state);
        }
#endif
        halo_pool_retire();
#ifdef HAVE_ISAAC_LASER_LIGHT_NEAREST
        if (premul) {
            texture unchanged = tex;
            for (unsigned failure = 1; failure <= 2; ++failure) {
                gxm_fail_at = gxm_calls + failure;
                submission.nearest_ready = GL_FALSE;
                assert(isaac_light_halo_try_stage(&tex, 0, input, 4, indices, 6,
                    &submission, &destination, &vertices));
                assert(!submission.nearest_ready);
                assert(memcmp(&tex, &unchanged, sizeof tex) == 0);
                halo_pool_retire();
            }
            gxm_fail_at = 0;
            tex.min_filter = tex.mag_filter = SCE_GXM_TEXTURE_FILTER_POINT;
            tex.gxm_tex.min_filter = tex.gxm_tex.mag_filter = SCE_GXM_TEXTURE_FILTER_POINT;
            assert(isaac_light_halo_try_stage(&tex, 0, input, 4, indices, 6,
                &submission, &destination, &vertices));
            assert(!submission.nearest_ready);
            halo_pool_retire();
        }
#endif
        assert(memcmp(original, input, sizeof original) == 0);
        depth_test_state = 0;
        blend_sfactor_rgb = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
        gpu_free_texture_data(&tex);
        assert(!isaacLaserLightHaloAtlasInfo(&tex, &actual_premul));
        free(rgba);
    }
    puts("Light halo: per-triangle interpolation/seam/winding/area, both widths, unchanged input, exact UV, finite/plain guards, reachable OOM, depth/stencil/wireframe/sampler/mips, raw-vs-premul ONE, delete identity PASS");
    assert(halo_success_lines == 1u);
#if defined(HAVE_ISAAC_LASER_LIGHT_HALO_DEPTH_APPROX) && HAVE_ISAAC_LASER_LIGHT_HALO_DEPTH_APPROX == 1
    assert(strstr(halo_success_line, " depth-approx=1 depth-test=1\n"));
    puts("Light halo depth approximation ON: depth-enabled admission, unchanged retained geometry/texture/state, existing guard fallbacks, bounded truthful marker PASS; fringe depth coverage is intentionally removed");
#else
    assert(!strstr(halo_success_line, "depth-approx"));
    assert(strstr(halo_success_line, TEST_LIGHT_PRODUCER_ENABLED ?
        " crop=416..448 keep=421..444 producer-uv=1 combined=1\n" :
        " crop=416..448 keep=421..444\n"));
    puts("Light halo depth approximation OFF: original depth veto and opt-in-only producer marker PASS");
#endif
    assert((strstr(halo_success_line, " producer-uv=1 combined=1") != NULL) ==
        TEST_LIGHT_PRODUCER_ENABLED);
#ifdef HAVE_ISAAC_LASER_LIGHT_NEAREST
    puts("Light nearest native caller MOCK: checked descriptor, POINT bind/draw/restore, failed bind baseline fallback, failed restore/failed draw truthful marker, no semantic mutation PASS; GXM hardware NOT executed");
#endif
}

static void test_light_producer_uv(void)
{
    static const uint32_t expected_bits[2][2] = {
        {0x3f6db6dcu, 0x3f800000u}, {0x3f500000u, 0x3f600000u}
    };
    const uint32_t *palettes[] = {isaac_laser_p8_palette,
        isaac_laser_p8_premul_linear, isaac_laser_p8_premul_gamma};
    for (unsigned width = 448; width <= 512; width += 64)
    for (unsigned palette = 0; palette < 3u; ++palette) {
        uint32_t *rgba = laser_rgba_palette(width, palettes[palette]);
        texture tex = laser_empty();
        tex.min_filter = tex.mag_filter = SCE_GXM_TEXTURE_FILTER_LINEAR;
        allocations = 0; fail_at = gxm_fail_at = 0;
        assert(laser_upload(&tex, width, rgba));
        for (unsigned mirror = 0; mirror < 2u; ++mirror)
        for (unsigned reverse = 0; reverse < 2u; ++reverse) {
            isaac_light_halo_vertex original[4] = {0}, expected_output[8];
            uint16_t original_indices[] = {0, 2, 1, 1, 2, 3}, expected_indices[12];
            for (unsigned i = 0; i < 4u; ++i) {
                original[i].lane[0] = (float)(i & 1u) * 32.0f;
                original[i].lane[1] = (float)(i >> 1u) * 64.0f;
                original[i].lane[2] = (float)(i * i) * 0.1f;
                for (unsigned lane = 3; lane <= 6; ++lane)
                    original[i].lane[lane] = (float)(i * i + lane) / 16.0f;
                original[i].lane[9] = (float)i * 0.1f;
                original[i].lane[10] = (float)(i * i) * 0.1f;
                original[i].lane[11] = 1.0f;
                original[i].lane[21] = -(float)(i + 1u);
                original[i].lane[7] = laser_atlas_producer_u(416u + ((i ^ mirror) & 1u)*32u, width);
                original[i].lane[8] = (float)(i >> 1u);
                uint32_t bits;
                memcpy(&bits, &original[i].lane[7], sizeof bits);
                assert(bits == expected_bits[width == 512u][(i ^ mirror) & 1u]);
            }
            if (reverse) for (unsigned t = 0; t < 6u; t += 3u) {
                uint16_t swap = original_indices[t];
                original_indices[t] = original_indices[t+1u]; original_indices[t+1u] = swap;
            }
            int admitted = TEST_LIGHT_PRODUCER_ENABLED || width == 512u;
            /* Restoring direct division MUST fail this assertion at width448. */
            assert(isaac_light_halo_input_is_exact(original, 4, original_indices, 6, width) == admitted);
            if (TEST_LIGHT_PRODUCER_ENABLED && width == 448u) {
                isaac_light_halo_vertex divided[4];
                memcpy(divided, original, sizeof divided);
                const uint32_t old_left = 0x3f6db6dbu;
                for (unsigned i = 0; i < 4u; ++i)
                    if (((i ^ mirror) & 1u) == 0u)
                        memcpy(&divided[i].lane[7], &old_left, sizeof old_left);
                assert(!isaac_light_halo_input_is_exact(divided, 4, original_indices, 6, width));
            }
            if (admitted)
                assert(isaac_light_halo_emit(expected_output, 8, expected_indices, 12,
                    original, 4, original_indices, 6, width));
            for (unsigned scenario = 0; scenario < 17u; ++scenario) {
                isaac_light_halo_vertex input[4];
                uint16_t indices[6];
                memcpy(input, original, sizeof input);
                memcpy(indices, original_indices, sizeof indices);
                int expected = admitted;
                switch (scenario) {
                case 1: case 2: case 3: {
                    unsigned vertex = scenario == 3 ? (mirror ^ 1u) : mirror;
                    uint32_t bits;
                    memcpy(&bits, &input[vertex].lane[7], sizeof bits);
                    bits += scenario == 2 ? UINT32_MAX : 1u;
                    memcpy(&input[vertex].lane[7], &bits, sizeof bits);
                    expected = 0; break;
                }
                case 4: input[0].lane[7] = laser_atlas_producer_u(352, width); expected = 0; break;
                case 5: input[1].lane[7] = input[0].lane[7]; expected = 0; break;
                case 6: input[3].lane[13] = 0.01f; expected = 0; break;
                case 7: halo_fail_at = 1; expected = 0; break;
                case 8: halo_fail_at = 2; expected = admitted && TEST_LIGHT_PRODUCER_ENABLED; break;
                case 9: case 10:
                    halo_cpu_recovery_at = scenario == 9 ? 1u : 2u;
                    halo_attach_pending = halo_dirty_fb = GL_TRUE;
                    halo_recovery_reset();
                    assert(!is_fbo_float && halo_fb_float && halo_dirty_fb);
                    if (scenario == 9 && TEST_LIGHT_PRODUCER_ENABLED) expected = 0;
                    break;
                case 11: halo_mutate_vertices = input; halo_mutate_indices = indices; break;
                case 16:
#if defined(HAVE_ISAAC_LASER_HALO_PROVEN_EMIT) && HAVE_ISAAC_LASER_HALO_PROVEN_EMIT == 1
                    halo_force_emit_failure = 1u; expected = 0;
#endif
                    break;
                default:
                    if (scenario >= 12u && TEST_LIGHT_PRODUCER_ENABLED) {
                        halo_gc_attack = 1; halo_gc_mark = scenario - 12u;
                        halo_gc_clean = (halo_gc_mark + 1u) % 4u;
                    }
                    break;
                }
                IsaacLightHaloDraw submission = {.primitive = SCE_GXM_PRIMITIVE_TRIANGLES};
                void *destination = NULL; uint32_t vertices = 4u;
                texture unchanged = tex;
                int actual = isaac_light_halo_try_stage(&tex, 0, input, 4, indices, 6,
                    &submission, &destination, &vertices);
                assert(memcmp(&tex, &unchanged, sizeof tex) == 0);
                if (actual != expected)
                    fprintf(stderr, "body width=%u palette=%u mirror=%u reverse=%u scenario=%u\n",
                        width, palette, mirror, reverse, scenario);
                assert(actual == expected);
#if defined(HAVE_ISAAC_LASER_HALO_PROVEN_EMIT) && HAVE_ISAAC_LASER_HALO_PROVEN_EMIT == 1
                assert(halo_proven_calls == (unsigned)(admitted &&
                    !(scenario >= 1u && scenario <= 7u) && scenario != 9u));
#endif
                if (actual) {
                    assert(vertices == 8u && submission.count == 12);
                    assert(memcmp(destination, expected_output, sizeof expected_output) == 0);
                    assert(memcmp(submission.indices, expected_indices, sizeof expected_indices) == 0);
                    if (scenario == 11) {
                        isaac_light_halo_vertex rejected_output[8];
                        uint16_t rejected_indices[12];
                        assert(!isaac_light_halo_emit(rejected_output, 8, rejected_indices, 12,
                            input, 4, indices, 6, width)); /* public API still checks mutated input */
                    }
                    assert(halo_allocations == (TEST_LIGHT_PRODUCER_ENABLED ? 1u : 2u));
                    if (TEST_LIGHT_PRODUCER_ENABLED) {
                        assert(!halo_gc_retired_first && halo_gc_visits == 0u);
                        assert((uint8_t *)submission.indices == (uint8_t *)destination + sizeof expected_output);
                        assert(sizeof expected_output % 32u == 0u);
                        assert((uintptr_t)submission.indices % _Alignof(uint16_t) == 0u);
                        assert(halo_pool_bytes[0] == sizeof expected_output + sizeof expected_indices);
                        if (scenario == 10) assert(!is_fbo_float && halo_recovery_resets == 1u);
                    }
                } else {
                    assert(!destination && !submission.indices && !submission.count && vertices == 4u);
                    if (!admitted || (scenario >= 1u && scenario <= 6u)) assert(!halo_allocations);
                    if (scenario == 9 && admitted)
                        assert(is_fbo_float && halo_recovery_resets == 2u);
                }
                halo_pool_retire();
            }
        }
        gpu_free_texture_data(&tex); free(rgba);
    }
    puts(TEST_LIGHT_PRODUCER_ENABLED ?
        "Light producer UV ON: actual f32 bits448/512, mirrors/windings, adjacent ULP and mixed-cap rejection; one reservation/all4 GC phases, lazy-float successful recovery veto, no second reservation, immutable snapshot PASS (host only)" :
        "Light producer UV OFF: width448 actual producer rejected, width512 old admission/two reservations preserved; caps independently controlled PASS (host only)");
}

/* This API remains checked even when native try_stage uses proven emission.
 * All failures here precede output writes in the original checked contract. */
static void test_halo_checked_emit_rejects(void)
{
    for (unsigned invalid = 0; invalid < 15u; ++invalid) {
        isaac_light_halo_vertex input[4] = {0}, output[8], before[8];
        uint16_t indices[] = {0, 2, 1, 1, 2, 3}, emitted[12], before_indices[12];
        for (unsigned i = 0; i < 4u; ++i) {
            input[i].lane[0] = (float)(i & 1u) * 32.0f;
            input[i].lane[1] = (float)(i >> 1u) * 64.0f;
            for (unsigned lane = 3; lane <= 6; ++lane) input[i].lane[lane] = 1.0f;
            input[i].lane[7] = (416.0f + (float)(i & 1u) * 32.0f) / 512.0f;
        }
        isaac_light_halo_vertex *out = output;
        uint16_t *out_indices = emitted;
        const isaac_light_halo_vertex *in = input;
        const uint16_t *in_indices = indices;
        uint32_t vc = 8u, ic = 12u, vertices = 4u, count = 6u, width = 512u;
        switch (invalid) {
        case 0: out = NULL; break;
        case 1: out_indices = NULL; break;
        case 2: in = NULL; break;
        case 3: in_indices = NULL; break;
        case 4: vc = 7u; break;
        case 5: ic = 11u; break;
        case 6: vertices = 0u; break;
        case 7: count = 5u; break;
        case 8: width = 449u; break;
        case 9: { uint32_t nan = 0x7fc00000u;
            memcpy(&input[3].lane[0], &nan, sizeof nan); break; }
        case 10: input[3].lane[13] = 0.01f; break;
        case 11: input[3].lane[6] = 1.01f; break;
        case 12: input[0].lane[7] = 415.0f / 512.0f; break;
        case 13: indices[5] = 4u; break;
        case 14: indices[2] = indices[0]; break;
        }
        memset(output, 0xa5, sizeof output); memcpy(before, output, sizeof before);
        memset(emitted, 0x5a, sizeof emitted); memcpy(before_indices, emitted, sizeof before_indices);
        assert(!isaac_light_halo_emit(out, vc, out_indices, ic,
            in, vertices, in_indices, count, width));
        assert(!memcmp(output, before, sizeof output));
        assert(!memcmp(emitted, before_indices, sizeof emitted));
    }
    puts("Halo checked emitter: null/count/capacity/width/NaN/plain/color/UV/index failures remain checked and unpublished PASS");
}

static void test_red_cap_reservation_lifetime(void)
{
#if defined(HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP) && HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP == 1
    uint32_t *rgba = laser_rgba(512);
    texture tex = laser_empty();
    tex.min_filter = tex.mag_filter = SCE_GXM_TEXTURE_FILTER_POINT;
    allocations = 0; fail_at = gxm_fail_at = 0;
    assert(laser_upload(&tex, 512, rgba));
    isaac_light_halo_vertex input[4] = {0};
    uint16_t indices[] = {0, 2, 1, 1, 2, 3};
    for (unsigned i = 0; i < 4u; ++i) {
        input[i].lane[0] = (float)(i & 1u) * 32.0f;
        input[i].lane[1] = (float)(i >> 1u) * 32.0f;
        for (unsigned lane = 3; lane <= 6; ++lane) input[i].lane[lane] = 1.0f;
        input[i].lane[7] = laser_atlas_producer_u(320u + (i & 1u)*32u, 512);
        input[i].lane[8] = (float)(i >> 1u) * 0.5f;
    }
    for (unsigned bucket = 0; bucket < 4u; ++bucket) {
        /* Historical two-reservation chronology reproduces the retirement
         * for EVERY phase, not a free merely because Finish was called. */
        halo_gc_attack = 1;
        halo_gc_mark = bucket; halo_gc_clean = (bucket + 1u) % 4u;
        void *first = gpu_alloc_mapped_temp(8u * sizeof input[0]);
        assert(first && !halo_gc_retired_first && halo_gc_pending[bucket] == first);
        assert(gpu_alloc_mapped_temp(12u * sizeof(uint16_t)));
        assert(halo_gc_visits == 4u && halo_gc_retired_first == first);
        halo_pool_retire();

        halo_gc_attack = 1;
        halo_gc_mark = bucket; halo_gc_clean = (bucket + 1u) % 4u;
        IsaacLightHaloDraw submission = {.primitive = SCE_GXM_PRIMITIVE_TRIANGLES};
        void *destination = NULL;
        uint32_t vertices = 4u;
        assert(isaac_light_halo_try_stage(&tex, 0, input, 4, indices, 6,
            &submission, &destination, &vertices));
        /* This exact assertion rejects a restored two-reservation candidate,
         * even if its output bytes happen to survive the logical free. */
        assert(!halo_gc_retired_first);
        assert(halo_allocations == 1u && halo_gc_visits == 0u);
        assert(halo_gc_pending[bucket] == destination);
        assert(vertices == 8u && submission.count == 12);
        size_t vertex_bytes = (size_t)vertices * sizeof input[0];
        assert(vertex_bytes % 32u == 0u);
        assert((uint8_t *)submission.indices == (uint8_t *)destination + vertex_bytes);
        assert((uintptr_t)submission.indices % _Alignof(uint16_t) == 0u);
        assert(halo_pool_bytes[0] == vertex_bytes + 12u * sizeof(uint16_t));
        halo_pool_retire();
    }
    gpu_free_texture_data(&tex);
    free(rgba);
    puts("Red cap reservation: historical two-call markP/cleanP+1..P retirement reproduced for4 phases; actual cap one-call publication/live buffer/index-tail alignment PASS (quarantined host free, not SDK)");
#endif
}

static void test_red_cap_side(void)
{
    const uint32_t *palettes[] = {isaac_laser_p8_palette,
        isaac_laser_p8_premul_gamma, isaac_laser_p8_premul_linear};
    int enabled = 0;
#if defined(HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP) && HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP == 1
    enabled = 1;
#endif
    for (unsigned width = 448; width <= 512; width += 64)
    for (unsigned palette = 0; palette < 3u; ++palette) {
        uint32_t *rgba = laser_rgba_palette(width, palettes[palette]);
        texture tex = laser_empty();
        tex.min_filter = tex.mag_filter = SCE_GXM_TEXTURE_FILTER_POINT;
        allocations = 0; fail_at = gxm_fail_at = 0;
        assert(laser_upload(&tex, width, rgba));
        for (unsigned crop = 320; crop <= 352; crop += 32)
        for (unsigned row = 0; row <= 32; row += 32)
        for (unsigned mirror = 0; mirror < 4u; ++mirror)
        for (unsigned reverse = 0; reverse < 2u; ++reverse) {
            isaac_light_halo_vertex input[4] = {0}, output[8];
            uint16_t indices[] = {0, 2, 1, 1, 2, 3}, emitted[12];
            for (unsigned i = 0; i < 4u; ++i) {
                input[i].lane[0] = (float)(i & 1u) * 32.0f;
                input[i].lane[1] = (float)(i >> 1u) * 32.0f;
                input[i].lane[2] = (float)(i * i) * 0.1f; /* non-affine Z */
                for (unsigned lane = 3; lane <= 6; ++lane)
                    input[i].lane[lane] = (float)(i * i + lane) / 16.0f;
                input[i].lane[7] = laser_atlas_producer_u(crop + ((i ^ mirror) & 1u)*32u, width);
                input[i].lane[8] = (float)(row + ((i ^ mirror) >> 1u)*32u) / 64.0f;
                input[i].lane[9] = (float)i * 0.1f;
                input[i].lane[10] = (float)(i * i) * 0.1f;
                input[i].lane[11] = 1.0f;
                input[i].lane[21] = -(float)(i + 1u);
            }
            if (reverse) for (unsigned t = 0; t < 6u; t += 3u) {
                uint16_t swap = indices[t]; indices[t] = indices[t + 1u];
                indices[t + 1u] = swap;
            }
            assert(isaac_light_halo_input_is_exact(input, 4, indices, 6, width) == enabled);
            assert(isaac_light_halo_emit(output, 8, emitted, 12,
                input, 4, indices, 6, width) == enabled);
            if (enabled) {
                float area = 0.0f;
                for (unsigned i = 0; i < 8u; ++i) {
                    float x = output[i].lane[0] / 32.0f;
                    float y = output[i].lane[1] / 32.0f;
                    assert(output[i].lane[0] >= 2.9999f && output[i].lane[0] <= 29.0001f);
                    for (unsigned lane = 0; lane < 22u; ++lane) {
                        float a = input[0].lane[lane], b = input[1].lane[lane];
                        float c = input[2].lane[lane], d = input[3].lane[lane];
                        float expected = i < 4u ? a + (b-a)*x + (c-a)*y :
                            b*(1.0f-y) + c*(1.0f-x) + d*(x+y-1.0f);
                        assert(fabsf(output[i].lane[lane] - expected) < 0.0001f);
                    }
                }
                for (unsigned i = 0; i < 12u; i += 3u) {
                    float *a = output[emitted[i]].lane, *b = output[emitted[i+1]].lane;
                    float *c = output[emitted[i+2]].lane;
                    float cross = (b[0]-a[0])*(c[1]-a[1]) - (b[1]-a[1])*(c[0]-a[0]);
                    assert(reverse ? cross > 0.0f : cross < 0.0f);
                    area += fabsf(cross) * 0.5f;
                }
                assert(fabsf(area - 26.0f*32.0f) < 0.01f);
                assert(!isaac_light_halo_emit(output, 7, emitted, 12, input, 4, indices, 6, width));
                assert(!isaac_light_halo_emit(output, 8, emitted, 11, input, 4, indices, 6, width));
            }
            texture unchanged = tex;
            isaac_light_halo_vertex original[4];
            memcpy(original, input, sizeof original);
            for (unsigned scenario = 0; scenario <= 28u; ++scenario) {
                IsaacLightHaloDraw submission = {.primitive = SCE_GXM_PRIMITIVE_TRIANGLES};
                void *destination = NULL;
                uint32_t vertices = 4u;
                tex = unchanged;
                memcpy(input, original, sizeof input);
                blend_sfactor_rgb = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
                switch (scenario) {
                case 1: tex.min_filter = SCE_GXM_TEXTURE_FILTER_LINEAR; break;
                case 2: tex.mag_filter = SCE_GXM_TEXTURE_FILTER_LINEAR; break;
                case 3: tex.gxm_tex.min_filter = SCE_GXM_TEXTURE_FILTER_LINEAR; break;
                case 4: tex.gxm_tex.mag_filter = SCE_GXM_TEXTURE_FILTER_LINEAR; break;
                case 5: tex.use_mips = 1; break;
                case 6: tex.overridden = 1; break;
                case 7: samplers[0] = (void *)(uintptr_t)1; break;
                case 8: stencil_test_state = 1; break;
                case 9: halo_fail_at = 1; break;
                case 10: halo_fail_at = 2; break;
                case 11: input[3].lane[7] = laser_atlas_producer_u(crop + 64u, width); break;
                case 12: input[3].lane[8] += 0.25f; break;
                case 13: input[3].lane[13] = 0.01f; break;
                case 14: input[3].lane[12] = 0.01f; break;
                case 15: { uint32_t nan = 0x7fc00000u;
                    memcpy(&input[3].lane[0], &nan, sizeof nan); break; }
                case 16: input[3].lane[6] = 1.01f; break;
                case 17: blend_sfactor_rgb = SCE_GXM_BLEND_FACTOR_ONE; break;
                case 18: depth_test_state = 1; break;
                case 19: tex.gxm_tex.format = SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR; break;
                case 20: input[0].lane[7] += 0.001f; break;
                case 21:
                    halo_cpu_recovery_at = 1u;
                    halo_attach_pending = halo_dirty_fb = GL_TRUE;
                    halo_recovery_reset();
                    assert(!is_fbo_float && halo_fb_float && halo_dirty_fb);
                    break;
                case 22: halo_cpu_recovery_at = 2u; break; /* must never be reached */
                case 23: { uint32_t bits;
                    memcpy(&bits, &input[0].lane[7], sizeof bits); ++bits;
                    memcpy(&input[0].lane[7], &bits, sizeof bits); break; }
                case 24: { uint32_t bits;
                    memcpy(&bits, &input[3].lane[7], sizeof bits); --bits;
                    memcpy(&input[3].lane[7], &bits, sizeof bits); break; }
                case 25: input[0].lane[7] = 416.0f / (float)width; break;
                case 26:
                    for (unsigned i = 0; i < 4u; ++i) input[i].lane[8] = 0.5f;
                    break;
                case 27:
                    for (unsigned i = 0; i < 4u; ++i) input[i].lane[7] = input[0].lane[7];
                    break;
                case 28: halo_mutate_vertices = input; halo_mutate_indices = indices; break;
                }
                int expected = enabled && (scenario == 0 || scenario == 10 ||
                    scenario == 22 || scenario == 28 ||
                    (scenario == 17 && palette != 0));
#if defined(HAVE_ISAAC_LASER_LIGHT_HALO_DEPTH_APPROX) && HAVE_ISAAC_LASER_LIGHT_HALO_DEPTH_APPROX == 1
                if (scenario == 18) expected = enabled;
#endif
                texture guarded = tex;
                int actual = isaac_light_halo_try_stage(&tex, 0, input, 4, indices, 6,
                    &submission, &destination, &vertices);
                if (actual != expected)
                    fprintf(stderr, "cap case width=%u palette=%u crop=%u row=%u mirror=%u reverse=%u scenario=%u\n",
                        width, palette, crop, row, mirror, reverse, scenario);
                assert(actual == expected);
                assert(memcmp(&tex, &guarded, sizeof tex) == 0);
                if (expected) {
                    assert(vertices == 8u && submission.count == 12);
                    assert(halo_allocations == 1u);
                    assert((uint8_t *)submission.indices == (uint8_t *)destination + sizeof output);
                    assert(halo_pool_bytes[0] == sizeof output + sizeof emitted);
                    if (scenario == 22) assert(!is_fbo_float && halo_recovery_resets == 0u);
                    assert(memcmp(destination, output, sizeof output) == 0);
                    assert(memcmp(submission.indices, emitted, sizeof emitted) == 0);
                    if (scenario == 28) {
                        isaac_light_halo_vertex rejected_output[8];
                        uint16_t rejected_indices[12];
                        assert(memcmp(input, original, sizeof input) != 0);
                        assert(!isaac_light_halo_emit(rejected_output, 8, rejected_indices, 12,
                            input, 4, indices, 6, width));
                    }
#ifdef HAVE_ISAAC_LASER_LIGHT_NEAREST
                    assert(!submission.nearest_ready);
                    SceGxmContext context = 1;
                    nearest_bind_calls = nearest_draw_calls = nearest_bind_fail_mask = 0;
                    nearest_draw_result = 0;
                    isaac_light_halo_draw(&context, &submission);
                    assert(nearest_bind_calls == 0u && nearest_draw_calls == 1u);
#endif
                } else {
                    assert(!destination && !submission.indices && vertices == 4u);
                    if (scenario != 9 && scenario != 10 && scenario != 21 && scenario != 22)
                        assert(halo_allocations == 0u);
                    if (enabled && scenario == 21)
                        assert(is_fbo_float && halo_recovery_resets == 2u);
                }
                halo_pool_retire();
                samplers[0] = NULL;
                stencil_test_state = depth_test_state = 0;
            }
            tex = unchanged;
        }
        blend_sfactor_rgb = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
        gpu_free_texture_data(&tex);
        free(rgba);
    }
    assert(cap_success_lines == (unsigned)enabled);
    if (enabled)
        puts("Red cap side ON: exact producer UV, four crops/two widths/three palettes/mirrors/windings/all22 interpolants, ULP/mixed-crop rejection, POINT semantic+descriptor, state/PLAIN/live-P8, combined OOM/float recovery veto, no second reservation, immutable snapshot and no nearest override PASS (host only)");
    else
        puts("Red cap side OFF: every cap rejected without allocation/publication; previous light-body cases and marker retained PASS (host only)");
}

static void test_exact_laser(void)
{
    uint32_t *rgba = laser_rgba(448), *padded = laser_rgba(512);
    texture tex = laser_empty(), before = tex;
    assert(isaac_laser_p8_match(rgba, 448, 64, 0));
    assert(isaac_laser_p8_match(padded, 512, 64, 512));
    assert(!isaac_laser_p8_match(rgba, 447, 64, 0));
    assert(!isaac_laser_p8_match(rgba, 448, 63, 0));
    assert(!isaac_laser_p8_match(rgba, 448, 64, 512));
    const unsigned positions[] = {0, 16384, 448 * 64 - 1};
    for (unsigned i = 0; i < sizeof positions / sizeof positions[0]; ++i) {
        rgba[positions[i]] ^= 1U; /* Includes RGB under alpha zero. */
        assert(!isaac_laser_p8_match(rgba, 448, 64, 0));
        rgba[positions[i]] ^= 1U;
    }
    padded[511] = 0x00ffffff;
    assert(!isaac_laser_p8_match(padded, 512, 64, 0));
    padded[511] = 0;
    padded[512 * 64 - 1] = 1;
    assert(!isaac_laser_p8_match(padded, 512, 64, 0));
    padded[512 * 64 - 1] = 0;
    unsigned retired_before = retired;
    for (unsigned failure = 1; failure <= 3; ++failure) {
        fail_at = failure; allocations = 0;
        assert(!laser_upload(&tex, 448, rgba));
        assert(memcmp(&tex, &before, sizeof tex) == 0);
        assert(!isaac_laser_p8_find(&tex) && retired == retired_before);
    }
    fail_at = 0;
    for (unsigned failure = 1; failure <= 2; ++failure) {
        gxm_fail_at = failure; gxm_calls = 0; allocations = 0;
        unsigned freed_before = immediate_frees;
        assert(!laser_upload(&tex, 448, rgba));
        assert(memcmp(&tex, &before, sizeof tex) == 0);
        assert(allocations == 3 && immediate_frees == freed_before + 3);
        assert(!isaac_laser_p8_find(&tex) && retired == retired_before);
    }
    gxm_fail_at = 0;
    fail_at = 0; allocations = 0;
    tex.ref_counter = 1; assert(!laser_upload(&tex, 448, rgba));
    tex = before; tex.overridden = 1; assert(!laser_upload(&tex, 448, rgba));
    tex = before; tex.use_mips = 1; assert(!laser_upload(&tex, 448, rgba));
    tex = before; tex.faces_counter = 1; assert(!laser_upload(&tex, 448, rgba));
    tex = before; tex.status = TEX_VALID; assert(!laser_upload(&tex, 448, rgba));
    tex = before; tex.data = (void *)(uintptr_t)1; assert(!laser_upload(&tex, 448, rgba));
    tex = before; tex.palette_data = (void *)(uintptr_t)1; assert(!laser_upload(&tex, 448, rgba));
    tex = before;
    assert(!isaac_laser_p8_try_upload(&tex, GL_TEXTURE_2D, 1, GL_RGBA,
        448, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba, 0));
    assert(!isaac_laser_p8_try_upload(&tex, GL_TEXTURE_2D, 0, GL_RGBA,
        448, 64, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba, 0));
    assert(allocations == 0);
    texture_slots[1] = before;
    texture_units[0].tex_id[0] = 1;
    samplers[0] = (void *)(uintptr_t)1;
    assert(!laser_upload(&texture_slots[1], 448, rgba));
    samplers[0] = NULL;

    assert(laser_upload(&tex, 448, rgba));
    assert(allocations ==
#ifdef HAVE_ISAAC_LASER_P8_SWIZZLE
        4 &&
#else
        3 &&
#endif
        tex.gxm_tex.mip_count == 0);
    for (unsigned i = 0; i < 448U * 64U; ++i)
        assert(laser_pixel(&tex, i) == rgba[i]);
    uint32_t *backup = isaac_laser_p8_find(&tex)->rgba;
    assert(memcmp(backup, rgba, 448U * 64U * 4U) == 0);
    tex.last_frame = 123;
    SceGxmTexture descriptor_before = tex.gxm_tex;
    isaac_laser_p8_parameter(&tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    isaac_laser_p8_parameter(&tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    assert(isaac_laser_p8_find(&tex));
    assert(memcmp(&descriptor_before, &tex.gxm_tex, sizeof descriptor_before) == 0);
    tex.min_filter = 27; tex.mag_filter = 28; tex.u_mode = 29;
    allocations = 0; fail_at = 1;
    isaacLaserP8Restore(&tex); /* Attach/mutation/mip/pointer escape seam. */
    assert(allocations == 0 && tex.data == backup && retired_frame == 123);
    assert(retired == retired_before + 2 && !isaac_laser_p8_find(&tex));
    assert(tex.palette_data == NULL && tex.gxm_tex.palette == NULL);
    assert(tex.gxm_tex.min_filter == 27 && tex.gxm_tex.mag_filter == 28);
    assert(tex.gxm_tex.u_mode == 29 && tex.gxm_tex.mip_count == 0);
    assert(memcmp(tex.data, rgba, 448U * 64U * 4U) == 0);
    isaacLaserP8Restore(&tex); /* Idempotent, no second retirement. */
    assert(retired == retired_before + 2);
    write_rgba8888(tex.data, 0x11223344U);
    assert(((uint32_t *)tex.data)[0] == 0x11223344U);
    gpu_free_texture_data(&tex);

    fail_at = 0;
    texture second = before, third = before;
    tex = before;
    assert(laser_upload(&tex, 512, padded));
    assert(laser_upload(&second, 448, rgba));
    allocations = 0;
    assert(!laser_upload(&third, 448, rgba) && allocations == 0);
    for (unsigned i = 0; i < 512U * 64U; ++i)
        assert(laser_pixel(&tex, i) == padded[i]);
    tex.last_frame = 456;
    unsigned immediate_before = immediate_frees;
    gpu_free_texture_data(&tex); /* Ordinary delete also drops the backup. */
    assert(immediate_frees == immediate_before + 1 && retired_frame == 456);
    assert(!isaac_laser_p8_find(&tex));
    tex = before; /* The same native slot can now be reused, with no metadata. */
    assert(laser_upload(&tex, 448, rgba));
    isaac_laser_p8_parameter(&tex, GL_TEXTURE_LOD_BIAS, 1);
    assert(!isaac_laser_p8_find(&tex) && tex.palette_data == NULL);
    gpu_free_texture_data(&tex);
    gpu_free_texture_data(&second);
    assert(!isaac_laser_p8_find(&second));

    texture_slots[2] = before;
    allocations = 0;
    isaac_laser_p8_escape(&texture_slots[2]); /* Descriptor before first upload. */
    assert(!laser_upload(&texture_slots[2], 448, rgba) && allocations == 0);
    assert(memcmp(&texture_slots[2], &before, sizeof before) == 0);
    texture_slots[2] = before; /* Slot reuse cannot revoke an escaped pointer. */
    assert(!laser_upload(&texture_slots[2], 448, rgba) && allocations == 0);
    texture_slots[3] = before;
    assert(laser_upload(&texture_slots[3], 448, rgba));
    isaac_laser_p8_escape(&texture_slots[3]);
    assert(!isaac_laser_p8_find(&texture_slots[3]));
    assert(memcmp(texture_slots[3].data, rgba, 448U * 64U * 4U) == 0);
    gpu_free_texture_data(&texture_slots[3]);
    texture_slots[3] = before;
    assert(!laser_upload(&texture_slots[3], 448, rgba));

    const uint32_t *variants[] = {isaac_laser_p8_palette,
        isaac_laser_p8_premul_gamma, isaac_laser_p8_premul_linear};
    for (unsigned variant = 0; variant < 3; ++variant) {
        for (unsigned width = 448; width <= 512; width += 64) {
            uint32_t *pixels = laser_rgba_palette(width, variants[variant]);
            assert(isaac_laser_p8_match(pixels, width, 64, 0) == (int)variant + 1);
            tex = before;
            assert(laser_upload(&tex, width, pixels));
            for (unsigned i = 0; i < width * 64U; ++i)
                assert(laser_pixel(&tex, i) == pixels[i]);
            isaacLaserP8Restore(&tex);
            assert(memcmp(tex.data, pixels, width * 64U * 4U) == 0);
            gpu_free_texture_data(&tex);
            pixels[0] ^= 1U;
            assert(!isaac_laser_p8_match(pixels, width, 64, 0));
            free(pixels);
        }
    }
    free(rgba); free(padded);
    puts("Exact laser P8: raw/two frozen premultiply variants, all pixels/padding, OOM1/2/3, GXM refusal1/2, sampler, no-alloc restore, early pointer escape, delete/reuse PASS");
}

static void check_sampler(const texture *t)
{
    assert(t->gxm_tex.u_mode == t->u_mode);
    assert(t->gxm_tex.v_mode == t->v_mode);
    assert(t->gxm_tex.min_filter == t->min_filter);
    assert(t->gxm_tex.mag_filter == t->mag_filter);
    assert(t->gxm_tex.mip_filter == t->mip_filter);
    assert(t->gxm_tex.lod_bias == t->lod_bias);
    assert(t->gxm_tex.mip_count == 1);
}

#ifdef HAVE_ISAAC_LASER_P8_SWIZZLE
static void test_exact_laser_swizzle(void)
{
    for (unsigned width = 448; width <= 512; width += 64) {
        uint32_t *rgba = laser_rgba(width);
        for (unsigned failure = 0; failure <= 9; ++failure) {
            texture tex = laser_empty();
            unsigned freed_before = immediate_frees;
            unsigned calls_before = swizzle_calls;
            allocations = gxm_calls = 0;
            /* Fourth allocation or any of nine layout-specific GXM checks. */
            fail_at = failure == 0 ? 4U : 0U;
            gxm_fail_at = failure == 0 ? 0U : failure + 2U;
            assert(laser_upload(&tex, width, rgba));
            assert(tex.gxm_tex.layout == SCE_GXM_TEXTURE_LINEAR);
            assert(tex.gxm_tex.width == width && tex.gxm_tex.height == 64);
            assert(isaac_laser_p8_find(&tex) && allocations == 4U);
            assert(immediate_frees == freed_before + (failure != 0));
            assert(swizzle_calls == calls_before + (failure != 0));
            for (unsigned i = 0; i < width * 64U; ++i)
                assert(laser_pixel(&tex, i) == rgba[i]);
            gpu_free_texture_data(&tex);
        }
        texture tex = laser_empty();
        allocations = gxm_calls = fail_at = gxm_fail_at = 0;
        assert(laser_upload(&tex, width, rgba));
        assert(tex.gxm_tex.layout == (width == 448 ?
            SCE_GXM_TEXTURE_SWIZZLED_ARBITRARY : SCE_GXM_TEXTURE_SWIZZLED));
        assert(tex.gxm_tex.width == width && tex.gxm_tex.height == 64);
        assert(tex.gxm_tex.palette == tex.palette_data && tex.gxm_tex.data == tex.data);
        for (unsigned i = 0; i < width * 64U; ++i)
            assert(laser_pixel(&tex, i) == rgba[i]);
        if (width == 448)
            for (unsigned i = width * 64; i < 512U * 64U; ++i)
                assert(((uint8_t *)tex.data)[i] == 187);
        uint32_t premul;
        assert(isaacLaserLightHaloAtlasInfo(&tex, &premul) == width);
        uint32_t *backup = isaac_laser_p8_find(&tex)->rgba;
        tex.last_frame = 901;
        allocations = 0; fail_at = 1;
        isaacLaserP8Restore(&tex);
        assert(allocations == 0 && retired_frame == 901 && tex.data == backup);
        assert(tex.gxm_tex.layout == SCE_GXM_TEXTURE_LINEAR);
        assert(memcmp(tex.data, rgba, width * 64U * 4U) == 0);
        gpu_free_texture_data(&tex);

        const GLenum names[] = {GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER,
            GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T};
        const GLint values[] = {GL_LINEAR, GL_NEAREST, GL_REPEAT, GL_CLAMP};
        for (unsigned setter = 0; setter < 4; ++setter)
            for (unsigned failure = 1; failure <= (setter == 0 ? 3U : 1U); ++failure) {
                tex = laser_empty();
                allocations = gxm_calls = fail_at = gxm_fail_at = 0;
                assert(laser_upload(&tex, width, rgba));
                backup = isaac_laser_p8_find(&tex)->rgba;
                allocations = 0; fail_at = 1;
                gxm_fail_at = gxm_calls + failure;
                isaac_laser_p8_parameter(&tex, names[setter], values[setter]);
                assert(!isaac_laser_p8_find(&tex) && tex.data == backup);
                assert(allocations == 0 && tex.palette_data == NULL);
                assert(memcmp(tex.data, rgba, width * 64U * 4U) == 0);
                gpu_free_texture_data(&tex);
            }
        tex = laser_empty();
        allocations = gxm_calls = fail_at = gxm_fail_at = 0;
        assert(laser_upload(&tex, width, rgba));
        SceGxmTexture descriptor_before = tex.gxm_tex;
        isaac_laser_p8_parameter(&tex, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT);
        if (width == 448) {
            assert(!isaac_laser_p8_find(&tex)); /* Mock native arbitrary-mode refusal. */
            assert(memcmp(tex.data, rgba, width * 64U * 4U) == 0);
        } else {
            assert(isaac_laser_p8_find(&tex));
            assert(memcmp(&descriptor_before, &tex.gxm_tex, sizeof descriptor_before) == 0);
        }
        gpu_free_texture_data(&tex);
        free(rgba);
    }
    fail_at = gxm_fail_at = 0;
    puts("Laser P8 swizzle CALLER MOCK: both logical widths, allocation/native refusal fallback, index/palette/padding preservation, checked sampler copy, no-alloc restore/retirement PASS; pinned NEON converter and hardware sampling NOT executed");
}
#endif

#if defined(HAVE_ISAAC_LASER_HALO_PROFILE) && HAVE_ISAAC_LASER_HALO_PROFILE
static void test_halo_profile(void)
{
    uint32_t *rgba = laser_rgba_palette(512u, isaac_laser_p8_palette);
    texture tex = laser_empty();
    allocations = fail_at = gxm_fail_at = 0u;
    assert(laser_upload(&tex, 512u, rgba));
    vglIsaacLaserHaloStats original = isaac_halo_stats, untouched;
    memset(&untouched, 0xa5, sizeof untouched);
    vglIsaacLaserHaloStats sentinel = untouched;
    assert(!vglGetIsaacLaserHaloStats(NULL, ISAAC_LASER_HALO_STATS_WORDS));
    assert(!vglGetIsaacLaserHaloStats(&untouched, ISAAC_LASER_HALO_STATS_WORDS - 1u));
    assert(!memcmp(&untouched, &sentinel, sizeof untouched));
    assert(!memcmp(&original, &isaac_halo_stats, sizeof original));
    struct { uint32_t prefix[11]; uint32_t canary[3]; } legacy;
    memset(&legacy, 0xa5, sizeof legacy);
    assert(vglGetIsaacLaserHaloStats((vglIsaacLaserHaloStats *)&legacy, 11u));
    assert(legacy.prefix[0] == 1u && !memcmp(legacy.prefix+1, (uint32_t *)&original+1, 40u));
    for (unsigned i=0;i<3;++i) assert(legacy.canary[i] == 0xa5a5a5a5u);
    assert(vglGetIsaacLaserHaloStats(&untouched, 14u) && untouched.abi_version == 2u);
    assert(!memcmp(&original, &untouched, sizeof original));
    assert(!memcmp(&original, &isaac_halo_stats, sizeof original)); /* both reads nonconsuming */
#if defined(HAVE_ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION) && HAVE_ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION
    assert(untouched.white_enabled == 1u);
    isaac_halo_stats.ordinary_plain_requests=UINT32_MAX;
    isaac_halo_stats.ordinary_white_requests=UINT32_MAX;
    isaac_white_profile_commit(0u); isaac_white_profile_commit(99u);
    assert(isaac_halo_stats.ordinary_plain_requests==UINT32_MAX);
    isaac_white_profile_commit(2u);
    assert(isaac_halo_stats.ordinary_plain_requests==0u && isaac_halo_stats.ordinary_white_requests==0u);
    isaac_white_profile_commit(1u);
    assert(isaac_halo_stats.ordinary_plain_requests==1u && isaac_halo_stats.ordinary_white_requests==0u);
    isaac_halo_stats=original;
#else
    assert(untouched.white_enabled == 0u);
#endif
    for (unsigned scenario = 0u; scenario < 18u; ++scenario) {
        isaac_light_halo_vertex input[4] = {0};
        uint16_t indices[6] = {0,2,1,1,2,3};
        for (unsigned i=0; i<4u; ++i) {
            input[i].lane[0] = (float)(i&1u)*32.0f;
            input[i].lane[1] = (float)(i>>1u)*64.0f;
            for (unsigned lane=3; lane<=6; ++lane) input[i].lane[lane]=1.0f;
            input[i].lane[7] = (416.0f + (float)(i&1u)*32.0f)/512.0f;
            input[i].lane[8] = (float)(i>>1u);
        }
        IsaacLightHaloDraw submission = {.primitive = SCE_GXM_PRIMITIVE_TRIANGLES};
        IsaacLightHaloDraw *submit = &submission;
        void *destination = NULL;
        const void *source = input;
        texture *selected = &tex, unowned = laser_empty();
        uint32_t vertices = 4u, new_vertices = 4u;
        GLsizei count = 6;
        uint32_t reason = ISAAC_HALO_LIGHT;
        tex.min_filter = tex.mag_filter = SCE_GXM_TEXTURE_FILTER_LINEAR;
        tex.gxm_tex.min_filter = tex.gxm_tex.mag_filter = SCE_GXM_TEXTURE_FILTER_LINEAR;
        samplers[0] = NULL; gxm_fail_at=0u;
        switch (scenario) {
        case 0: submit=NULL; selected=(texture *)(uintptr_t)1; source=(void *)(uintptr_t)1; vertices=257u; reason=ISAAC_HALO_LIMITS; break;
        case 1: submission.primitive=999u; selected=(texture *)(uintptr_t)1; reason=ISAAC_HALO_LIMITS; break;
        case 2: vertices=257u; source=(void *)(uintptr_t)1; reason=ISAAC_HALO_TOO_BIG; break;
        case 3: count=769; source=(void *)(uintptr_t)1; reason=ISAAC_HALO_TOO_BIG; break;
        case 4: count=5; reason=ISAAC_HALO_LIMITS; break;
        case 5: samplers[0]=(void *)(uintptr_t)1; source=(void *)(uintptr_t)1; reason=ISAAC_HALO_STATE; break;
        case 6: selected=&unowned; source=(void *)(uintptr_t)1; reason=ISAAC_HALO_STATE; break;
        case 7: source=(void *)(UINTPTR_MAX-20u); reason=ISAAC_HALO_LIMITS; break;
        case 8: input[0].lane[13]=0.1f; reason=ISAAC_HALO_PLAIN; break;
        case 9: input[0].lane[7]=0.1f; reason=ISAAC_HALO_UV; break;
        case 10: indices[0]=2; reason=ISAAC_HALO_UV; break;
        case 11: halo_fail_at=1u; reason=ISAAC_HALO_OUTPUT; break;
        case 12: halo_fail_at=2u; reason=TEST_LIGHT_PRODUCER_ENABLED ? ISAAC_HALO_LIGHT : ISAAC_HALO_OUTPUT; break;
        case 14: case 15:
            for (unsigned i=0; i<4u; ++i) {
                input[i].lane[7]=laser_atlas_producer_u(320u+(i&1u)*32u,512u);
                input[i].lane[8]=(float)(i>>1u)*0.5f;
            }
            reason=ISAAC_HALO_UV;
#if defined(HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP) && HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP
            reason=ISAAC_HALO_STATE;
#endif
            if (scenario==14u) {
                tex.min_filter=tex.mag_filter=SCE_GXM_TEXTURE_FILTER_POINT;
                tex.gxm_tex.min_filter=tex.gxm_tex.mag_filter=SCE_GXM_TEXTURE_FILTER_POINT;
#if defined(HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP) && HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP
                reason=ISAAC_HALO_CAP;
#endif
            }
            break;
        case 16: gxm_fail_at=gxm_calls+1u; break; /* nearest failure is not staging failure */
        case 17:
            halo_cpu_recovery_at=1u;
            halo_attach_pending=halo_dirty_fb=GL_TRUE;
            halo_recovery_reset();
            reason=TEST_LIGHT_PRODUCER_ENABLED ? ISAAC_HALO_OUTPUT : ISAAC_HALO_LIGHT;
            break;
        }
        vglIsaacLaserHaloStats before, after;
        assert(vglGetIsaacLaserHaloStats(&before, ISAAC_LASER_HALO_STATS_WORDS));
        int staged=isaac_light_halo_try_stage(selected,0u,source,vertices,indices,count,
            submit,&destination,&new_vertices);
        assert(halo_last_reason==reason);
        assert(staged==(reason==ISAAC_HALO_LIGHT || reason==ISAAC_HALO_CAP));
        assert(vglGetIsaacLaserHaloStats(&after, ISAAC_LASER_HALO_STATS_WORDS));
        assert(after.attempts-before.attempts==1u);
        unsigned terminal=reason==ISAAC_HALO_TOO_BIG ? ISAAC_HALO_LIMITS : reason;
        for (unsigned i=0;i<ISAAC_HALO_OUTCOMES;++i)
            assert(after.outcome[i]-before.outcome[i]==(i==terminal));
        assert(after.too_big-before.too_big==(reason==ISAAC_HALO_TOO_BIG));
        if (!staged) assert(!destination && !submission.indices && new_vertices==4u);
        if (terminal<ISAAC_HALO_OUTPUT) assert(!halo_allocations);
        halo_pool_retire();
    }
    samplers[0]=NULL; gxm_fail_at=0u;
    /* Actual unsigned counter update and non-consuming snapshots across wrap. */
    isaac_halo_stats.attempts=UINT32_MAX;
    isaac_halo_stats.outcome[ISAAC_HALO_LIMITS]=UINT32_MAX;
    isaac_halo_stats.too_big=UINT32_MAX;
    isaac_halo_profile_commit(ISAAC_HALO_TOO_BIG);
    assert(isaac_halo_stats.attempts==0u && isaac_halo_stats.outcome[ISAAC_HALO_LIMITS]==0u && isaac_halo_stats.too_big==0u);
    isaac_halo_stats=original;
    gpu_free_texture_data(&tex); free(rgba);
    puts("Halo counters: 18 exact outcomes, poison early exits, size subset, original allocations/recovery, staged-not-nearest, endpoint/no-consume and modular wrap PASS");
}
#endif

int main(void)
{
    uint8_t packed[1024 + 256];
    uint32_t expected[256];
    texture tex;
    memset(&tex, 0, sizeof tex);
    tex.status = TEX_VALID;
    tex.last_frame = 77;
    tex.data = malloc(64);
    assert(tex.data);
    tex.u_mode = 3; tex.v_mode = 4; tex.min_filter = 5;
    tex.mag_filter = 6; tex.mip_filter = 7; tex.lod_bias = 8;
    for (unsigned i = 0; i < 256; ++i) {
        packed[4*i] = (uint8_t)i;
        packed[4*i+1] = (uint8_t)(i ^ 0xa5);
        packed[4*i+2] = (uint8_t)(255-i);
        packed[4*i+3] = (uint8_t)(37*i);
        packed[1024+i] = (uint8_t)i;
        expected[i] = read_rgba8888(packed + 4*i);
    }
    assert((expected[0] >> 24) == 0 && (expected[0] & 0xffffff) != 0);
    texture before = tex;
    for (unsigned failed = 1; failed <= 2; ++failed) {
        allocations = 0; fail_at = failed;
        assert(isaac_p8_image_level0(&tex, 16, 16, 0,
            sizeof packed, packed) == GL_OUT_OF_MEMORY);
        assert(memcmp(&before, &tex, sizeof tex) == 0);
        assert(retired == 0);
    }
    assert(immediate_frees == 1); /* The unpublished palette on index OOM. */
    fail_at = 0; allocations = 0;
    assert(isaac_p8_image_level0(&tex, 16, 16, 0,
        sizeof packed - 1, packed) == GL_INVALID_VALUE);
    assert(isaac_p8_image_level0(&tex, 15, 16, 0,
        sizeof packed, packed) == GL_INVALID_VALUE);
    tex.ref_counter = 1;
    assert(isaac_p8_image_level0(&tex, 16, 16, 0,
        sizeof packed, packed) == GL_INVALID_OPERATION);
    tex.ref_counter = 0;
    assert(allocations == 0 && memcmp(&before, &tex, sizeof tex) == 0);

    assert(isaac_p8_image_level0(&tex, 16, 16, 0,
        sizeof packed, packed) == GL_NO_ERROR);
    assert(retired == 1 && retired_frame == 77);
    assert(memcmp(tex.palette_data, expected, sizeof expected) == 0);
    assert(memcmp(tex.data, packed + 1024, 256) == 0);
    assert(tex.gxm_tex.palette == tex.palette_data);
    check_sampler(&tex);
    tex.last_frame = 88;
    before = tex;
    allocations = 0; fail_at = 1;
    assert(isaac_p8_promote_rgba_subimage(&tex, GL_TEXTURE_2D, 0,
        0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, packed, 0) == GL_OUT_OF_MEMORY);
    assert(memcmp(&before, &tex, sizeof tex) == 0 && retired == 1);
    fail_at = 0; allocations = 0;
    assert(isaac_p8_promote_rgba_subimage(&tex, GL_TEXTURE_2D, 0,
        INT32_MAX, 0, INT32_MAX, 1, GL_RGBA, GL_UNSIGNED_BYTE, packed, 0)
        == GL_INVALID_VALUE);
    assert(isaac_p8_promote_rgba_subimage(&tex, GL_TEXTURE_2D, 1,
        0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, packed, 0) == GL_INVALID_OPERATION);
    assert(isaac_p8_promote_rgba_subimage(&tex, GL_TEXTURE_2D, 0,
        0, 0, 0, 1, GL_RGBA, GL_UNSIGNED_BYTE, packed, 0) == GL_NO_ERROR);
    assert(allocations == 0 && memcmp(&before, &tex, sizeof tex) == 0);
    const uint32_t rectangle[] = { 0x12345678, 0xabcdef01, 0, 0x76543210,
        0x01020304, 0 };
    assert(isaac_p8_promote_rgba_subimage(&tex, GL_TEXTURE_2D, 0,
        2, 3, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, rectangle, 3) == GL_NO_ERROR);
    expected[3*16+2] = rectangle[0]; expected[3*16+3] = rectangle[1];
    expected[4*16+2] = rectangle[3]; expected[4*16+3] = rectangle[4];
    assert(retired == 3 && retired_frame == 88); /* Both old GPU buffers. */
    assert(tex.palette_data == NULL && tex.gxm_tex.palette == NULL);
    assert(memcmp(tex.data, expected, sizeof expected) == 0);
    assert(tex.format == SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    assert(tex.write_cb == write_rgba8888);
    check_sampler(&tex);
    assert(isaac_p8_image_level0(&tex, 16, 16, 0,
        sizeof packed, packed) == GL_NO_ERROR);
    const uint32_t alias_pixel = read_rgba8888((uint8_t *)tex.data + 10);
    assert(isaac_p8_promote_rgba_subimage(&tex, GL_TEXTURE_2D, 0,
        0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
        (uint8_t *)tex.data + 10, 0) == GL_NO_ERROR);
    assert(((uint32_t *)tex.data)[0] == alias_pixel);
    gpu_free_texture_data(&tex);
    puts("P8 helper: allocation failures, unchanged state, retirement, RGBA expansion PASS");
    test_exact_laser();
    test_light_halo();
    test_light_producer_uv();
    test_halo_checked_emit_rejects();
    test_red_cap_reservation_lifetime();
    test_red_cap_side();
#if defined(HAVE_ISAAC_LASER_HALO_PROFILE) && HAVE_ISAAC_LASER_HALO_PROFILE
    test_halo_profile();
#endif
#ifdef HAVE_ISAAC_LASER_ATLAS_NEAREST
    test_atlas_nearest();
#endif
#ifdef HAVE_ISAAC_LASER_P8_SWIZZLE
    test_exact_laser_swizzle();
#endif
    return 0;
}
