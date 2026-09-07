/* Narrow prerequisites for a future exact P8 atlas experiment.
 * Included only by the optional stock-vitaGL patch 0010.  This does not choose
 * any game texture or convert an ordinary RGBA upload to P8.
 *
 * The supported upload is PALETTE8_RGBA8_OES, level 0, width aligned to eight,
 * no FBO references.  Mutation promotes owned P8 level 0 to RGBA8 and applies
 * the same RGBA/UNSIGNED_BYTE row copy as stock TexSubImage2D.  Other mutations fail
 * closed: no palette quantizer or generic mip/format policy is implied.
 */
#ifndef ISAAC_P8_TEXTURE_SAFETY_H
#define ISAAC_P8_TEXTURE_SAFETY_H

#if defined(HAVE_TEX_CACHE)
#error "Isaac P8 safety has no palette-aware file-cache implementation"
#endif

static void isaac_p8_apply_sampler(texture *tex)
{
    vglSetTexUMode(&tex->gxm_tex, tex->u_mode);
    vglSetTexVMode(&tex->gxm_tex, tex->v_mode);
    vglSetTexMinFilter(&tex->gxm_tex, tex->min_filter);
    vglSetTexMagFilter(&tex->gxm_tex, tex->mag_filter);
    vglSetTexMipFilter(&tex->gxm_tex, tex->mip_filter);
    vglSetTexLodBias(&tex->gxm_tex, tex->lod_bias);
    vglSetTexMipmapCount(&tex->gxm_tex, tex->use_mips ? tex->mip_count : 1);
}

static int isaac_p8_level0_shape(GLsizei width, GLsizei height)
{
    return width > 0 && height > 0 && width <= GXM_TEX_MAX_SIZE &&
        height <= GXM_TEX_MAX_SIZE && (width & 7) == 0;
}

static GLenum isaac_p8_image_level0(texture *tex, GLsizei width,
    GLsizei height, GLint border, GLsizei image_size, const void *data)
{
    if (!isaac_p8_level0_shape(width, height) || border != 0)
        return GL_INVALID_VALUE;
    if ((tex->status != TEX_UNINITIALIZED && tex->status != TEX_VALID) ||
            tex->ref_counter != 0 || tex->overridden)
        return GL_INVALID_OPERATION;
    const uint32_t pixel_count = (uint32_t)width * (uint32_t)height;
    if (image_size < 0 || (uint32_t)image_size != 1024U + pixel_count)
        return GL_INVALID_VALUE;

    uint32_t *palette = gpu_alloc_mapped_aligned_for_gpu(
        SCE_GXM_PALETTE_ALIGNMENT, 256U * sizeof(uint32_t));
    if (!palette)
        return GL_OUT_OF_MEMORY;
    uint8_t *indices = gpu_alloc_mapped_for_gpu(pixel_count);
    if (!indices) {
        vgl_free(palette); /* Not yet published to the GPU. */
        return GL_OUT_OF_MEMORY;
    }
    if (data) {
        const uint8_t *source = (const uint8_t *)data;
        for (uint32_t i = 0; i < 256U; ++i)
            palette[i] = read_rgba8888(source + 4U * i);
        vgl_fast_memcpy(indices, source + 1024U, pixel_count);
    } else {
        vgl_memset(palette, 0, 256U * sizeof(uint32_t));
        vgl_memset(indices, 0, pixel_count);
    }

    /* Both allocations and all input reads precede the only live-state
     * mutation.  gpu_free_texture_data retires the old indices AND palette
     * using their original last_frame; no stack/persistent palette aliases. */
    if (tex->status == TEX_VALID)
        gpu_free_texture_data(tex);
    tex->data = indices;
    tex->palette_data = palette;
    tex->format = SCE_GXM_TEXTURE_FORMAT_P8_ABGR;
    tex->mip_count = 1;
    vglInitLinearTexture(&tex->gxm_tex, indices, tex->format,
        (uint32_t)width, (uint32_t)height, 1);
    vglSetTexPalette(&tex->gxm_tex, palette);
    isaac_p8_apply_sampler(tex);
    tex->status = TEX_VALID;
#ifndef TEXTURES_SPEEDHACK
    tex->last_frame = OBJ_NOT_USED;
#endif
#ifdef HAVE_UNPURE_TEXTURES
    tex->mip_start = 0;
#endif
    return GL_NO_ERROR;
}

static GLenum isaac_p8_promote_rgba_subimage(texture *tex, GLenum target,
    GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
    GLenum format, GLenum type, const void *pixels, GLint row_length)
{
    uint32_t original_width, original_height;
    vglGetTexSizes(&tex->gxm_tex, &original_width, &original_height);
    if (target != GL_TEXTURE_2D || level != 0 ||
            format != GL_RGBA || type != GL_UNSIGNED_BYTE ||
            tex->status != TEX_VALID || tex->mip_count != 1 ||
            tex->ref_counter != 0 || tex->overridden ||
            !tex->palette_data || !tex->data ||
            !isaac_p8_level0_shape(original_width, original_height))
        return GL_INVALID_OPERATION;
    /* Subtraction after nonnegative/range checks avoids offset+size overflow. */
    if (xoffset < 0 || yoffset < 0 || width < 0 || height < 0 ||
            (uint32_t)xoffset > original_width ||
            (uint32_t)yoffset > original_height ||
            (uint32_t)width > original_width - (uint32_t)xoffset ||
            (uint32_t)height > original_height - (uint32_t)yoffset)
        return GL_INVALID_VALUE;
    if (width == 0 || height == 0)
        return GL_NO_ERROR;
    if (!pixels || row_length < 0)
        return GL_INVALID_VALUE;
    const uint64_t source_stride =
        (uint64_t)(row_length ? row_length : width) * sizeof(uint32_t);
    const uint64_t source_extent = (uint64_t)(height - 1) * source_stride +
        (uint64_t)width * sizeof(uint32_t);
    if (source_extent > SIZE_MAX)
        return GL_INVALID_VALUE;

    const uint32_t pixel_count = original_width * original_height;
    uint32_t *rgba = gpu_alloc_mapped_for_gpu(pixel_count * sizeof(uint32_t));
    if (!rgba)
        return GL_OUT_OF_MEMORY;
    const uint8_t *indices = (const uint8_t *)tex->data;
    const uint32_t *palette = (const uint32_t *)tex->palette_data;
    for (uint32_t i = 0; i < pixel_count; ++i)
        rgba[i] = palette[indices[i]]; /* Includes RGB under zero alpha. */
    /* Consume the incoming rectangle before retiring either old allocation:
     * a native caller can legally supply an alias into those old buffers. */
    for (GLsizei row = 0; row < height; ++row)
        vgl_fast_memcpy(rgba + ((uint32_t)yoffset + (uint32_t)row) *
                original_width + (uint32_t)xoffset,
            (const uint8_t *)pixels + (size_t)row * (size_t)source_stride,
            (size_t)width * sizeof(uint32_t));

    /* A fresh RGBA allocation avoids the stock P8 copy-on-write path, which
     * retires the palette while retaining its descriptor address.  Both old
     * allocations are instead retired together, with the old GPU-use age. */
    gpu_free_texture_data(tex);
    tex->data = rgba;
    tex->format = SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR;
    tex->write_cb = write_rgba8888;
    vglInitLinearTexture(&tex->gxm_tex, rgba, tex->format,
        original_width, original_height, 1);
    isaac_p8_apply_sampler(tex);
#ifndef TEXTURES_SPEEDHACK
    tex->last_frame = OBJ_NOT_USED;
#endif
    return GL_NO_ERROR;
}

#endif
