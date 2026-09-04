/* Redistributable loading animation used by every clean source build.
 *
 * This is deliberately procedural: it contains no converted frame, sprite,
 * texture, or motion data from a third-party work.  Eight rectangles form a
 * simple activity ring whose highlighted cell advances with the cooperative
 * loading-animation cadence.
 *
 * Included by kage_vita_loading.c after kage_loading_fill_rect is defined.
 */
#ifndef KAGE_VITA_LOADING_FALLBACK_INC
#define KAGE_VITA_LOADING_FALLBACK_INC

#define KAGE_LOADING_ANIMATION_FRAME_COUNT 32u
#define KAGE_LOADING_PRESENTATION_TEXT "PROCEDURAL LOADER"

static const uint8_t s_loading_fallback_dot_x[8] = {
    26u, 35u, 39u, 35u, 26u, 17u, 13u, 17u
};
static const uint8_t s_loading_fallback_dot_y[8] = {
    7u, 11u, 20u, 29u, 33u, 29u, 20u, 11u
};

_Static_assert(KAGE_LOADING_ANIMATION_FRAME_COUNT == 32u,
               "procedural loading cadence must retain 32 frames");
_Static_assert(sizeof s_loading_fallback_dot_x ==
                   sizeof s_loading_fallback_dot_y,
               "procedural loading ring coordinates must stay paired");

static void kage_loading_draw_animation(uint32_t *framebuffer,
                                        unsigned frame_index,
                                        unsigned x, unsigned y,
                                        unsigned scale)
{
    unsigned phase;
    unsigned index;

    if (!framebuffer || !scale)
        return;
    /* All eight visible phases move at the measured 4.276 Hz presentation
     * cadence. The 32-frame snapshot range remains shared with the optional
     * sampled art. */
    phase = frame_index % 8u;
    for (index = 0u; index < 8u; ++index) {
        unsigned distance = (index + 8u - phase) % 8u;
        uint32_t color = KAGE_LOADING_TRACK;

        if (distance == 0u)
            color = KAGE_LOADING_FILL;
        else if (distance <= 2u)
            color = KAGE_LOADING_MUTED;
        kage_loading_fill_rect(
            framebuffer,
            x + (unsigned)s_loading_fallback_dot_x[index] * scale,
            y + (unsigned)s_loading_fallback_dot_y[index] * scale,
            4u * scale, 4u * scale, color);
    }
}

#endif
