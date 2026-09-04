#include "kage_vita_world_seam_diag.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#if !defined(ISAAC_VITA_WORLD_SEAM_DIAG)
#error "world-seam implementation must only be compiled for its diagnostic"
#endif

#if defined(ISAAC_KAGE_VITA_WORLD_SEAM_DIAG_ORACLE)
int kage_vita_world_seam_diag_oracle_log(const char *format, ...);
#define WORLD_SEAM_LOG kage_vita_world_seam_diag_oracle_log
#else
void isaac_vita_log(const char *format, ...);
#define WORLD_SEAM_LOG isaac_vita_log
#endif

#ifndef ISAAC_VITA_WORLD_SEAM_DIAG_BUILD_ID
#define ISAAC_VITA_WORLD_SEAM_DIAG_BUILD_ID "world-seam:unstamped"
#endif

#define WORLD_SEAM_TEXTURE0            0x000084c0u
#define WORLD_SEAM_TEXTURE_2D          0x00000de1u
#define WORLD_SEAM_FRAMEBUFFER         0x00008d40u
#define WORLD_SEAM_READ_FRAMEBUFFER    0x00008ca8u
#define WORLD_SEAM_DRAW_FRAMEBUFFER    0x00008ca9u
#define WORLD_SEAM_COLOR_ATTACHMENT0   0x00008ce0u
#define WORLD_SEAM_TEXTURE_UNITS       16u
#define WORLD_SEAM_TEXTURE_CAPACITY    16384u
#define WORLD_SEAM_FRAMEBUFFER_RECORDS 64u
#define WORLD_SEAM_CONTROL_LIMIT       1u
#define WORLD_SEAM_TARGET_LIMIT        2u

typedef struct WorldSeamTexture {
    uint16_t width;
    uint16_t height;
    uint16_t generation;
    uint8_t defined;
    uint8_t reserved;
} WorldSeamTexture;

typedef struct WorldSeamFramebuffer {
    uint32_t name;
    uint32_t color_texture;
} WorldSeamFramebuffer;

typedef struct WorldSeamDraw {
    uint32_t framebuffer;
    uint32_t attachment;
    uint32_t program;
    uint32_t texture;
    uint16_t texture_generation;
    uint16_t texture_width;
    uint16_t texture_height;
    uint16_t reserved;
} WorldSeamDraw;

typedef struct WorldSeamSegment {
    uint16_t draws;
    uint16_t clears;
    uint16_t framebuffer_binds;
    uint16_t program_uses;
    uint16_t texture_binds;
    uint16_t viewport_sets;
    uint16_t attachments;
    uint32_t last_clear_framebuffer;
    uint32_t last_clear_mask;
    uint32_t last_clear_color_hash;
    WorldSeamDraw first_draw;
    WorldSeamDraw last_draw;
} WorldSeamSegment;

typedef struct WorldSeamState {
    WorldSeamTexture textures[WORLD_SEAM_TEXTURE_CAPACITY];
    WorldSeamFramebuffer framebuffers[WORLD_SEAM_FRAMEBUFFER_RECORDS];
    uint32_t bound_textures[WORLD_SEAM_TEXTURE_UNITS];
    uint32_t draw_framebuffer;
    uint32_t read_framebuffer;
    uint32_t program;
    uint32_t clear_rgba[4];
    int32_t viewport[4];
    uint32_t active_texture;
    uint32_t sequence;
    uint32_t stage;
    uint32_t stage_type;
    uint32_t control_receipts;
    uint32_t target_receipts;
    uint32_t tracking_bad;
    uint32_t active;
    uint32_t phase;
    uint32_t target;
    WorldSeamSegment segment;
} WorldSeamState;

static WorldSeamState s_world;

static void world_seam_increment(uint32_t *value)
{
    if (*value != UINT32_MAX)
        ++*value;
}

static void world_seam_increment_count(uint16_t *value)
{
    if (*value != UINT16_MAX)
        ++*value;
}

static void world_seam_bad(void)
{
    world_seam_increment(&s_world.tracking_bad);
}

static WorldSeamTexture *world_seam_texture(uint32_t texture)
{
    if (texture >= WORLD_SEAM_TEXTURE_CAPACITY) {
        world_seam_bad();
        return NULL;
    }
    return &s_world.textures[texture];
}

static WorldSeamFramebuffer *world_seam_framebuffer(
    uint32_t framebuffer, int create)
{
    uint32_t index;
    WorldSeamFramebuffer *empty = NULL;

    if (framebuffer == 0u)
        return NULL;
    for (index = 0u; index < WORLD_SEAM_FRAMEBUFFER_RECORDS; ++index) {
        WorldSeamFramebuffer *record = &s_world.framebuffers[index];
        if (record->name == framebuffer)
            return record;
        if (record->name == 0u && empty == NULL)
            empty = record;
    }
    if (!create)
        return NULL;
    if (empty == NULL) {
        world_seam_bad();
        return NULL;
    }
    empty->name = framebuffer;
    empty->color_texture = 0u;
    return empty;
}

static uint32_t world_seam_attachment(uint32_t framebuffer)
{
    WorldSeamFramebuffer *record = world_seam_framebuffer(framebuffer, 0);
    return record != NULL ? record->color_texture : 0u;
}

static WorldSeamDraw world_seam_draw_snapshot(void)
{
    WorldSeamDraw result;
    WorldSeamTexture *texture;

    memset(&result, 0, sizeof result);
    result.framebuffer = s_world.draw_framebuffer;
    result.attachment = world_seam_attachment(result.framebuffer);
    result.program = s_world.program;
    result.texture = s_world.bound_textures[0];
    texture = world_seam_texture(result.texture);
    if (texture != NULL) {
        result.texture_generation = texture->generation;
        if (texture->defined != 0u) {
            result.texture_width = texture->width;
            result.texture_height = texture->height;
        }
    }
    return result;
}

static uint32_t world_seam_float_bits(float value)
{
    uint32_t result;
    memcpy(&result, &value, sizeof result);
    return result;
}

static uint32_t world_seam_pair16(uint32_t high, uint32_t low)
{
    return ((high & 0xffffu) << 16) | (low & 0xffffu);
}

static uint32_t world_seam_color_hash(const uint32_t rgba[4])
{
    uint32_t hash = UINT32_C(2166136261);
    uint32_t index;
    for (index = 0u; index < 4u; ++index) {
        hash ^= rgba[index];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static void world_seam_segment_reset(void)
{
    memset(&s_world.segment, 0, sizeof s_world.segment);
}

static void world_seam_log(const char *point)
{
    WorldSeamDraw current = world_seam_draw_snapshot();
    WorldSeamSegment *segment = &s_world.segment;

    /* Compact hexadecimal fields keep even maximal legal state below the
     * logger's 384-byte body limit.  n=d/c/fbo/program/texture/viewport/attach;
     * cur=fbo/attachment/program/source/generation/source-WH/viewport-XY/WH;
     * fst/lst are the first/last draw tuples; clr=fbo/mask/RGBA-word-hash. */
    WORLD_SEAM_LOG(
        "[kage-vita] wsd b=%.24s q=%x k=%c s=%x t=%x p=%s "
        "n=%x/%x/%x/%x/%x/%x/%x "
        "cur=%x/%x/%x/%x/%x/%x/%x/%x "
        "fst=%x/%x/%x/%x lst=%x/%x/%x/%x/%x/%x "
        "clr=%x/%x/%x bad=%x",
        ISAAC_VITA_WORLD_SEAM_DIAG_BUILD_ID,
        (unsigned)s_world.sequence, s_world.target != 0u ? 'T' : 'C',
        (unsigned)s_world.stage, (unsigned)s_world.stage_type, point,
        (unsigned)segment->draws, (unsigned)segment->clears,
        (unsigned)segment->framebuffer_binds,
        (unsigned)segment->program_uses,
        (unsigned)segment->texture_binds,
        (unsigned)segment->viewport_sets,
        (unsigned)segment->attachments,
        (unsigned)current.framebuffer, (unsigned)current.attachment,
        (unsigned)current.program, (unsigned)current.texture,
        (unsigned)current.texture_generation,
        (unsigned)world_seam_pair16(
            current.texture_width, current.texture_height),
        (unsigned)world_seam_pair16(
            (uint32_t)s_world.viewport[0], (uint32_t)s_world.viewport[1]),
        (unsigned)world_seam_pair16(
            (uint32_t)s_world.viewport[2], (uint32_t)s_world.viewport[3]),
        (unsigned)segment->first_draw.framebuffer,
        (unsigned)segment->first_draw.attachment,
        (unsigned)segment->first_draw.program,
        (unsigned)segment->first_draw.texture,
        (unsigned)segment->last_draw.framebuffer,
        (unsigned)segment->last_draw.attachment,
        (unsigned)segment->last_draw.program,
        (unsigned)segment->last_draw.texture,
        (unsigned)segment->last_draw.texture_generation,
        (unsigned)world_seam_pair16(
            segment->last_draw.texture_width,
            segment->last_draw.texture_height),
        (unsigned)segment->last_clear_framebuffer,
        (unsigned)segment->last_clear_mask,
        (unsigned)segment->last_clear_color_hash,
        (unsigned)s_world.tracking_bad);
}

void kage_vita_world_seam_diag_reset(void)
{
    memset(&s_world, 0, sizeof s_world);
}

void kage_vita_world_seam_diag_begin(
    uint32_t stage, uint32_t stage_type)
{
    uint32_t target = stage == 8u && stage_type == 1u;
    uint32_t control = stage >= 1u && stage <= 13u && !target;

    if (s_world.active != 0u) {
        world_seam_bad();
        s_world.active = 0u;
    }
    if ((target && s_world.target_receipts >= WORLD_SEAM_TARGET_LIMIT) ||
            (control &&
             s_world.control_receipts >= WORLD_SEAM_CONTROL_LIMIT) ||
            (!target && !control))
        return;
    if (target)
        ++s_world.target_receipts;
    else
        ++s_world.control_receipts;
    world_seam_increment(&s_world.sequence);
    s_world.stage = stage;
    s_world.stage_type = stage_type;
    s_world.target = target;
    s_world.active = 1u;
    s_world.phase = 1u;
    world_seam_segment_reset();
    world_seam_log("begin");
}

void kage_vita_world_seam_diag_post_room(void)
{
    if (s_world.active == 0u)
        return;
    if (s_world.phase != 1u) {
        world_seam_bad();
        return;
    }
    world_seam_log("room");
    world_seam_segment_reset();
    s_world.phase = 2u;
}

void kage_vita_world_seam_diag_post_lua(void)
{
    if (s_world.active == 0u)
        return;
    if (s_world.phase != 2u) {
        world_seam_bad();
        return;
    }
    world_seam_log("lua");
    world_seam_segment_reset();
    s_world.phase = 3u;
}

void kage_vita_world_seam_diag_pre_hud(void)
{
    if (s_world.active == 0u)
        return;
    if (s_world.phase != 3u) {
        world_seam_bad();
        s_world.active = 0u;
        return;
    }
    world_seam_log("late");
    world_seam_segment_reset();
    s_world.phase = 0u;
    s_world.active = 0u;
}

void kage_vita_world_seam_diag_note_active_texture(uint32_t texture)
{
    if (texture < WORLD_SEAM_TEXTURE0 ||
            texture >= WORLD_SEAM_TEXTURE0 + WORLD_SEAM_TEXTURE_UNITS) {
        world_seam_bad();
        return;
    }
    s_world.active_texture = texture - WORLD_SEAM_TEXTURE0;
}

void kage_vita_world_seam_diag_note_bind_texture(
    uint32_t target, uint32_t texture)
{
    if (target != WORLD_SEAM_TEXTURE_2D ||
            s_world.active_texture >= WORLD_SEAM_TEXTURE_UNITS) {
        world_seam_bad();
        return;
    }
    s_world.bound_textures[s_world.active_texture] = texture;
    if (s_world.active != 0u)
        world_seam_increment_count(&s_world.segment.texture_binds);
}

void kage_vita_world_seam_diag_note_gen_texture(uint32_t texture)
{
    WorldSeamTexture *record = world_seam_texture(texture);
    if (record == NULL)
        return;
    record->width = 0u;
    record->height = 0u;
    record->defined = 0u;
    if (record->generation != UINT16_MAX)
        ++record->generation;
}

void kage_vita_world_seam_diag_note_delete_texture(uint32_t texture)
{
    uint32_t index;
    WorldSeamTexture *record = world_seam_texture(texture);
    if (record != NULL) {
        record->width = 0u;
        record->height = 0u;
        record->defined = 0u;
    }
    for (index = 0u; index < WORLD_SEAM_TEXTURE_UNITS; ++index) {
        if (s_world.bound_textures[index] == texture)
            s_world.bound_textures[index] = 0u;
    }
    for (index = 0u; index < WORLD_SEAM_FRAMEBUFFER_RECORDS; ++index) {
        if (s_world.framebuffers[index].color_texture == texture)
            s_world.framebuffers[index].color_texture = 0u;
    }
}

void kage_vita_world_seam_diag_note_tex_image(
    uint32_t target, int32_t level, int32_t width, int32_t height)
{
    uint32_t texture;
    WorldSeamTexture *record;
    if (target != WORLD_SEAM_TEXTURE_2D || level != 0 || width < 0 ||
            height < 0 || width > UINT16_MAX || height > UINT16_MAX ||
            s_world.active_texture >= WORLD_SEAM_TEXTURE_UNITS) {
        world_seam_bad();
        return;
    }
    texture = s_world.bound_textures[s_world.active_texture];
    record = world_seam_texture(texture);
    if (record == NULL)
        return;
    record->width = (uint16_t)width;
    record->height = (uint16_t)height;
    record->defined = 1u;
}

void kage_vita_world_seam_diag_note_bind_framebuffer(
    uint32_t target, uint32_t framebuffer)
{
    if (target == WORLD_SEAM_FRAMEBUFFER) {
        s_world.draw_framebuffer = framebuffer;
        s_world.read_framebuffer = framebuffer;
    } else if (target == WORLD_SEAM_DRAW_FRAMEBUFFER) {
        s_world.draw_framebuffer = framebuffer;
    } else if (target == WORLD_SEAM_READ_FRAMEBUFFER) {
        s_world.read_framebuffer = framebuffer;
    } else {
        world_seam_bad();
        return;
    }
    if (framebuffer != 0u)
        (void)world_seam_framebuffer(framebuffer, 1);
    if (s_world.active != 0u)
        world_seam_increment_count(&s_world.segment.framebuffer_binds);
}

void kage_vita_world_seam_diag_note_delete_framebuffer(uint32_t framebuffer)
{
    WorldSeamFramebuffer *record = world_seam_framebuffer(framebuffer, 0);
    if (record != NULL)
        memset(record, 0, sizeof *record);
    if (s_world.draw_framebuffer == framebuffer)
        s_world.draw_framebuffer = 0u;
    if (s_world.read_framebuffer == framebuffer)
        s_world.read_framebuffer = 0u;
}

void kage_vita_world_seam_diag_note_framebuffer_texture(
    uint32_t target, uint32_t attachment, uint32_t texture_target,
    uint32_t texture, int32_t level)
{
    uint32_t framebuffer;
    WorldSeamFramebuffer *record;
    if (attachment != WORLD_SEAM_COLOR_ATTACHMENT0 ||
            texture_target != WORLD_SEAM_TEXTURE_2D || level != 0) {
        world_seam_bad();
        return;
    }
    if (target == WORLD_SEAM_FRAMEBUFFER ||
            target == WORLD_SEAM_DRAW_FRAMEBUFFER)
        framebuffer = s_world.draw_framebuffer;
    else if (target == WORLD_SEAM_READ_FRAMEBUFFER)
        framebuffer = s_world.read_framebuffer;
    else {
        world_seam_bad();
        return;
    }
    record = world_seam_framebuffer(framebuffer, framebuffer != 0u);
    if (record != NULL)
        record->color_texture = texture;
    else if (framebuffer != 0u)
        world_seam_bad();
    if (s_world.active != 0u)
        world_seam_increment_count(&s_world.segment.attachments);
}

void kage_vita_world_seam_diag_note_use_program(uint32_t program)
{
    s_world.program = program;
    if (s_world.active != 0u)
        world_seam_increment_count(&s_world.segment.program_uses);
}

void kage_vita_world_seam_diag_note_viewport(
    int32_t x, int32_t y, int32_t width, int32_t height)
{
    if (x < INT16_MIN || x > INT16_MAX ||
            y < INT16_MIN || y > INT16_MAX ||
            width < 0 || width > UINT16_MAX ||
            height < 0 || height > UINT16_MAX) {
        world_seam_bad();
        return;
    }
    s_world.viewport[0] = x;
    s_world.viewport[1] = y;
    s_world.viewport[2] = width;
    s_world.viewport[3] = height;
    if (s_world.active != 0u)
        world_seam_increment_count(&s_world.segment.viewport_sets);
}

void kage_vita_world_seam_diag_note_clear_color(
    float red, float green, float blue, float alpha)
{
    s_world.clear_rgba[0] = world_seam_float_bits(red);
    s_world.clear_rgba[1] = world_seam_float_bits(green);
    s_world.clear_rgba[2] = world_seam_float_bits(blue);
    s_world.clear_rgba[3] = world_seam_float_bits(alpha);
}

void kage_vita_world_seam_diag_note_clear(uint32_t mask)
{
    WorldSeamSegment *segment;
    if (s_world.active == 0u)
        return;
    segment = &s_world.segment;
    world_seam_increment_count(&segment->clears);
    segment->last_clear_framebuffer = s_world.draw_framebuffer;
    segment->last_clear_mask = mask;
    segment->last_clear_color_hash =
        world_seam_color_hash(s_world.clear_rgba);
}

void kage_vita_world_seam_diag_note_draw(void)
{
    WorldSeamSegment *segment;
    WorldSeamDraw draw;
    if (s_world.active == 0u)
        return;
    segment = &s_world.segment;
    draw = world_seam_draw_snapshot();
    if (segment->draws == 0u)
        segment->first_draw = draw;
    segment->last_draw = draw;
    world_seam_increment_count(&segment->draws);
}
