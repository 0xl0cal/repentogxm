/* Actual freshly patched scene_reset, attachment helper, scene_end and
 * glFinish are included below. SDK/memory boundaries are mocks: this is not
 * a Vita ABI, OOM, shader execution, or public-API integration test. */
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
typedef int SceGxmTextureFormat;
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
static texture tex;
static int old_backing, new_backing, depth_backing, legacy_backing;
static SceGxmRenderTarget target = {1}, *gxm_render_target = &target;
static SceGxmColorSurface gxm_color_surfaces[2];
static SceGxmDepthStencilSurface gxm_depth_stencil_surface;
static void *gxm_context = (void *)(uintptr_t)0x1234, *gxm_sync_objects[2];
static int gxm_back_buffer_index, gxm_fbo_rt_size = 8, msaa_mode, frame_purge_idx;
static int system_app_mode, shared_fb, vsync_interval;
static struct { int index, vsync; } shared_fb_info;
static struct { unsigned value; } query_fence;
static struct { int x,y,w,h; } gl_viewport, region;
static float x_port, x_scale, y_port, y_scale, z_port, z_scale;
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
    CHECK(type == 0 && scale == 0 && w == 64 && h == 64 && stride == 64);
    CHECK(registers == (fmt == 16 ? 64 : 32));
    *s = (SceGxmColorSurface){data, registers, fmt}; ++syncs; return 0;
}
static void sceGxmFinish(void *ctx) { CHECK(ctx == gxm_context); expect_selector(); ++finishes; }
static int sceGxmBeginScene(void *ctx, int flags, SceGxmRenderTarget *rt,
                           const void *valid, const void *vertex_sync, const void *fragment_sync,
                           SceGxmColorSurface *color, SceGxmDepthStencilSurface *depth) {
    CHECK(ctx == gxm_context && !flags && !valid && !vertex_sync);
    (void)fragment_sync; (void)rt; (void)depth;
    expect_selector();
    if (active_write_fb) {
        CHECK(color == &fb.colorbuffer && color->data == tex.data);
        CHECK(color->registers == (tex.format == 16 ? 64 : 32));
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
static void glViewport(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; ++viewport_calls; }
static void vglSetViewport(void *ctx, float x, float xs, float y, float ys, float z, float zs) {
    (void)ctx; (void)x; (void)xs; (void)y; (void)ys; (void)z; (void)zs; ++viewport_calls;
}
static void change_cull_mode(void) {}
static void update_scissor_test(void) { ++scissor_calls; dirty_scissor_state = 0; }
static void sceGxmSetRegionClip(void *ctx, int mode, int x, int y, int r, int b) {
    (void)ctx; (void)mode; (void)x; (void)y; (void)r; (void)b;
}
static void gpu_free_texture(texture *t) { (void)t; CHECK(0); }
static void mark_rt_as_dirty(SceGxmRenderTarget *rt) { (void)rt; CHECK(0); }
static void mark_as_dirty(void *p) { (void)p; CHECK(0); }
static void isaacFboRtInvalidate(uintptr_t p) { (void)p; CHECK(0); }
static int isaacFboRtSwitch(uintptr_t f, uintptr_t t, int ow, int oh, int nw, int nh,
                            int eligible, int bucket, uintptr_t *next) {
    (void)f; (void)t; (void)ow; (void)oh; (void)nw; (void)nh;
    (void)eligible; (void)bucket; (void)next; CHECK(0); return 0;
}
static int vglIsaacSetupFboRenderTargetScenes(int n) { CHECK(n == 0); return 8; }
static void isaacFboRtCreated(uintptr_t f, uintptr_t t, int w, int h, int eligible) {
    CHECK(f == (uintptr_t)&fb && w == 64 && h == 64);
    CHECK(t == (fail_create ? 0 : (uintptr_t)&target) && eligible == !fail_create); ++rt_records;
}
static int isaacFboRtHasSpare(void) { return 0; }
static int isaacFboRtHasMatureSpare(void) { CHECK(0); return 0; }
static void isaacFboRtRecoveryBegin(void) { CHECK(0); }
static void isaacFboRtRecoveryEnd(void) { CHECK(0); }
static void vglIsaacFboRtDrainAfterFinish(void) { CHECK(0); }
static int setup_render_target(SceGxmRenderTarget **rt, int w, int h, int scenes) {
    CHECK(w == 64 && h == 64 && creates < 4); expect_selector();
    scene_sizes[creates++] = (unsigned)scenes;
    if (fail_create && scenes == 8) return -1;
    *rt = &target; return 0;
}
static void init_depth_stencil_buffer(unsigned w, unsigned h, SceGxmDepthStencilSurface *s, GLboolean stencil) {
    CHECK(w == 64 && h == 64 && !stencil); expect_selector(); ++depths;
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
#include "native_fbo_float_sync.h"

static void reset(int old_float, int new_float, int mismatch) {
    begins=ends=finishes=syncs=depths=creates=rt_records=0;
    viewport_calls=scissor_calls=shadow_ends=shadow_begins=0;
    fail_create=begin_result=nested_legacy=recover_depth=0; legacy_pool_size=0;
    tex = (texture){{64,64,new_float ? 16 : 8,mismatch ? &new_backing : &old_backing},
                    new_float ? 16 : 8,1,0,mismatch ? &new_backing : &old_backing};
    memset(&fb,0,sizeof fb);
    fb.width=fb.height=64; fb.tex=&tex; fb.target=&target; fb.is_float=old_float;
    fb.depthbuffer_state=DEPTHBUFFER_READY; fb.depthbuffer_ptr=&fb.depthbuffer;
    fb.colorbuffer=(SceGxmColorSurface){&old_backing,old_float ? 64 : 32,old_float ? 16 : 8};
    active_write_fb=in_use_framebuffer=old_framebuffer=&fb;
    dirty_framebuffer=dirty_query=dirty_scissor_state=0;
    needs_scene_reset=needs_end_scene=1;
    is_fbo_float=!old_float; /* The early snapshot still owns unchanged paths. */
    expected_selector=EXPECT_FIXED && mismatch ? new_float : old_float;
}
int main(void) {
    for (int old_float=0;old_float<2;old_float++) {
        int new_float=!old_float;
        reset(old_float,new_float,1); scene_reset();
        CHECK(fb.is_float == new_float && dirty_framebuffer && syncs == 1);
        CHECK(begins == 1 && ends == 1 && !creates && !depths && !finishes);
        CHECK(tex.ref_counter == 1 && fb.target == &target && fb.depthbuffer_ptr == &fb.depthbuffer);
        expected_selector=new_float; glFinish(); /* Actual second-reset consumer. */
        CHECK(is_fbo_float == new_float && syncs == 1 && !dirty_framebuffer);
        CHECK(begins == 2 && ends == 2 && finishes == 1);

        reset(old_float,old_float,0); scene_reset();
        CHECK(is_fbo_float == old_float && !syncs && !dirty_framebuffer && begins == 1);
        unsigned previous=begins; is_fbo_float=!old_float; scene_reset();
        CHECK(begins == previous && is_fbo_float == !old_float); /* No reset, no refresh. */
        reset(old_float,old_float,1); scene_reset();
        CHECK(is_fbo_float == old_float && syncs == 1 && dirty_framebuffer);

        reset(old_float,new_float,1); needs_end_scene=0;
        legacy_pool_size=sizeof legacy_backing; nested_legacy=1; scene_reset();
        CHECK(syncs == 1 && begins == 2 && finishes == 1 && fb.is_float == new_float);
        expect_selector();

        reset(old_float,new_float,1); fb.depthbuffer_state=0; fb.depthbuffer_ptr=NULL;
        fb.target=NULL; recover_depth=1; fail_create=1; scene_reset();
        CHECK(depths == 1 && finishes == 1 && creates == 2 && rt_records == 1);
        CHECK(scene_sizes[0] == 8 && scene_sizes[1] == 1 && begins == 1 && syncs == 1);
        CHECK(fb.target == &target && (fb.depthbuffer_state & DEPTHBUFFER_READY));

        reset(old_float,new_float,1); begin_result=-5; scene_reset();
        CHECK(begins == 1 && shadow_begins == 1 && dirty_framebuffer && syncs == 1);
        expect_selector(); /* No added retry/Finish on BeginScene failure. */
    }
    reset(1,1,0); active_write_fb=NULL; expected_selector=0; scene_reset();
    CHECK(is_fbo_float == 0 && is_rendering_display && !syncs && begins == 1);
    CHECK(!dirty_framebuffer && !creates && !depths && !finishes);
    printf("FBO float sync actual native bodies: fixed=%d checks=%u PASS\n", EXPECT_FIXED, checks);
    return 0;
}
