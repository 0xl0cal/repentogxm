/* Exact laser-atlas sampling experiment. Included by stock patch 0011 only.
 * The mapped RGBA backup is deliberate: every escape restores without an
 * allocation. This saves sampled bytes, NOT total allocated memory.
 */
#ifndef ISAAC_LASER_P8_H
#define ISAAC_LASER_P8_H
#if !defined(HAVE_ISAAC_P8_SAFE_UPLOAD) || defined(HAVE_TEX_CACHE)
#error "Exact laser P8 requires the checked P8 path and no file texture cache"
#endif
#include "isaac_laser_p8_reference.h"
#ifdef HAVE_ISAAC_LASER_P8_SWIZZLE
#ifndef SUPPORT_SMALL_FMT
#error "Laser P8 swizzle requires the pinned 8-bit texture converter"
#endif
#ifndef TEXTURE_SWIZZLER_H_
#include "utils/texture_swizzler.h"
#endif

/* Only called for the already matched, unpublished exact atlas. Keep the
 * linear P8 allocation/descriptor intact until every native check succeeds.
 * 448x64 uses arbitrary dimensions over a 512x64 swizzled allocation: changing
 * the descriptor width to 512 would change UVs, filtering and wrapping.
 */
static int isaac_laser_p8_try_swizzle(texture *staged, unsigned width)
{
    SceGxmTexture candidate;
    uint8_t *swizzled = gpu_alloc_mapped_for_gpu(512U * 64U);
    if (!swizzled)
        return 0;
    vgl_memset(swizzled, 187, 512U * 64U);
    /* Exact pinned vitaGL converter; distinct buffers, byte indices only.
     * tileSize=min(next_pow2(width), next_pow2(height))=64, not 8. */
    SwizzleTexData8Bpp(swizzled, (uint8_t *)staged->data,
        0, 0, width, 64, width, 64);
    int result = width == 512U ?
        sceGxmTextureInitSwizzled(&candidate, swizzled, staged->format,
            width, 64, 1) :
        sceGxmTextureInitSwizzledArbitrary(&candidate, swizzled, staged->format,
            width, 64, 1);
    if (result != 0 ||
            sceGxmTextureSetPalette(&candidate, staged->palette_data) != 0 ||
            sceGxmTextureSetUAddrMode(&candidate, staged->u_mode) != 0 ||
            sceGxmTextureSetVAddrMode(&candidate, staged->v_mode) != 0 ||
            sceGxmTextureSetMinFilter(&candidate, staged->min_filter) != 0 ||
            sceGxmTextureSetMagFilter(&candidate, staged->mag_filter) != 0 ||
            sceGxmTextureSetMipFilter(&candidate, staged->mip_filter) != 0 ||
            sceGxmTextureSetLodBias(&candidate, staged->lod_bias) != 0 ||
            sceGxmTextureSetMipmapCount(&candidate, 0) != 0) {
        vgl_free(swizzled);
        return 0;
    }
    vgl_free(staged->data); /* Unpublished linear indices, never submitted. */
    staged->data = swizzled;
    staged->gxm_tex = candidate;
    return 1;
}

/* Check the same value mapping as pinned textures.c on a descriptor copy.
 * Do not publish it: the original GL setter still owns semantic state and
 * error handling. Unsupported modes restore the existing RGBA backup first.
 */
static int isaac_laser_p8_swizzle_parameter_ok(texture *tex, GLenum pname,
        GLint param)
{
    SceGxmTexture candidate = tex->gxm_tex;
    if (pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER) {
        SceGxmTextureFilter filter;
        if (param != GL_NEAREST && param != GL_LINEAR)
            return 0;
        filter = param == GL_NEAREST ? SCE_GXM_TEXTURE_FILTER_POINT :
            SCE_GXM_TEXTURE_FILTER_LINEAR;
        if (pname == GL_TEXTURE_MAG_FILTER)
            return sceGxmTextureSetMagFilter(&candidate, filter) == 0;
        return sceGxmTextureSetMinFilter(&candidate, filter) == 0 &&
            sceGxmTextureSetMipFilter(&candidate,
                SCE_GXM_TEXTURE_MIP_FILTER_DISABLED) == 0 &&
            sceGxmTextureSetMipmapCount(&candidate, 1) == 0;
    }
    if (pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T) {
        SceGxmTextureAddrMode mode;
        switch (param) {
        case GL_CLAMP_TO_EDGE:
        case GL_CLAMP: mode = SCE_GXM_TEXTURE_ADDR_CLAMP; break;
        case GL_REPEAT: mode = SCE_GXM_TEXTURE_ADDR_REPEAT; break;
        case GL_MIRRORED_REPEAT: mode = SCE_GXM_TEXTURE_ADDR_MIRROR; break;
        case GL_MIRROR_CLAMP_EXT: mode = SCE_GXM_TEXTURE_ADDR_MIRROR_CLAMP; break;
        default: return 0;
        }
        return (pname == GL_TEXTURE_WRAP_S ?
            sceGxmTextureSetUAddrMode(&candidate, mode) :
            sceGxmTextureSetVAddrMode(&candidate, mode)) == 0;
    }
    return 0;
}
#endif

#define ISAAC_LASER_P8_SLOTS 2
typedef struct {
    texture *owner;
    uint32_t *rgba;
    uint32_t width;
#if defined(HAVE_ISAAC_LASER_LIGHT_HALO_CLIP) || defined(HAVE_ISAAC_LASER_ATLAS_NEAREST)
    uint32_t halo_premultiplied;
#endif
} isaac_laser_p8_record;
static isaac_laser_p8_record isaac_laser_p8_records[ISAAC_LASER_P8_SLOTS];
static uint32_t isaac_laser_p8_logs;
/* A descriptor can escape before the first upload. Never clear these bits:
 * an old descriptor pointer still addresses this native slot after reuse. */
static uint8_t isaac_laser_p8_escaped[(TEXTURES_NUM + 7U) / 8U];

static int isaac_laser_p8_escape_slot(texture *tex, int mark)
{
    uintptr_t first = (uintptr_t)&texture_slots[0];
    uintptr_t address = (uintptr_t)tex;
    if (address < first || address - first >= sizeof texture_slots ||
            (address - first) % sizeof(texture))
        return 0; /* Private callers always use native slots; host fixtures may not. */
    unsigned slot = (unsigned)((address - first) / sizeof(texture));
    unsigned bit = 1U << (slot & 7U);
    if (mark)
        isaac_laser_p8_escaped[slot >> 3] |= (uint8_t)bit;
    return (isaac_laser_p8_escaped[slot >> 3] & bit) != 0;
}

static isaac_laser_p8_record *isaac_laser_p8_find(texture *tex)
{
    for (unsigned i = 0; i < ISAAC_LASER_P8_SLOTS; ++i)
        if (isaac_laser_p8_records[i].owner == tex)
            return &isaac_laser_p8_records[i];
    return NULL;
}

#if defined(HAVE_ISAAC_LASER_LIGHT_HALO_CLIP) || defined(HAVE_ISAAC_LASER_ATLAS_NEAREST)
/* A private query, not a pointer escape. Ownership is invalidated by every
 * existing mutation/attachment/delete/reuse edge before native GL changes it. */
uint32_t isaacLaserLightHaloAtlasInfo(texture *tex, uint32_t *premultiplied)
{
    isaac_laser_p8_record *record;
    if (!tex || !premultiplied || tex->status != TEX_VALID ||
            !tex->data || !tex->palette_data ||
            vglGetTexFormat(&tex->gxm_tex) != SCE_GXM_TEXTURE_FORMAT_P8_ABGR)
        return 0;
    record = isaac_laser_p8_find(tex);
    if (!record || !record->rgba)
        return 0;
    *premultiplied = record->halo_premultiplied;
    return record->width;
}
#endif

/* Every canonical pixel and every accepted padding pixel is compared. No
 * hash collision, filename, dimensions-only or <=256-colour heuristic. */
static int isaac_laser_p8_match(const void *data, unsigned width,
    unsigned height, int row_length)
{
    if (!data || (width != 448U && width != 512U) || height != 64U ||
            (row_length != 0 && row_length != (int)width))
        return 0;
    const uint8_t *rgba = (const uint8_t *)data;
    unsigned pixel = 0;
    unsigned variants = 7U;
    for (unsigned run = 0; run < sizeof isaac_laser_p8_runs /
            sizeof isaac_laser_p8_runs[0]; ++run) {
        unsigned count = isaac_laser_p8_runs[run] >> 8;
        unsigned index = isaac_laser_p8_runs[run] & 255U;
        if (!count || index >= 187U || count > 448U * 64U - pixel)
            return 0;
        for (unsigned n = 0; n < count; ++n, ++pixel) {
            unsigned offset = (pixel / 448U) * width + pixel % 448U;
            uint32_t actual = read_rgba8888((void *)(rgba + 4U * offset));
            if (actual != isaac_laser_p8_palette[index])
                variants &= ~1U;
            if (actual != isaac_laser_p8_premul_gamma[index])
                variants &= ~2U;
            if (actual != isaac_laser_p8_premul_linear[index])
                variants &= ~4U;
            if (!variants)
                return 0;
        }
    }
    if (pixel != 448U * 64U)
        return 0;
    if (width == 512U)
        for (unsigned y = 0; y < 64U; ++y)
            for (unsigned x = 448U; x < 512U; ++x)
                if (read_rgba8888((void *)(rgba + 4U * (y * width + x))) != 0)
                    return 0;
    return (variants & 1U) ? 1 : (variants & 2U) ? 2 : 3;
}

static int isaac_laser_p8_has_sampler(texture *tex)
{
    for (unsigned unit = 0; unit < COMBINED_TEXTURE_IMAGE_UNITS_NUM; ++unit)
        if (samplers[unit]) {
            unsigned name = texture_units[unit].tex_id[0];
            if (name >= TEXTURES_NUM || &texture_slots[name] == tex)
                return 1;
        }
    return 0;
}

/* Return 1 only after all checked allocations and input reads succeeded.
 * A zero result leaves the entire texture and GL error unchanged, allowing
 * the original glTexImage2D call to consume its original pixels normally. */
static int isaac_laser_p8_try_upload(texture *tex, GLenum target, GLint level,
    GLint internal_format, GLsizei width, GLsizei height, GLint border,
    GLenum format, GLenum type, const void *data, GLint row_length)
{
    if (target != GL_TEXTURE_2D || level != 0 || internal_format != GL_RGBA ||
            format != GL_RGBA || type != GL_UNSIGNED_BYTE || border != 0 ||
            tex->status != TEX_UNINITIALIZED || tex->ref_counter != 0 ||
            tex->overridden || tex->use_mips || tex->faces_counter ||
            tex->data || tex->palette_data ||
            isaac_laser_p8_escape_slot(tex, 0) ||
            isaac_laser_p8_has_sampler(tex))
        return 0;
    int variant = isaac_laser_p8_match(data, (unsigned)width,
        (unsigned)height, row_length);
    if (!variant)
        return 0;
    isaac_laser_p8_record *record = NULL;
    for (unsigned i = 0; i < ISAAC_LASER_P8_SLOTS; ++i)
        if (!isaac_laser_p8_records[i].owner) {
            record = &isaac_laser_p8_records[i];
            break;
        }
    if (!record)
        return 0;

    const uint32_t pixels = (uint32_t)width * (uint32_t)height;
    uint32_t *backup = gpu_alloc_mapped_for_gpu(pixels * 4U);
    if (!backup)
        return 0;
    uint32_t *palette = gpu_alloc_mapped_aligned_for_gpu(
        SCE_GXM_PALETTE_ALIGNMENT, 256U * sizeof(uint32_t));
    if (!palette) {
        vgl_free(backup);
        return 0;
    }
    uint8_t *indices = gpu_alloc_mapped_for_gpu(pixels);
    if (!indices) {
        vgl_free(palette);
        vgl_free(backup);
        return 0;
    }
    vgl_fast_memcpy(backup, data, pixels * 4U);
    vgl_memset(palette, 0, 256U * sizeof(uint32_t));
    const uint32_t *colors = variant == 1 ? isaac_laser_p8_palette :
        variant == 2 ? isaac_laser_p8_premul_gamma : isaac_laser_p8_premul_linear;
    vgl_fast_memcpy(palette, colors,
        sizeof isaac_laser_p8_palette);
    vgl_memset(indices, 187, pixels); /* Checked, exactly-zero padded columns. */
    unsigned pixel = 0;
    for (unsigned run = 0; run < sizeof isaac_laser_p8_runs /
            sizeof isaac_laser_p8_runs[0]; ++run) {
        unsigned count = isaac_laser_p8_runs[run] >> 8;
        uint8_t index = (uint8_t)isaac_laser_p8_runs[run];
        for (unsigned n = 0; n < count; ++n, ++pixel)
            indices[(pixel / 448U) * (unsigned)width + pixel % 448U] = index;
    }

    texture staged = *tex;
    staged.data = indices;
    staged.palette_data = palette;
    staged.format = SCE_GXM_TEXTURE_FORMAT_P8_ABGR;
    staged.mip_count = 1;
    staged.write_cb = write_rgba8888; /* All writers restore before use. */
    /* This optional format must be accepted by the real GXM API before
     * publication, not merely written with vitaGL's unchecked bit setters. */
    if (sceGxmTextureInitLinear(&staged.gxm_tex, indices, staged.format,
            (uint32_t)width, (uint32_t)height, 1) != 0 ||
            sceGxmTextureSetPalette(&staged.gxm_tex, palette) != 0) {
        vgl_free(indices);
        vgl_free(palette);
        vgl_free(backup);
        return 0;
    }
    isaac_p8_apply_sampler(&staged);
    /* Match the ordinary _glTexImage2D_FlatIMPL no-mips descriptor state. */
    vglSetTexMipmapCount(&staged.gxm_tex, 0);
#ifdef HAVE_ISAAC_LASER_P8_SWIZZLE
    int swizzled = isaac_laser_p8_try_swizzle(&staged, (unsigned)width);
#endif
    staged.status = TEX_VALID;
#ifndef TEXTURES_SPEEDHACK
    staged.last_frame = OBJ_NOT_USED;
#endif
#ifdef HAVE_UNPURE_TEXTURES
    staged.mip_start = 0;
#endif
    record->rgba = backup;
    record->width = (uint32_t)width;
#if defined(HAVE_ISAAC_LASER_LIGHT_HALO_CLIP) || defined(HAVE_ISAAC_LASER_ATLAS_NEAREST)
    record->halo_premultiplied = variant != 1;
#endif
    record->owner = tex;
    *tex = staged;
    if (isaac_laser_p8_logs++ < 8U)
#ifdef HAVE_ISAAC_LASER_P8_SWIZZLE
        sceClibPrintf("Isaac laser P8: exact=%ux64 variant=%u sampled=%u rgba-backup=%u layout=%s\n",
            (unsigned)width, (unsigned)variant,
            (unsigned)((swizzled ? 512U * 64U : pixels) + 1024U),
            (unsigned)(pixels * 4U), swizzled ?
                (width == 512 ? "swizzled" : "swizzled-arbitrary") : "linear");
#else
        sceClibPrintf("Isaac laser P8: exact=%ux64 variant=%u sampled=%u rgba-backup=%u\n",
            (unsigned)width, (unsigned)variant, (unsigned)(pixels + 1024U),
            (unsigned)(pixels * 4U));
#endif
    return 1;
}

/* Called by gpu_free_texture_data before the ordinary native retirement.
 * Backup was never submitted to GXM: immediate release is correct. Clearing
 * the owner here also covers deletion, deferred FBO free and name reuse. */
void isaacLaserP8ReleaseBackup(texture *tex)
{
    isaac_laser_p8_record *record = isaac_laser_p8_find(tex);
    if (!record)
        return;
    uint32_t *backup = record->rgba;
    record->owner = NULL;
    record->rgba = NULL;
    record->width = 0;
    vgl_free(backup);
}

/* No allocation, no failure, no modified pixels. The P8 indices and palette
 * retain their actual old last_frame for GPU retirement. All semantic state
 * belongs to the live object; only its storage representation is restored. */
void isaacLaserP8Restore(texture *tex)
{
    isaac_laser_p8_record *record = isaac_laser_p8_find(tex);
    if (!record)
        return;
    uint32_t *backup = record->rgba;
    uint32_t width = record->width;
    record->owner = NULL; /* Detach before the common free hook. */
    record->rgba = NULL;
    record->width = 0;
    gpu_free_texture_data(tex);
    tex->data = backup;
    tex->format = SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR;
    tex->write_cb = write_rgba8888;
    tex->mip_count = 1;
    vglInitLinearTexture(&tex->gxm_tex, backup, tex->format, width, 64, 1);
    isaac_p8_apply_sampler(tex);
    vglSetTexMipmapCount(&tex->gxm_tex, 0);
#ifndef TEXTURES_SPEEDHACK
    tex->last_frame = OBJ_NOT_USED;
#endif
}

static void isaac_laser_p8_parameter(texture *tex, GLenum pname, GLint param)
{
#ifdef HAVE_ISAAC_LASER_P8_SWIZZLE
    if (isaac_laser_p8_find(tex) &&
            sceGxmTextureGetType(&tex->gxm_tex) != SCE_GXM_TEXTURE_LINEAR &&
            !isaac_laser_p8_swizzle_parameter_ok(tex, pname, param)) {
        isaacLaserP8Restore(tex);
        return;
    }
#endif
    if ((pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER) &&
            (param == GL_NEAREST || param == GL_LINEAR))
        return;
    if (pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T)
        return;
    isaacLaserP8Restore(tex); /* Mips/LOD/unexpected state stays ordinary. */
}

static void isaac_laser_p8_escape(texture *tex)
{
    isaac_laser_p8_escape_slot(tex, 1);
    isaacLaserP8Restore(tex);
}
#endif
