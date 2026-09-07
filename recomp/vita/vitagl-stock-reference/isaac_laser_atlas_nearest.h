#ifndef ISAAC_LASER_ATLAS_NEAREST_H
#define ISAAC_LASER_ATLAS_NEAREST_H

/* Included by custom_shaders.c only for the independent atlas quality option.
 * fragment_exact is THIS draw's existing immutable source/link-generation
 * proof. This does not classify/read vertices, require a plain selected FS,
 * alter geometry, or depend on successful light-halo clipping. */
static void isaac_laser_atlas_nearest_prepare(program *p,
        GLboolean fragment_exact, texture *tex, IsaacLightHaloDraw *submission)
{
    uint32_t unit, premultiplied;
    SceGxmTextureFilter min_filter, mag_filter;
    SceGxmTexture point;
    if (!submission || submission->primitive != SCE_GXM_PRIMITIVE_TRIANGLES ||
            !p || !fragment_exact || !p->isaac_coloroffset_link_vertex_exact ||
            p->max_frag_texunit_idx != 1u || p->max_vert_texunit_idx != 0u ||
            !p->frag_texunits[0] || p->frag_texunits[0]->type != UNIFORM_SAMPLER ||
            is_fbo_float || !tex)
        return;
    unit = (uint32_t)p->frag_texunits[0]->sampler_index;
    if (unit >= COMBINED_TEXTURE_IMAGE_UNITS_NUM || samplers[unit] ||
            tex->overridden || tex->use_mips || tex->mip_count != 1u ||
            !isaacLaserLightHaloAtlasInfo(tex, &premultiplied))
        return;
    /* Called AFTER the complete fragment/vertex sampler loops, uniforms and
     * streams. The live descriptor, not a semantic-cache approximation, is
     * copied; only its min/mag filters may differ for this one draw. Layout,
     * palette, dimensions, UV addressing and LOD remain untouched. */
    min_filter = sceGxmTextureGetMinFilter(&tex->gxm_tex);
    mag_filter = sceGxmTextureGetMagFilter(&tex->gxm_tex);
    if ((min_filter != SCE_GXM_TEXTURE_FILTER_POINT &&
         min_filter != SCE_GXM_TEXTURE_FILTER_LINEAR) ||
            (mag_filter != SCE_GXM_TEXTURE_FILTER_POINT &&
             mag_filter != SCE_GXM_TEXTURE_FILTER_LINEAR) ||
            (min_filter == SCE_GXM_TEXTURE_FILTER_POINT &&
             mag_filter == SCE_GXM_TEXTURE_FILTER_POINT) ||
            sceGxmTextureGetMipFilter(&tex->gxm_tex) != SCE_GXM_TEXTURE_MIP_FILTER_DISABLED)
        return;
    point = tex->gxm_tex;
    if (sceGxmTextureSetMinFilter(&point, SCE_GXM_TEXTURE_FILTER_POINT) != 0 ||
            sceGxmTextureSetMagFilter(&point, SCE_GXM_TEXTURE_FILTER_POINT) != 0)
        return;
    /* Atomic publication: failures preserve an independently admitted old
     * halo-nearest descriptor, if that separate option is also enabled. */
    submission->nearest_original = tex->gxm_tex;
    submission->nearest_point = point;
    submission->nearest_ready = GL_TRUE;
    submission->atlas_nearest_ready = GL_TRUE;
}
#endif
