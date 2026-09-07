/* Fresh actual attachment/reset/scissor bodies; SDK and memory are mocks.
 * The scene harness follows vitagl_fbo_float_sync_host_test.c. This checks
 * deferred invalidation and SDK argument values, not GPU pixels/ABI or OOM. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_COLOR_ATTACHMENT0 1
#define GL_INVALID_ENUM 2
#define DEPTHBUFFER_READY 1
#define DEPTHBUFFER_WANTS_STENCIL 2
#define SCE_GXM_TEXTURE_FORMAT_F16F16F16F16_RGBA 16
#define SCE_GXM_MULTISAMPLE_NONE 0
#define SCE_GXM_COLOR_SURFACE_LINEAR 0
#define SCE_GXM_COLOR_SURFACE_SCALE_NONE 0
#define SCE_GXM_COLOR_SURFACE_SCALE_MSAA_DOWNSCALE 1
#define SCE_GXM_OUTPUT_REGISTER_SIZE_64BIT 64
#define SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT 32
#define SCE_GXM_REGION_CLIP_OUTSIDE 0
#define SKIP_SPLASHSCREEN 1
#define HAVE_ISAAC_GXM_STATE_SHADOW 1
#define HAVE_ISAAC_FBO_RT_SCENES 1
#define HAVE_ISAAC_FBO_RT_REUSE 1
#define HAVE_ISAAC_FBO_RT_REUSE_LEASE 1
#define THREAD_SAFE()
#define VGL_ALIGN(v,a) (((v)+(a)-1)&~((a)-1))
#define ISAAC_FBO_RT_SCENES() gxm_fbo_rt_size
#define SET_GL_ERROR_WITH_VALUE(e,v) do { (void)(e); (void)(v); CHECK(0); return; } while (0);
typedef int GLboolean;
typedef unsigned GLenum, GLuint;
typedef int GLint, GLsizei;
typedef int SceGxmTextureFormat;
typedef struct { uint32_t xMax, yMax; } SceGxmValidRegion;
typedef struct { int width, height, format; void *data; } MockTexture;
typedef struct { MockTexture gxm_tex; int format, ref_counter, dirty; void *data; } texture;
typedef struct { int id; } SceGxmRenderTarget;
typedef struct { void *data; int registers, format; } SceGxmColorSurface;
typedef struct { void *depthData; } SceGxmDepthStencilSurface;
typedef struct framebuffer {
    int width, height, stride, format, depthbuffer_state, is_float;
    void *data;
    texture *tex;
    SceGxmRenderTarget *target;
    SceGxmColorSurface colorbuffer;
    SceGxmDepthStencilSurface depthbuffer, *depthbuffer_ptr;
} framebuffer;
static unsigned checks, begins, ends, finishes, syncs, depths, creates, rt_records;
static unsigned viewport_calls, scissor_calls, shadow_ends, shadow_begins;
static unsigned scene_sizes[4];
static int expected_selector, begin_result, fail_create, nested_legacy, recover_depth;
static GLboolean dirty_framebuffer, dirty_query, needs_scene_reset, needs_end_scene;
static GLboolean dirty_scissor_state, is_fbo_float, is_rendering_display, scissor_test_state;
static framebuffer fb, *active_write_fb, *in_use_framebuffer, *old_framebuffer;
static texture texture_slots[2];
static framebuffer *active_read_fb;
#ifndef TEST_SCISSOR_REPLAY
static framebuffer other;
#endif
static unsigned retired_rt, retired_depth, clip_calls, mask_draws;
static int last_clip[4];
static int old_backing, new_backing, depth_backing, legacy_backing;
static SceGxmRenderTarget target = {1}, *gxm_render_target = &target;
static SceGxmColorSurface gxm_color_surfaces[2];
static SceGxmDepthStencilSurface gxm_depth_stencil_surface;
static void *gxm_context = (void *)(uintptr_t)0x1234, *gxm_sync_objects[2];
static int gxm_back_buffer_index, gxm_fbo_rt_size = 8, msaa_mode, frame_purge_idx;
static int system_app_mode, shared_fb, vsync_interval;
static struct { int index, vsync; } shared_fb_info;
static struct { unsigned value; } query_fence;
static struct { int x,y,w,h; } gl_viewport;
static struct { int x,y,w,h,gl_x,gl_y,gl_w,gl_h; } region;
static float x_port, x_scale, y_port, y_scale, z_port, z_scale;
#ifdef TEST_SCISSOR_REPLAY
static GLboolean skip_viewport_override;
static float fullscreen_x_port=480, fullscreen_x_scale=480;
static float fullscreen_y_port=272, fullscreen_y_scale=-272;
static float fullscreen_z_port, fullscreen_z_scale;
static float last_viewport[6];
/* Timing boundaries are not part of this SDK-coordinate check. */
#define ISAAC_SCENE_TIME_BEGIN() ((void)0)
#define ISAAC_SCENE_TIME_END_END_SCENE() ((void)0)
#define ISAAC_SCENE_TIME_END_BEGIN_SCENE() ((void)0)
#define isaacNativeResourceRetireDepth(p) (p)
#endif
static unsigned legacy_pool_size;
static float *legacy_pool, *legacy_pool_ptr, *legacy_pool_end;
static void glFinish(void);
static void scene_reset(void);
static void expect_selector(void) { CHECK(is_fbo_float == expected_selector); }
static int vglGetTexFormat(MockTexture *t) { return t->format; }
static void vglGetTexSizes(MockTexture *t, int *w, int *h) { *w=t->width; *h=t->height; }
static void *vglGetTexData(MockTexture *t) { return t->data; }
static int tex_format_to_bytespp(int f) { return f == 16 ? 8 : 4; }
static int get_color_from_texture(int f) { return f; }
static void *sceGxmColorSurfaceGetData(SceGxmColorSurface *s) { return s->data; }
static int sceGxmColorSurfaceInit(SceGxmColorSurface *s, int fmt, int type,
                                int scale, int registers, int w, int h,
                                int stride, void *data) {
    CHECK(type == 0 && scale == 0 && w > 0 && h > 0 && stride == VGL_ALIGN(w,8));
    CHECK(registers == (fmt == 16 ? 64 : 32));
    *s = (SceGxmColorSurface){data, registers, fmt}; ++syncs; return 0;
}
static void sceGxmFinish(void *ctx) { CHECK(ctx == gxm_context); expect_selector(); ++finishes; }
static int sceGxmBeginScene(void *ctx, int flags, SceGxmRenderTarget *rt,
                           const void *valid, const void *vertex_sync, const void *fragment_sync,
                           SceGxmColorSurface *color, SceGxmDepthStencilSurface *depth) {
#ifdef TEST_VALID_REGION
    CHECK(ctx == gxm_context && !flags && !vertex_sync);
    if (valid) {
        const SceGxmValidRegion *r=valid;
        CHECK(r->xMax == (unsigned)gl_viewport.w-1 && r->yMax == (unsigned)gl_viewport.h-1);
    }
#else
    CHECK(ctx == gxm_context && !flags && !valid && !vertex_sync);
#endif
    (void)fragment_sync; (void)rt; (void)depth;
    expect_selector();
    if (active_write_fb) {
        CHECK(color == &active_write_fb->colorbuffer && color->data == active_write_fb->tex->data);
        CHECK(color->registers == (active_write_fb->tex->format == 16 ? 64 : 32));
    } else CHECK(color == &gxm_color_surfaces[gxm_back_buffer_index]);
    ++begins; return begin_result;
}
static void sceGxmEndScene(void *ctx, void *a, const void *fence) {
    CHECK(ctx == gxm_context && !a && fence == &query_fence); ++ends;
}
static void sceDisplayWaitVblankStartMulti(int n) { CHECK(n == vsync_interval); }
static void isaacGxmStateEndScene(void *ctx) { CHECK(ctx == gxm_context); ++shadow_ends; }
static void isaacGxmStateBeginScene(void *ctx, int r) { CHECK(ctx == gxm_context && r == begin_result); ++shadow_begins; }
static void sceSharedFbBegin(int shared, void *info) { (void)shared; (void)info; CHECK(0); }
#ifdef TEST_SCISSOR_REPLAY
static void glViewport(int x, int y, int w, int h);
#else
static void glViewport(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; ++viewport_calls; }
#endif
static void vglSetViewport(void *ctx, float x, float xs, float y, float ys, float z, float zs) {
    (void)ctx; (void)x; (void)xs; (void)y; (void)ys; (void)z; (void)zs; ++viewport_calls;
#ifdef TEST_SCISSOR_REPLAY
    CHECK(ctx == gxm_context);
    const float v[6]={x,xs,y,ys,z,zs}; memcpy(last_viewport,v,sizeof v);
#endif
}
static void change_cull_mode(void) {}
static void update_scissor_test(void);
static void sceGxmSetRegionClip(void *ctx, int mode, int x, int y, int r, int b) {
    CHECK(ctx == gxm_context && mode == SCE_GXM_REGION_CLIP_OUTSIDE);
    last_clip[0]=x; last_clip[1]=y; last_clip[2]=r; last_clip[3]=b; ++clip_calls;
}
static void gpu_free_texture(texture *t) { (void)t; CHECK(0); }
static void mark_rt_as_dirty(SceGxmRenderTarget *rt) { CHECK(rt == &target); ++retired_rt; }
static void mark_as_dirty(void *p) { CHECK(p == &depth_backing); ++retired_depth; }
static void isaacFboRtInvalidate(uintptr_t p) { (void)p; CHECK(0); }
static int isaacFboRtSwitch(uintptr_t f, uintptr_t t, int ow, int oh, int nw, int nh,
                            int eligible, int bucket, uintptr_t *next) {
    (void)f; (void)t; (void)ow; (void)oh; (void)nw; (void)nh;
    (void)eligible; (void)bucket; *next=0; return 0;
}
static int vglIsaacSetupFboRenderTargetScenes(int n) { CHECK(n == 0); return 8; }
static void isaacFboRtCreated(uintptr_t f, uintptr_t t, int w, int h, int eligible) {
    CHECK(f == (uintptr_t)active_write_fb && w == active_write_fb->width && h == active_write_fb->height);
    CHECK(t == (fail_create ? 0 : (uintptr_t)&target) && eligible == !fail_create); ++rt_records;
}
static int isaacFboRtHasSpare(void) { return 0; }
static int isaacFboRtHasMatureSpare(void) { CHECK(0); return 0; }
static void isaacFboRtRecoveryBegin(void) { CHECK(0); }
static void isaacFboRtRecoveryEnd(void) { CHECK(0); }
static void vglIsaacFboRtDrainAfterFinish(void) { CHECK(0); }
static int setup_render_target(SceGxmRenderTarget **rt, int w, int h, int scenes) {
    CHECK(w == active_write_fb->width && h == active_write_fb->height && creates < 4); expect_selector();
    scene_sizes[creates++] = (unsigned)scenes;
    if (fail_create && scenes == 8) return -1;
    *rt = &target; return 0;
}
static void init_depth_stencil_buffer(unsigned w, unsigned h, SceGxmDepthStencilSurface *s, GLboolean stencil) {
    CHECK(w == (unsigned)active_write_fb->width && h == (unsigned)active_write_fb->height && !stencil); expect_selector(); ++depths;
    /* Native depth allocation uses GPU recovery: Finish, not glFinish. */
    if (recover_depth) { recover_depth = 0; sceGxmFinish(gxm_context); }
    s->depthData = &depth_backing;
}
static void *gpu_alloc_mapped_temp(unsigned bytes) {
    CHECK(bytes == sizeof legacy_backing);
    /* Native CPU temporary-allocation recovery can reenter before lazy sync.
     * One-shot boundary mock; extracted glFinish/scene_reset run unchanged. */
    if (nested_legacy) { nested_legacy = 0; legacy_pool_size = 0; glFinish(); }
    return &legacy_backing;
}

#define DISPLAY_WIDTH 960
#define DISPLAY_HEIGHT 544
#define GL_FRAMEBUFFER 10
#define GL_DRAW_FRAMEBUFFER 11
#define GL_READ_FRAMEBUFFER 12
#define GL_TEXTURE_2D 13
#define GL_INVALID_OPERATION 14
#define GL_INVALID_VALUE 15
#define SET_GL_ERROR(e) SET_GL_ERROR_WITH_VALUE(e,0)
#define SCE_GXM_CULL_NONE 0
#define SCE_GXM_PRIMITIVE_TRIANGLE_FAN 0
#define SCE_GXM_INDEX_FORMAT_U16 0
#define SCE_GXM_STENCIL_FUNC_NEVER 0
#define SCE_GXM_STENCIL_FUNC_ALWAYS 1
#define SCE_GXM_STENCIL_OP_KEEP 0
#define clear_vertex_program_patched 0
#define scissor_test_fragment_program 0
#define clear_position 0
#define clear_depth 1
#define depth_clear_indices NULL
static struct { float x,y,z,w; } clear_vertices[1], scissor_test_vertices[1];
#ifdef TEST_SCISSOR_REPLAY
static void invalidate_viewport(void);
static void validate_viewport(void);
#else
static void invalidate_viewport(void) {}
static void validate_viewport(void) {}
#endif
static void refresh_stencil_settings(void) {}
static void vglRestoreVertexUniformBuffer(void) {}
#define sceGxmSetVertexProgram(c,p) ((void)(c),(void)(p))
#define sceGxmSetFragmentProgram(c,p) ((void)(c),(void)(p))
#define sceGxmSetCullMode(c,m) ((void)(c),(void)(m))
#define vector4f_convert_to_local_space(p,x,y,w,h) ((void)(p),(void)(x),(void)(y),(void)(w),(void)(h))
#define sceGxmSetFrontStencilFunc(...) ((void)0)
#define sceGxmSetBackStencilFunc(...) ((void)0)
static void sceGxmReserveVertexDefaultUniformBuffer(void *ctx, void **out) {
    CHECK(ctx == gxm_context); *out=&legacy_backing;
}
static void sceGxmSetUniformDataF(void *buffer, int parameter, int off, int count, const float *data) {
    CHECK(buffer == &legacy_backing && off == 0 && (count == 1 || count == 4) && data);
    (void)parameter;
}
static void sceGxmDraw(void *ctx, int primitive, int format, const void *indices, int count) {
    CHECK(ctx == gxm_context && !primitive && !format && !indices && count == 4);
    ++mask_draws;
}
#include "native_fbo_scissor_resize.h"

static void reset(int w, int h, int enabled) {
    begins=ends=finishes=syncs=depths=creates=rt_records=0;
    viewport_calls=scissor_calls=shadow_ends=shadow_begins=0;
    retired_rt=retired_depth=clip_calls=mask_draws=0;
    fail_create=begin_result=nested_legacy=recover_depth=0; legacy_pool_size=0;
    texture_slots[1]=(texture){{w,h,8,&old_backing},8,1,0,&old_backing};
    memset(&fb,0,sizeof fb);
    fb.width=w; fb.height=h; fb.stride=VGL_ALIGN(w,8)*4;
    fb.tex=&texture_slots[1]; fb.target=&target; fb.depthbuffer.depthData=&depth_backing;
    fb.depthbuffer_state=DEPTHBUFFER_READY; fb.depthbuffer_ptr=&fb.depthbuffer;
    fb.colorbuffer=(SceGxmColorSurface){&old_backing,32,8};
    active_write_fb=in_use_framebuffer=old_framebuffer=active_read_fb=&fb;
    dirty_framebuffer=dirty_query=needs_scene_reset=0; needs_end_scene=1;
    is_fbo_float=is_rendering_display=expected_selector=0;
    scissor_test_state=enabled; dirty_scissor_state=1;
#ifdef TEST_VALID_REGION
    vglIsaacSetupFboValidRegion(0);
    isaac_fbo_region_active=isaac_fbo_region_known=0;
#endif
#ifdef TEST_SCISSOR_REPLAY
    glViewport(0,0,w,h);
#endif
    glScissor(0,0,160,160);
    scene_reset();
    CHECK(mask_draws == (unsigned)(enabled ? 2 : 1) && !dirty_scissor_state);
    CHECK(region.w == w && region.h == h);
    clip_calls=mask_draws=0;
}
static void redefine(int w, int h) {
    /* Model only public upload's resulting descriptor/backing publication.
     * Actual glTexImage2D allocation/retirement is not emulated. */
    texture_slots[1].gxm_tex.width=w; texture_slots[1].gxm_tex.height=h;
    texture_slots[1].gxm_tex.data=texture_slots[1].data=&new_backing;
}
static void check_resize(int ow, int oh, int nw, int nh, int explicit_attach, int enabled) {
    reset(ow,oh,enabled); redefine(nw,nh);
    if (explicit_attach) {
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,1,0);
        CHECK(!mask_draws && dirty_scissor_state == (EXPECT_FIXED && enabled));
    }
    glFlush();
    CHECK(fb.width == nw && fb.height == nh && fb.stride == VGL_ALIGN(nw,8)*4);
    CHECK(syncs == 1 && begins == 1 && ends == 1 && !finishes);
    CHECK(depths == 1 && creates == 1 && retired_rt == 1 && retired_depth == 1);
    CHECK(texture_slots[1].ref_counter == 1 && fb.target == &target && fb.depthbuffer_ptr == &fb.depthbuffer);
    CHECK(mask_draws == (unsigned)(EXPECT_FIXED && enabled ? 2 : 0));
    CHECK(!dirty_scissor_state);
    CHECK(region.w == (EXPECT_FIXED && enabled ? nw : ow));
    CHECK(region.h == (EXPECT_FIXED && enabled ? nh : oh));
    if (enabled) {
        CHECK(last_clip[0] == 0 && last_clip[1] == 0);
        CHECK(last_clip[2] == (EXPECT_FIXED ? nw : ow)-1);
        CHECK(last_clip[3] == (EXPECT_FIXED ? nh : oh)-1);
    } else CHECK(clip_calls == 0);
    unsigned draws=mask_draws, clips=clip_calls;
    scene_reset();
    CHECK(mask_draws == draws && !dirty_scissor_state && syncs == 1);
    CHECK(begins == (unsigned)(explicit_attach ? 1 : 2));
    if (!enabled) CHECK(clip_calls == clips);
}
#ifdef TEST_SCISSOR_REPLAY
static void check_replay(int display, int enabled, int y, int apply_region) {
    reset(128,128,enabled);
    if (display) { active_write_fb=NULL; glFlush(); }
#ifdef TEST_VALID_REGION
    vglIsaacSetupFboValidRegion((uint8_t)apply_region);
#else
    CHECK(!apply_region);
#endif
    /* Smaller viewport exercises the real 0009 clamp, not a replacement mock.
     * Actual glViewport + invalidate/validate bodies retain SDK orientation. */
    glViewport(0,0,apply_region ? 96 : 128,apply_region ? 32 : 128);
    glScissor(8,y,32,32); glFlush();
    int initial[4], cached_region[8]; float viewport[6];
    memcpy(initial,last_clip,sizeof initial); memcpy(cached_region,&region,sizeof cached_region);
    memcpy(viewport,last_viewport,sizeof viewport);
    CHECK(!dirty_scissor_state && old_framebuffer == in_use_framebuffer);
    if (enabled) {
        int first_y=region.y;
#ifndef HAVE_UNFLIPPED_FBOS
        if (!display) first_y=y > 0 ? y : 0;
#endif
        if (apply_region && !display && first_y > 31) first_y=31;
        CHECK(initial[0] == 8 && initial[1] == first_y && initial[2] == 39);
    }
    const unsigned draws=mask_draws, synced=syncs, allocated=creates+depths;
    for (unsigned i=0;i<2;++i) {
        unsigned b=begins,e=ends,f=finishes,c=clip_calls;
        if (i) glFinish(); else glFlush();
        CHECK(begins == b+1 && ends == e+1 && finishes == f+i);
        CHECK(mask_draws == draws && syncs == synced && creates+depths == allocated);
        CHECK(!dirty_scissor_state && !memcmp(cached_region,&region,sizeof cached_region));
        CHECK(!memcmp(viewport,last_viewport,sizeof viewport));
        CHECK(clip_calls == c+(unsigned)enabled);
        if (enabled) {
            int expected[4]; memcpy(expected,initial,sizeof expected);
#if !EXPECT_REPLAY_FIXED && !defined(HAVE_UNFLIPPED_FBOS)
            if (!display) {
                expected[1]=region.y; expected[3]=region.y+region.h-1;
                if (apply_region) {
                    if (expected[3] > 31) expected[3]=31;
                    if (expected[1] > expected[3]) expected[1]=expected[3];
                }
            }
#endif
            CHECK(!memcmp(last_clip,expected,sizeof expected));
        }
        b=begins; c=clip_calls; scene_reset();
        CHECK(begins == b && clip_calls == c && mask_draws == draws);
    }
}
#endif
int main(void) {
#ifdef TEST_SCISSOR_REPLAY
    for (int display=0;display<2;++display)
        for (int enabled=0;enabled<2;++enabled)
            for (int y=-4;y<=16;y+=20) {
#ifndef TEST_REPLAY_REGION_ONLY
                check_replay(display,enabled,y,0);
#endif
#ifdef TEST_VALID_REGION
                check_replay(display,enabled,y,1);
#endif
            }
    printf("FBO scissor replay actual bodies: fixed=%d checks=%u PASS\n",EXPECT_REPLAY_FIXED,checks);
#else
    const int sizes[][4]={{64,128,128,128},{128,128,64,128},
                         {128,64,128,128},{128,128,128,64}};
    for (unsigned i=0;i<sizeof sizes/sizeof sizes[0];++i)
        for (int explicit_attach=0;explicit_attach<2;++explicit_attach)
            for (int enabled=0;enabled<2;++enabled)
                check_resize(sizes[i][0],sizes[i][1],sizes[i][2],sizes[i][3],explicit_attach,enabled);
    /* Asymmetric X box; symmetric Y excludes the known flip-replay issue. */
    reset(64,128,1); glScissor(48,32,48,64); scene_reset();
    CHECK(region.w == 16); mask_draws=0; redefine(128,128); glFlush();
    CHECK(last_clip[0] == 48 && last_clip[1] == 32 && last_clip[3] == 95);
    CHECK(last_clip[2] == (EXPECT_FIXED ? 95 : 63));
    reset(64,128,1); redefine(64,128); glFlush();
    CHECK(syncs == 1 && !mask_draws && !depths && !creates && !retired_rt && !retired_depth);
    /* Inactive object resize must not invalidate current object's scissor. */
    reset(64,128,1); other=fb; other.depthbuffer_ptr=&other.depthbuffer;
    ++texture_slots[1].ref_counter; redefine(128,128);
    _glFramebufferTexture2D(&other,GL_COLOR_ATTACHMENT0,&texture_slots[1],1);
    CHECK(!dirty_scissor_state && !mask_draws && region.w == 64);
    /* Model glBindFramebuffer's pointer selection; actual reset identity path runs. */
    active_write_fb=&other; glFlush();
    CHECK(old_framebuffer == &other && region.w == 128 && region.h == 128 && mask_draws == 2);
    CHECK(last_clip[2] == 127 && !dirty_scissor_state);
    printf("FBO scissor resize actual bodies: fixed=%d checks=%u PASS\n",EXPECT_FIXED,checks);
#endif
    return 0;
}
