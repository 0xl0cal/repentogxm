#ifndef ISAAC_LASER_LIGHT_NEAREST_H
#define ISAAC_LASER_LIGHT_NEAREST_H

/* Included only by draw.c with the companion option. The original descriptor
 * and checked POINT copy belong to this synchronous draw, not to the texture.
 * Halo staging or the separate exact-atlas option may populate nearest_ready. */
static struct {
    GLboolean disabled, logged_selection, logged_failure;
} isaac_light_nearest_state;

#ifdef HAVE_ISAAC_LASER_LIGHT_NEAREST
static void isaac_light_halo_draw(SceGxmContext *context,
        const IsaacLightHaloDraw *halo)
{
    int bind_result, draw_result, restore_result;
    if (!halo->nearest_ready || isaac_light_nearest_state.disabled) {
        sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES,
            SCE_GXM_INDEX_FORMAT_U16, halo->indices, halo->count);
        return;
    }
    bind_result = sceGxmSetFragmentTexture(context, 0, &halo->nearest_point);
    if (bind_result != 0) {
        isaac_light_nearest_state.disabled = GL_TRUE;
        restore_result = sceGxmSetFragmentTexture(context, 0,
            &halo->nearest_original);
        if (!isaac_light_nearest_state.logged_failure) {
            isaac_light_nearest_state.logged_failure = GL_TRUE;
            sceClibPrintf("Isaac light halo: nearest bind failed=%d baseline-bind=%d disabled=1\n",
                bind_result, restore_result);
        }
        /* A failed original bind cannot safely draw even the baseline. The
         * checked shadow wrapper invalidates its values on every bind error. */
        if (restore_result == 0)
            sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES,
                SCE_GXM_INDEX_FORMAT_U16, halo->indices, halo->count);
        return;
    }
    draw_result = sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES,
        SCE_GXM_INDEX_FORMAT_U16, halo->indices, halo->count);
    /* Restore even when sceGxmDraw fails. Never persist POINT semantic state. */
    restore_result = sceGxmSetFragmentTexture(context, 0,
        &halo->nearest_original);
    if (restore_result != 0) {
        isaac_light_nearest_state.disabled = GL_TRUE;
        if (!isaac_light_nearest_state.logged_failure) {
            isaac_light_nearest_state.logged_failure = GL_TRUE;
            sceClibPrintf("Isaac light halo: nearest restore failed=%d draw=%d disabled=1\n",
                restore_result, draw_result);
        }
    } else if (draw_result == 0 && !isaac_light_nearest_state.logged_selection) {
        isaac_light_nearest_state.logged_selection = GL_TRUE;
        sceClibPrintf("Isaac light halo: nearest=point applied=1 restored=1 indices=%u\n",
            (unsigned)halo->count);
    }
}
#endif

#ifdef HAVE_ISAAC_LASER_ATLAS_NEAREST
/* Indices here are the existing GPU-resident original draw indices unless
 * halo staging has explicitly supplied replacement geometry. Never submit the
 * guest/client source pointer. Both nearest paths share the disable latch. */
static void isaac_laser_atlas_draw(SceGxmContext *context,
        const IsaacLightHaloDraw *submission, SceGxmPrimitiveType primitive,
        const uint16_t *indices, GLsizei count)
{
    static GLboolean logged_atlas_selection;
    int bind_result, draw_result, restore_result;
    if (submission->indices) {
        indices = submission->indices;
        count = submission->count;
        primitive = SCE_GXM_PRIMITIVE_TRIANGLES;
    }
    if (!submission->atlas_nearest_ready || isaac_light_nearest_state.disabled) {
#ifdef HAVE_ISAAC_LASER_LIGHT_NEAREST
        if (submission->indices) {
            isaac_light_halo_draw(context, submission);
            return;
        }
#endif
        sceGxmDraw(context, primitive, SCE_GXM_INDEX_FORMAT_U16, indices, count);
        return;
    }
    bind_result = sceGxmSetFragmentTexture(context, 0, &submission->nearest_point);
    if (bind_result != 0) {
        isaac_light_nearest_state.disabled = GL_TRUE;
        restore_result = sceGxmSetFragmentTexture(context, 0, &submission->nearest_original);
        if (!isaac_light_nearest_state.logged_failure) {
            isaac_light_nearest_state.logged_failure = GL_TRUE;
            sceClibPrintf("Isaac laser atlas: nearest bind failed=%d baseline-bind=%d disabled=1\n",
                bind_result, restore_result);
        }
        if (restore_result == 0)
            sceGxmDraw(context, primitive, SCE_GXM_INDEX_FORMAT_U16, indices, count);
        return;
    }
    draw_result = sceGxmDraw(context, primitive, SCE_GXM_INDEX_FORMAT_U16, indices, count);
    restore_result = sceGxmSetFragmentTexture(context, 0, &submission->nearest_original);
    if (restore_result != 0) {
        isaac_light_nearest_state.disabled = GL_TRUE;
        if (!isaac_light_nearest_state.logged_failure) {
            isaac_light_nearest_state.logged_failure = GL_TRUE;
            sceClibPrintf("Isaac laser atlas: nearest restore failed=%d draw=%d disabled=1\n",
                restore_result, draw_result);
        }
    } else if (draw_result == 0 && !logged_atlas_selection) {
        logged_atlas_selection = GL_TRUE;
        sceClibPrintf("Isaac laser atlas: nearest=point applied=1 restored=1 indices=%u halo=%u\n",
            (unsigned)count, (unsigned)(submission->indices != NULL));
    }
}
#endif
#endif
