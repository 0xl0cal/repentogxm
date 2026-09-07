/* Actual native functions are regenerated from pinned source by the runner.
 * This proves ownership/retirement chronology, not GXM fences or target speed. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef RT_POLICY_SOURCE
#define RT_POLICY_SOURCE "isaac_fbo_rt_reuse.c"
#endif
#include RT_POLICY_SOURCE
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); \
} } while (0)
#if defined(HAVE_ISAAC_FBO_RT_INTERLUDE_REUSE) && HAVE_ISAAC_FBO_RT_INTERLUDE_REUSE
#define INTERLUDE 1
#else
#define INTERLUDE 0
#endif
#if defined(HAVE_ISAAC_FBO_RT_REUSE_LEASE) && HAVE_ISAAC_FBO_RT_REUSE_LEASE
#define LEASE 1
#else
#define LEASE 0
#endif
#define HAVE_ISAAC_FBO_RT_REUSE 1
#define HAVE_ISAAC_FBO_RT_SCENES 1
#define FRAME_PURGE_FREQ 4
#define FRAME_PURGE_LIST_SIZE 32
#define FRAME_PURGE_RENDERTARGETS_LIST_SIZE 32
#define BUFFERS_NUM 4
#define TEXTURES_NUM 16
#define TEXTURE_IMAGE_UNITS_NUM 2
#define THREAD_SAFE()
#define SET_GL_ERROR(x) do { native_error(); return; } while (0);
#define SET_GL_ERROR_WITH_VALUE(x,y) native_error();
#define GL_INVALID_VALUE 1
#define GL_INVALID_ENUM 2
#define GL_COLOR_ATTACHMENT0 3
#define GL_TRUE 1
#define GL_FALSE 0
#define DEPTHBUFFER_READY 1
#define DEPTHBUFFER_MISSING 0
#define TEX_VALID 1
#define TEX_UNINITIALIZED 2
#define TEX_UNUSED 0
#define SCE_GXM_MULTISAMPLE_NONE 0
#define SCE_GXM_TEXTURE_FORMAT_F16F16F16F16_RGBA 99
#define SCE_GXM_COLOR_SURFACE_LINEAR 1
#define SCE_GXM_COLOR_SURFACE_SCALE_NONE 2
#define SCE_GXM_COLOR_SURFACE_SCALE_MSAA_DOWNSCALE 3
#define SCE_GXM_OUTPUT_REGISTER_SIZE_64BIT 8
#define SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT 4
#define ISAAC_FBO_RT_SCENES() gxm_fbo_rt_size
#define VGL_ALIGN(x,n) (((x)+(n)-1)&~((n)-1))
typedef uintptr_t GLuint;
typedef int GLenum, GLsizei, SceGxmTextureFormat;
typedef struct SceGxmRenderTarget { unsigned id; int w,h,scenes; } SceGxmRenderTarget;
typedef struct { int w,h,format; void *data; } GxmTexture;
typedef struct { GxmTexture gxm_tex; int format,ref_counter,dirty,status; } texture;
typedef struct { void *depthData; } Depth;
typedef struct framebuffer {
    int width,height,stride,format,is_float,active,depthbuffer_state;
    SceGxmRenderTarget *target;
    texture *tex;
    Depth *depthbuffer_ptr;
    void *data;
    int colorbuffer;
} framebuffer;
typedef struct { GLuint tex_id[3]; } texture_unit;
static unsigned checks, cases, creations, destructions, live, peak, cycles, finishes;
static unsigned allocated[128], destroyed[128], retired[128], deadline[128], queued[128];
static unsigned next_id, depth_frees, surface_calls, texture_frees;
static Depth depth_slots[128];
static int frame_purge_idx, frame_purge_clean_idx, frame_elem_purge_idx, frame_rt_purge_idx;
static void *frame_purge_list[4][32];
static SceGxmRenderTarget *frame_rt_purge_list[4][32], targets[128];
static framebuffer framebuffers[BUFFERS_NUM], *active_write_fb, *active_read_fb, *in_use_framebuffer;
static texture texture_slots[TEXTURES_NUM];
static texture_unit texture_units[TEXTURE_IMAGE_UNITS_NUM];
static int dirty_framebuffer, gxm_fbo_rt_size, msaa_mode, fail_eight;
static void *gxm_context = (void *)(uintptr_t)1;
static void native_error(void) { CHECK(0); }
static void retire(SceGxmRenderTarget *target) {
    if (!target) return;
    unsigned id = target->id;
    CHECK(allocated[id] && !destroyed[id] && !retired[id]);
    retired[id] = 1;
    /* Independent of the policy's spare_bucket: actual mark/clean ring order. */
    deadline[id] = cycles + (unsigned)((frame_purge_idx - frame_purge_clean_idx + 4) % 4) + 1;
}
static void mark_rt_as_dirty(SceGxmRenderTarget *target) {
    CHECK(target && retired[target->id] && !queued[target->id]);
    CHECK(frame_rt_purge_idx < FRAME_PURGE_RENDERTARGETS_LIST_SIZE);
    queued[target->id] = (unsigned)frame_purge_idx + 1;
    frame_rt_purge_list[frame_purge_idx][frame_rt_purge_idx++] = target;
}
static void mark_as_dirty(void *data) {
    CHECK(data && frame_elem_purge_idx < FRAME_PURGE_LIST_SIZE);
    frame_purge_list[frame_purge_idx][frame_elem_purge_idx++] = data;
}
static void vgl_free(void *data) { CHECK(data); ++depth_frees; }
static void gpu_free_texture(texture *tex) { CHECK(tex); tex->status = TEX_UNUSED; ++texture_frees; }
static void vgl_log(const char *fmt, ...) { (void)fmt; CHECK(0); }
static int vglGetTexFormat(GxmTexture *tex) { return tex->format; }
static void vglGetTexSizes(GxmTexture *tex, int *w, int *h) { *w=tex->w; *h=tex->h; }
static void *vglGetTexData(GxmTexture *tex) { return tex->data; }
static int tex_format_to_bytespp(int fmt) { CHECK(fmt == 4); return 4; }
static int get_color_from_texture(int fmt) { return fmt; }
static void sceGxmColorSurfaceInit(int *out,int fmt,int kind,int scale,int reg,int w,int h,int stride,void *data) {
    CHECK(fmt==4 && kind==1 && (scale==2 || scale==3) && reg==4 && w>0 && h>0);
    CHECK(stride==VGL_ALIGN(w,8) && data); *out=1; ++surface_calls;
}
uint8_t vglIsaacSetupFboRenderTargetScenes(uint8_t size) { if(size)gxm_fbo_rt_size=size;return (uint8_t)gxm_fbo_rt_size; }
static int setup_render_target(SceGxmRenderTarget **out,int w,int h,int scenes) {
    if (scenes==8 && fail_eight) { fail_eight=0; return -1; }
    CHECK(next_id<128); unsigned id=next_id++;
    targets[id]=(SceGxmRenderTarget){id,w,h,scenes}; *out=&targets[id];
    allocated[id]=1; ++creations; ++live; if(live>peak)peak=live;
    return 0;
}
static int sceGxmDestroyRenderTarget(SceGxmRenderTarget *target) {
    CHECK(target && allocated[target->id] && !destroyed[target->id]);
    unsigned id=target->id;
    CHECK(retired[id] && deadline[id] <= cycles);
    for(unsigned i=0;i<BUFFERS_NUM;++i) CHECK(!framebuffers[i].active || framebuffers[i].target!=target);
    if(queued[id]) CHECK(queued[id] == (unsigned)frame_purge_clean_idx+1);
    destroyed[id]=1; queued[id]=retired[id]=0; ++destructions; --live;
    return 0;
}
#ifdef HAVE_ISAAC_NATIVE_RESOURCE_PROFILE
static void isaacNativeResourceRetireRT(SceGxmRenderTarget *target) { CHECK(target && !destroyed[target->id]); }
#endif
static void sceGxmFinish(void *context) { CHECK(context==gxm_context); ++finishes; }
#if LEASE
void vglIsaacFboRtDrainAfterFinish(void) {
    uintptr_t target=isaacFboRtRecoveryTake();
    if(target) sceGxmDestroyRenderTarget((SceGxmRenderTarget *)target);
}
#endif
#include "native_rt_interlude.h"

static void collect(void) { ++cycles; garbage_collector(0,NULL); }
static void collect_n(unsigned n) { while(n--)collect(); }
static void reset(void) {
    memset(&rt_reuse,0,sizeof(rt_reuse));
#if INTERLUDE
    memset(&interlude_profile,0,sizeof(interlude_profile));
#endif
    memset(framebuffers,0,sizeof(framebuffers)); memset(texture_slots,0,sizeof(texture_slots));
    texture_slots[0].gxm_tex.format=4;
    memset(texture_units,0,sizeof(texture_units)); memset(targets,0,sizeof(targets));
    memset(allocated,0,sizeof(allocated)); memset(destroyed,0,sizeof(destroyed));
    memset(retired,0,sizeof(retired)); memset(deadline,0,sizeof(deadline)); memset(queued,0,sizeof(queued));
    memset(frame_purge_list,0,sizeof(frame_purge_list)); memset(frame_rt_purge_list,0,sizeof(frame_rt_purge_list));
    creations=destructions=live=peak=cycles=finishes=0; next_id=1;
    depth_frees=surface_calls=texture_frees=0; frame_purge_idx=0; frame_purge_clean_idx=1;
    frame_elem_purge_idx=frame_rt_purge_idx=0; gxm_fbo_rt_size=8; msaa_mode=fail_eight=0;
    active_write_fb=active_read_fb=in_use_framebuffer=NULL; dirty_framebuffer=0;
    ++cases;
}
static framebuffer *new_fb(void) {
    GLuint id=0; glGenFramebuffers(1,&id); CHECK(id);
    return (framebuffer *)id;
}
static SceGxmRenderTarget *attach(framebuffer *fb,unsigned texid,int w,int h,int create) {
    CHECK(texid>0 && texid<TEXTURES_NUM);
    texture *tex=&texture_slots[texid];
    tex->gxm_tex=(GxmTexture){w,h,4,(void *)(uintptr_t)(0x1000+texid*256)};
    tex->format=4; tex->status=TEX_VALID;
    SceGxmRenderTarget *old=fb->target;
    int changed=fb->width!=w || fb->height!=h;
    unsigned depth_queued=(unsigned)frame_elem_purge_idx;
    if(old && changed) {
        depth_slots[old->id].depthData=(void *)(uintptr_t)(0x10000+old->id*256);
        fb->depthbuffer_ptr=&depth_slots[old->id]; fb->depthbuffer_state=DEPTHBUFFER_READY;
    }
    if(old && (fb->width!=w || fb->height!=h))retire(old);
    active_write_fb=active_read_fb=in_use_framebuffer=fb; dirty_framebuffer=0;
    unsigned surfaces=surface_calls;
    _glFramebufferTexture2D(fb,GL_COLOR_ATTACHMENT0,tex,texid);
    if(old && changed) CHECK(!fb->depthbuffer_ptr && !(fb->depthbuffer_state&DEPTHBUFFER_READY) && (unsigned)frame_elem_purge_idx==depth_queued+1);
    CHECK(fb->tex==tex && tex->ref_counter==1 && fb->width==w && fb->height==h);
    CHECK(fb->stride==VGL_ALIGN(w,8)*4 && fb->data==tex->gxm_tex.data && dirty_framebuffer);
    CHECK(surface_calls==surfaces+1);
    if(fb->target && fb->target!=old) {
        CHECK(retired[fb->target->id] && !queued[fb->target->id] && !destroyed[fb->target->id]);
        CHECK(cycles<deadline[fb->target->id]);
        retired[fb->target->id]=0;
    }
    if(!fb->target && create)CHECK(native_create()==0);
    return fb->target;
}
static SceGxmRenderTarget *pair(framebuffer **out) {
    framebuffer *fb=new_fb(); *out=fb;
    SceGxmRenderTarget *saved=attach(fb,1,784,472,1);
    attach(fb,2,480,272,1);
    CHECK(rt_reuse.spare.target==(uintptr_t)saved && !queued[saved->id]);
    return saved;
}
static void dispose(framebuffer *fb) {
    if(fb->active) { retire(fb->target); GLuint id=(uintptr_t)fb; glDeleteFramebuffers(1,&id); }
}
static void finish_all(void) {
    for(unsigned i=0;i<BUFFERS_NUM;++i)dispose(&framebuffers[i]);
    collect_n(8); CHECK(live==0 && creations==destructions && !rt_reuse.spare.target);
    CHECK(finishes==0);
}
static void tour(unsigned delay_before, unsigned delay_during, int restore) {
    reset(); framebuffer *fb; SceGxmRenderTarget *saved=pair(&fb), *a=fb->target;
    collect_n(delay_before);
    attach(fb,3,64,56,1); CHECK(queued[a->id]);
    SceGxmRenderTarget *c=fb->target;
    collect_n(delay_during);
    attach(fb,4,232,208,1); CHECK(queued[c->id] || destroyed[c->id]);
    SceGxmRenderTarget *d=fb->target;
    unsigned before=creations;
    int eligible=INTERLUDE && delay_before<4 && delay_before+delay_during<4;
    if(restore) {
        attach(fb,1,784,472,1); CHECK(queued[d->id]);
        CHECK(creations==before+(unsigned)!eligible);
        CHECK((fb->target==saved)==eligible);
    }
    if(!delay_before && !delay_during) {
        CHECK(peak==(restore && eligible ? 4u : (restore ? 5u:4u)));
        collect_n(3); CHECK(destructions==0);
        collect(); CHECK(destructions==(restore && eligible ? 3u:(restore ? 4u:3u)));
    }
#if INTERLUDE
    IsaacFboRtInterludeStats stats; vglIsaacFboRtInterludeProfileTake(NULL);
    vglIsaacFboRtInterludeProfileTake(&stats);
    CHECK(stats.restores==(unsigned)(restore&&eligible) && stats.saturation_mask==0);
    CHECK(stats.holds==(delay_before<4 ? (delay_before+delay_during<4 ? 2u:1u):0u));
    vglIsaacFboRtInterludeProfileTake(&stats); CHECK(!stats.holds && !stats.restores);
#endif
    finish_all();
}
static void refusal(unsigned kind) {
    reset(); framebuffer *fb; SceGxmRenderTarget *saved=pair(&fb);
    attach(fb,3,64,56,1);
    switch(kind) {
    case 0: /* Detach uses the actual native invalidation, not direct helper. */
        retire(fb->target); _glFramebufferTexture2D(fb,GL_COLOR_ATTACHMENT0,&texture_slots[0],0); CHECK(!rt_reuse.owner); break;
    case 1: /* Deleted/recreated address must never inherit a previous owner. */
        dispose(fb); CHECK(!rt_reuse.owner); CHECK(new_fb()==fb); CHECK(!rt_reuse.owner); break;
    case 2: { /* Actual attached texture deletion also revokes. */
        GLuint id=3; retire(fb->target); glDeleteTextures(1,&id); CHECK(!rt_reuse.owner); break;
    }
    case 3: msaa_mode=1; break;
    case 4: gxm_fbo_rt_size=1; break;
    case 5: attach(fb,4,128,128,1); break;
    case 6: /* Failed requested-eight create resolves pending only as invalid. */
        attach(fb,4,232,208,0); fail_eight=1; CHECK(native_create()==0); CHECK(fb->target->scenes==1); break;
    case 7: /* Native collection during pending creation expires on original bucket. */
        attach(fb,4,232,208,0); collect_n(4); CHECK(destroyed[saved->id]); CHECK(native_create()==0); break;
    default: CHECK(0);
    }
    attach(fb,1,784,472,1); CHECK(fb->target!=saved);
    finish_all();
}
static void ordinary_and_foreign(void) {
    reset(); framebuffer *fb; SceGxmRenderTarget *saved=pair(&fb);
    attach(fb,1,784,472,1); CHECK(fb->target==saved && creations==2);
    /* No spare after exact interlude restore; an unrelated owner cannot adopt. */
    attach(fb,3,64,56,1);
    framebuffer *foreign=new_fb(); attach(foreign,5,784,472,1);
    CHECK(foreign->target!=saved);
    finish_all();
}
static void malformed_switch(unsigned kind) {
    reset(); framebuffer *fb; SceGxmRenderTarget *saved=pair(&fb);
    attach(fb,3,64,56,1);
    uintptr_t owner=(uintptr_t)fb,current=(uintptr_t)fb->target,out=99,*output=&out;
    int old_w=64,old_h=56,new_w=784,new_h=472,compatible=1,bucket=frame_purge_idx;
    switch(kind) {
    case 0: owner=0; break;
    case 1: current++; break;
    case 2: old_w++; break;
    case 3: old_h++; break;
    case 4: new_w=480; new_h=272; break; /* Supported but NOT saved shape. */
    case 5: compatible=0; break;
    case 6: bucket=-1; break;
    case 7: bucket=4; break;
    case 8: output=NULL; break;
    default: CHECK(0);
    }
    CHECK(!isaacFboRtSwitch(owner,current,old_w,old_h,new_w,new_h,compatible,bucket,output));
    CHECK(out==99);
    /* Foreign/zero owner does not revoke the real owner. Explicitly invalidate
     * after the null-owner case before normal cleanup; no transfer occurred. */
    if(!owner)isaacFboRtInvalidate((uintptr_t)fb);
    attach(fb,1,784,472,1); CHECK(fb->target!=saved);
    finish_all();
}
int main(void) {
    for(unsigned before=0;before<=4;++before)
        for(unsigned during=0;during<=4;++during)
            for(int restore=0;restore<=1;++restore)tour(before,during,restore);
    for(unsigned kind=0;kind<8;++kind)refusal(kind);
    ordinary_and_foreign();
    for(unsigned kind=0;kind<9;++kind)malformed_switch(kind);
#if INTERLUDE
    interlude_profile.holds=interlude_profile.restores=UINT32_MAX;
    interlude_increment(&interlude_profile.holds,0); interlude_increment(&interlude_profile.restores,1);
    CHECK(interlude_profile.holds==UINT32_MAX && interlude_profile.restores==UINT32_MAX && interlude_profile.saturation_mask==3);
#endif
    printf("native cases=%u checks=%u interlude=%d lease=%d PASS\n",cases,checks,INTERLUDE,LEASE);
    return 0;
}
