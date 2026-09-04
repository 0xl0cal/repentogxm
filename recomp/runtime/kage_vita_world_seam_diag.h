#ifndef KAGE_VITA_WORLD_SEAM_DIAG_H
#define KAGE_VITA_WORLD_SEAM_DIAG_H

#include <stdint.h>

#if defined(ISAAC_VITA_WORLD_SEAM_DIAG)

void kage_vita_world_seam_diag_reset(void);

void kage_vita_world_seam_diag_begin(
    uint32_t stage, uint32_t stage_type);
void kage_vita_world_seam_diag_post_room(void);
void kage_vita_world_seam_diag_post_lua(void);
void kage_vita_world_seam_diag_pre_hud(void);

void kage_vita_world_seam_diag_note_active_texture(uint32_t texture);
void kage_vita_world_seam_diag_note_bind_texture(
    uint32_t target, uint32_t texture);
void kage_vita_world_seam_diag_note_gen_texture(uint32_t texture);
void kage_vita_world_seam_diag_note_delete_texture(uint32_t texture);
void kage_vita_world_seam_diag_note_tex_image(
    uint32_t target, int32_t level, int32_t width, int32_t height);
void kage_vita_world_seam_diag_note_bind_framebuffer(
    uint32_t target, uint32_t framebuffer);
void kage_vita_world_seam_diag_note_delete_framebuffer(uint32_t framebuffer);
void kage_vita_world_seam_diag_note_framebuffer_texture(
    uint32_t target, uint32_t attachment, uint32_t texture_target,
    uint32_t texture, int32_t level);
void kage_vita_world_seam_diag_note_use_program(uint32_t program);
void kage_vita_world_seam_diag_note_viewport(
    int32_t x, int32_t y, int32_t width, int32_t height);
void kage_vita_world_seam_diag_note_clear_color(
    float red, float green, float blue, float alpha);
void kage_vita_world_seam_diag_note_clear(uint32_t mask);
void kage_vita_world_seam_diag_note_draw(void);

#else

#define kage_vita_world_seam_diag_reset() ((void)0)
#define kage_vita_world_seam_diag_note_active_texture(...) ((void)0)
#define kage_vita_world_seam_diag_note_bind_texture(...) ((void)0)
#define kage_vita_world_seam_diag_note_gen_texture(...) ((void)0)
#define kage_vita_world_seam_diag_note_delete_texture(...) ((void)0)
#define kage_vita_world_seam_diag_note_tex_image(...) ((void)0)
#define kage_vita_world_seam_diag_note_bind_framebuffer(...) ((void)0)
#define kage_vita_world_seam_diag_note_delete_framebuffer(...) ((void)0)
#define kage_vita_world_seam_diag_note_framebuffer_texture(...) ((void)0)
#define kage_vita_world_seam_diag_note_use_program(...) ((void)0)
#define kage_vita_world_seam_diag_note_viewport(...) ((void)0)
#define kage_vita_world_seam_diag_note_clear_color(...) ((void)0)
#define kage_vita_world_seam_diag_note_clear(...) ((void)0)
#define kage_vita_world_seam_diag_note_draw(...) ((void)0)

#endif

#endif
