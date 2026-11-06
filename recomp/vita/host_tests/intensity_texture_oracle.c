/* Fresh native bodies/callbacks, not a recreation of their pixel algorithms.
 * GXM/allocator/RT endpoints are explicit doubles: no GPU timing/ABI claim. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "intensity-enums.h"
typedef uint32_t GLenum, GLuint, SceGxmTextureFormat, SceGxmTextureFilter, SceGxmTextureAddrMode;
typedef int32_t GLint, GLsizei;
typedef uint8_t GLboolean;
typedef void GLvoid;
/* TEX_* and DEPTHBUFFER_* enums come from the pinned shared.h (enums header). */
#define THREAD_SAFE()
typedef struct {
    uint32_t width,height,format,kind,mip_count,u_mode,v_mode,min_filter,mag_filter,mip_filter,lod_bias;
    void *data;
} SceGxmTexture;
typedef struct texture {
    uint32_t status,mip_count,ref_counter,faces_counter,use_mips,dirty,overridden,last_frame;
    uint32_t format,u_mode,v_mode,min_filter,mag_filter,mip_filter,lod_bias;
    void *data,*palette_data;
    void (*write_cb)(void *,uint32_t);
    SceGxmTexture gxm_tex;
} texture;
typedef struct { void *depthData; } depthbuffer;
typedef struct {
    uint32_t width,height,stride,format,depthbuffer_state,is_float,colorbuffer;
    void *data,*target;
    texture *tex;
    depthbuffer *depthbuffer_ptr;
} framebuffer;
static texture texture_slots[TEXTURES_NUM];
static framebuffer *in_use_framebuffer,*active_write_fb,*active_read_fb;
/* Only the member the extracted glDeleteTextures reads (stock: tex_id[3]). */
typedef struct { GLuint tex_id[3]; } texture_unit;
static texture_unit texture_units[COMBINED_TEXTURE_IMAGE_UNITS_NUM];
static uint32_t dirty_framebuffer,msaa_mode,scissor_test_state,dirty_scissor_state;
static int unpack_row_len;
static uint32_t vgl_framecount=20, vgl_error,allocs,frees,retires,mip_calls;
static uint32_t fail_alloc,descriptor_calls,fail_descriptor,color_calls;
static size_t live_bytes,peak_bytes,last_alloc;
static void *retired[32];
static unsigned retired_count;
#define VGL_ALIGN(v,a) (((v)+(a)-1U)&~((a)-1U))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define SET_GL_ERROR(e) { vgl_error=(e); return; }
#define SET_GL_ERROR_WITH_VALUE(e,v) { (void)(v); vgl_error=(e); return; }
/* Counts every "Isaac intensity:" tty line by kind and keeps the last one;
 * nothing is printed. */
static unsigned armed_receipts,skip_receipts,reject_receipts;
static unsigned tty_lines,tty_upload,tty_failed,tty_deopt,tty_deopt_failed,tty_redefine;
static char last_receipt[256];
static int sceClibPrintf(const char *f, ...) {
    if(strncmp(f,"Isaac intensity: ",17)) return 0;
    va_list ap;va_start(ap,f);vsnprintf(last_receipt,sizeof last_receipt,f,ap);va_end(ap);
    ++tty_lines;
    if(!strncmp(f,"Isaac intensity: armed ",23)) ++armed_receipts;
    else if(!strncmp(f,"Isaac intensity: phase=skip ",28)) ++skip_receipts;
    else if(!strncmp(f,"Isaac intensity: phase=reject ",30)) ++reject_receipts;
    else if(!strncmp(f,"Isaac intensity: phase=upload ",30)) ++tty_upload;
    else if(!strncmp(f,"Isaac intensity: phase=upload-failed ",37)) ++tty_failed;
    else if(!strncmp(f,"Isaac intensity: phase=deopt ",29)) ++tty_deopt;
    else if(!strncmp(f,"Isaac intensity: phase=deopt-failed ",36)) ++tty_deopt_failed;
    else if(!strncmp(f,"Isaac intensity: phase=redefine-rejected ",41)) ++tty_redefine;
    else assert(!"unclassified Isaac intensity line");
    return 0;
}
/* The runtime reader (extracted from gl_vita_backend.c) logs here. */
static unsigned native_lines,native_armed;
static char native_line[512];
void isaac_vita_log(const char *f, ...) {
    va_list ap;va_start(ap,f);int n=vsnprintf(native_line,sizeof native_line,f,ap);va_end(ap);
    assert(n>0&&n<384); /* platform.c isaac_vita_log body[384] */
    ++native_lines;
    if(strstr(native_line,"KAGE VITA INTENSITY: armed ")==native_line) ++native_armed;
}
static void vgl_log(const char *f, ...) { (void)f; }
typedef struct { size_t size; unsigned char guard[16]; } Allocation;
static void *gpu_alloc_mapped_for_gpu(size_t size) {
    ++allocs; last_alloc=size;
    if(fail_alloc) return NULL;
    Allocation *a=malloc(sizeof(*a)+size+16); assert(a);
    a->size=size; memset(a->guard,0xA5,16); memset(a+1,0xCD,size+16);
    live_bytes+=size; if(live_bytes>peak_bytes) peak_bytes=live_bytes;
    return a+1;
}
static void vgl_free(void *p) {
    if(!p) return;
    Allocation *a=(Allocation *)p-1;
    for(unsigned i=0;i<16;++i) assert(a->guard[i]==0xA5 && ((uint8_t *)p)[a->size+i]==0xCD);
    live_bytes-=a->size; ++frees; free(a);
}
static void mark_as_dirty(void *p) { assert(retired_count<32); retired[retired_count++]=p; ++retires; }
static void mark_rt_as_dirty(void *p) { (void)p; }
static void *isaacNativeResourceRetireDepth(void *p) { return p; }
static void gpu_free_texture(texture *t);
static uint32_t vglGetTexFormat(const SceGxmTexture *t) { return t->format; }
static void *vglGetTexData(const SceGxmTexture *t) { return t->data; }
static void vglGetTexSizes(const SceGxmTexture *t,uint32_t *w,uint32_t *h) { *w=t->width; *h=t->height; }
static uint32_t sceGxmTextureGetType(const SceGxmTexture *t) { return t->kind; }
static int descriptor_result(void) { ++descriptor_calls; return descriptor_calls==fail_descriptor?-1:0; }
static int sceGxmTextureInitLinear(SceGxmTexture *t,void *p,uint32_t f,uint32_t w,uint32_t h,uint32_t m) {
    if(descriptor_result()) return -1;
    memset(t,0,sizeof(*t)); t->data=p;t->format=f;t->width=w;t->height=h;t->mip_count=m;t->kind=SCE_GXM_TEXTURE_LINEAR;return 0;
}
static int sceGxmTextureSetData(SceGxmTexture *t,void *p) { if(descriptor_result()) return -1;t->data=p;return 0; }
static int sceGxmTextureSetFormat(SceGxmTexture *t,uint32_t f) { if(descriptor_result()) return -1;t->format=f;return 0; }
#define SETTER(n,field) \
static int sceGxmTextureSet##n(SceGxmTexture *t,uint32_t v) { if(descriptor_result()) return -1;t->field=v;return 0; }
SETTER(UAddrMode,u_mode) SETTER(VAddrMode,v_mode) SETTER(MinFilter,min_filter) SETTER(MagFilter,mag_filter)
SETTER(MipFilter,mip_filter) SETTER(LodBias,lod_bias) SETTER(MipmapCount,mip_count)
#define VGL_SET(n,field) static void vglSetTex##n(SceGxmTexture *t,uint32_t v) { t->field=v; }
VGL_SET(UMode,u_mode) VGL_SET(VMode,v_mode) VGL_SET(MinFilter,min_filter) VGL_SET(MagFilter,mag_filter)
VGL_SET(MipFilter,mip_filter) VGL_SET(LodBias,lod_bias) VGL_SET(MipmapCount,mip_count)
static void vglSetTexGammaMode(SceGxmTexture *t,uint32_t v) { (void)t;(void)v; }
static void vglSetTexPalette(SceGxmTexture *t,void *p) { (void)t;(void)p; }
static void vglInitLinearTexture(SceGxmTexture *t,void *p,uint32_t f,uint32_t w,uint32_t h,uint32_t m) {
    assert(sceGxmTextureInitLinear(t,p,f,w,h,m)==0);
}
static void sceGxmTextureSetWidth(SceGxmTexture *t,uint32_t w) { t->width=w; }
static void sceGxmTextureSetHeight(SceGxmTexture *t,uint32_t h) { t->height=h; }
static void vgl_fast_memcpy(void *d,const void *s,size_t n) { assert(d&&s);memcpy(d,s,n); }
#define vgl_memset memset
#define vglMalloc gpu_alloc_mapped_for_gpu
static uint32_t nearest_po2(uint32_t n) { uint32_t r=1;while(r<n)r*=2;return r; }
static uint8_t tex_format_to_bytespp(uint32_t f) {
    if(f==SCE_GXM_TEXTURE_FORMAT_U8_RRRR || f==SCE_GXM_TEXTURE_FORMAT_U8_R111 || f==SCE_GXM_TEXTURE_FORMAT_U8_R ||
       f==SCE_GXM_TEXTURE_FORMAT_L8 || f==SCE_GXM_TEXTURE_FORMAT_P8_ABGR) return 1;
    if(f==SCE_GXM_TEXTURE_FORMAT_U8U8U8_BGR || f==SCE_GXM_TEXTURE_FORMAT_U8U8U8_RGB) return 3;
    if(f==SCE_GXM_TEXTURE_FORMAT_U8U8_GR || f==SCE_GXM_TEXTURE_FORMAT_A8L8 || f==SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB ||
       f==SCE_GXM_TEXTURE_FORMAT_U4U4U4U4_RGBA || f==SCE_GXM_TEXTURE_FORMAT_U5U5U5U1_RGBA) return 2;
    return 4;
}
static void *color_table;
static void write_rgba8888_inlined(void *p,uint32_t v) { memcpy(p,&v,4); }
static void gpu_alloc_compressed_texture(int l,int w,int h,uint32_t f,int s,const void *p,texture *t,int b,int u) {
    (void)l;(void)w;(void)h;(void)f;(void)s;(void)p;(void)t;(void)b;(void)u;assert(0);
}
static void gpu_alloc_mipmaps(int level,texture *t) {
    assert(level>0); assert(t->format==SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    assert(t->gxm_tex.data==t->data); ++mip_calls;
    /* Boundary double only: stock's mip-generation algorithm is not tested. */
}
static void _glTexImage2D_CubeIMPL(texture *t,int l,int f,int w,int h,uint32_t fmt,uint32_t ty,const void *p,int face) {
    (void)t;(void)l;(void)f;(void)w;(void)h;(void)fmt;(void)ty;(void)p;(void)face;assert(0);
}
static uint32_t get_color_from_texture(uint32_t f) { assert(f==SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);return f; }
static void sceGxmColorSurfaceInit(uint32_t *s,uint32_t f,uint32_t a,uint32_t b,uint32_t c,uint32_t w,uint32_t h,uint32_t stride,void *p) {
    (void)a;(void)b;(void)c;(void)w;(void)h;(void)stride;assert(p);*s=f;++color_calls;
}
#include "intensity-generated.h"
static void gpu_free_texture(texture *t) { gpu_free_texture_data(t);t->status=TEX_UNUSED; }

_Static_assert(GL_VITA_INTENSITY_WORDS==ISAAC_INTENSITY_WORDS,"runtime reader and 0037 word count differ");
static uint32_t word(unsigned i) { uint32_t w[ISAAC_INTENSITY_WORDS];(void)vglIsaacIntensitySnapshot(UINT32_MAX,w);return w[i]; }
static uint32_t durable_gen;
static unsigned durable_changes,durable_multi; /* reads that moved word 0; of them, by more than 1 */
/* After every step: run the REAL runtime reader (extracted from
 * gl_vita_backend.c) and check its native.log line and the 0037 words
 * against the tty lines printed so far, and live storage against the slots. */
static void durable(void) {
    const unsigned lines=native_lines,armed=native_armed;
    gl_vita_intensity_receipt();
    uint32_t w[ISAAC_INTENSITY_WORDS];
    const uint32_t gen=vglIsaacIntensitySnapshot(UINT32_MAX,w);
    assert(gen==tty_lines);
    uint32_t up=0,fail=0,skip=0,reject=0,pages=0,bytes=0;
    for(unsigned s=1;s<=4;++s) {
        up+=w[ISAAC_INTENSITY_W_UPLOAD_BASE+s];fail+=w[ISAAC_INTENSITY_W_FAILED_BASE+s];
        skip+=w[ISAAC_INTENSITY_W_SKIP_BASE+s];reject+=w[ISAAC_INTENSITY_W_REJECT_BASE+s];
    }
    assert(up==tty_upload&&w[ISAAC_INTENSITY_W_U8]+w[ISAAC_INTENSITY_W_A8L8]==up&&fail==tty_failed);
    assert(w[ISAAC_INTENSITY_W_DEOPT]==tty_deopt&&w[ISAAC_INTENSITY_W_DEOPT_FAILED]==tty_deopt_failed);
    assert(w[ISAAC_INTENSITY_W_REDEFINE_REJECTED]==tty_redefine);
    assert(skip>=skip_receipts&&reject>=reject_receipts); /* every event, printed or capped */
    for(unsigned i=0;i<TEXTURES_NUM;++i) if(isaac_intensity_state[i]&ISAAC_INTENSITY_COMPACT) {
        const texture *t=&texture_slots[i];
        assert(t->data&&(t->format==SCE_GXM_TEXTURE_FORMAT_U8_RRRR||t->format==SCE_GXM_TEXTURE_FORMAT_A8L8));
        ++pages;bytes+=t->gxm_tex.width*t->gxm_tex.height*(t->format==SCE_GXM_TEXTURE_FORMAT_A8L8?2U:1U);
    }
    assert(w[ISAAC_INTENSITY_W_LIVE_PAGES]==pages&&w[ISAAC_INTENSITY_W_LIVE_BYTES]==bytes);
    if(gen==durable_gen) { assert(native_lines==lines);return; }
    /* One "armed" line at the first change, then one line per change. */
    assert(native_armed==armed+(durable_gen==0)&&native_lines==lines+1+(durable_gen==0));
    char want[400],phase[32]="",size[16]="",detail[16]="";const char *p;
    if(!strncmp(last_receipt,"Isaac intensity: armed ",23)) snprintf(want,sizeof want,"gen=%u last=armed up=",(unsigned)gen);
    else {
        assert((p=strstr(last_receipt,"phase="))&&sscanf(p,"phase=%31s",phase)==1);
        if((p=strstr(last_receipt," size="))) assert(sscanf(p," size=%15s",size)==1);
        if((p=strstr(last_receipt," storage="))||(p=strstr(last_receipt," reason="))) assert(sscanf(strchr(p,'=')+1,"%15s",detail)==1);
        /* deopt-failed/redefine-rejected tty lines carry no size: prefix only. */
        snprintf(want,sizeof want,"gen=%u last=%s%s%s%s%s%s",(unsigned)gen,phase,*size?":":"",size,
            *detail?":":"",detail,*size?" up=":"");
    }
    assert(strstr(native_line,"KAGE VITA INTENSITY: gen=")==native_line&&strstr(native_line,want));
    snprintf(want,sizeof want," up=%u/%u/%u/%u fail=%u/%u/%u/%u skip=%u/%u/%u/%u rej=%u/%u/%u/%u u8=%u a8l8=%u deopt=%u dfail=%u redef=%u live=%u/%u",
        w[2],w[3],w[4],w[5],w[6],w[7],w[8],w[9],w[10],w[11],w[12],w[13],w[14],w[15],w[16],w[17],
        w[18],w[19],w[20],w[21],w[22],w[23],w[24]);
    assert(strlen(native_line)>=strlen(want)&&!strcmp(native_line+strlen(native_line)-strlen(want),want));
    if(gen-durable_gen>1U) ++durable_multi;
    ++durable_changes;durable_gen=gen;
}

static texture *fresh(unsigned slot) {
    assert(slot<TEXTURES_NUM);texture *t=&texture_slots[slot];memset(t,0,sizeof(*t));
    t->status=TEX_UNINITIALIZED;t->mip_count=1;t->last_frame=OBJ_NOT_USED;
    t->min_filter=t->mag_filter=SCE_GXM_TEXTURE_FILTER_LINEAR;
    t->mip_filter=SCE_GXM_TEXTURE_MIP_FILTER_DISABLED;
    t->u_mode=t->v_mode=SCE_GXM_TEXTURE_ADDR_REPEAT;t->lod_bias=GL_MAX_TEXTURE_LOD_BIAS;
    return t;
}
static void drains(void) { while(retired_count) vgl_free(retired[--retired_count]);durable(); }
static void cleanup(texture *t) { t->ref_counter=0;gpu_free_texture_data(t);drains(); }
static void exact(texture *t,const uint8_t *source,size_t pixels) {
    assert(t->status==TEX_VALID&&t->gxm_tex.data==t->data);
    for(size_t i=0;i<pixels;++i) {
        const uint8_t *stored=(const uint8_t *)t->data;
        if(t->format==SCE_GXM_TEXTURE_FORMAT_A8L8) {
            assert(stored[2*i]==source[4*i] && stored[2*i+1]==source[4*i+3]);
            uint32_t decoded=read_la88(stored+2*i);
            assert(decoded==read_rgba8888(source+4*i));
        } else {
            assert(t->format==SCE_GXM_TEXTURE_FORMAT_U8_RRRR);
            uint8_t v=stored[i];
            for(unsigned c=0;c<4;++c) assert(source[4*i+c]==v);
        }
    }
}
static void equal_rgba(texture *t,const uint8_t *source,size_t pixels) {
    assert(t->format==SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    assert(t->gxm_tex.data==t->data);assert(!memcmp(t->data,source,pixels*4));
}
static void image(texture *t,int w,int h,const void *p) {
    candidate_glTexImage2D_FlatIMPL(t,0,GL_RGBA,w,h,GL_RGBA,GL_UNSIGNED_BYTE,p);durable();
}
static unsigned cases;
static void gray_suite(void) {
    const unsigned w=512,h=512,pixels=w*h;
    uint8_t *gray=malloc(pixels*4U),*snapshot=malloc(pixels*4U);assert(gray&&snapshot);
    /* Every (L,A) pair occurs four times, including gray RGB under alpha=0.
     * Callback packing and expansion are ACTUAL freshly extracted vitaGL
     * functions. Never substitute red for alpha or gamma-convert luminance. */
    for(unsigned i=0;i<pixels;++i) {
        gray[i*4]=gray[i*4+1]=gray[i*4+2]=(uint8_t)(i>>8);
        gray[i*4+3]=(uint8_t)i;
        uint8_t la[2];write_ra88(la,read_rgba8888(gray+i*4));
        assert(la[0]==gray[i*4]&&la[1]==gray[i*4+3]);
        assert(read_la88(la)==read_rgba8888(gray+i*4));
    }
    memcpy(snapshot,gray,pixels*4U);
    texture *rgba=fresh(30),*native_la=fresh(31),*t=fresh(32);
    reference_glTexImage2D_FlatIMPL(rgba,0,GL_RGBA,w,h,GL_RGBA,GL_UNSIGNED_BYTE,gray);
    reference_glTexImage2D_FlatIMPL(native_la,0,GL_LUMINANCE_ALPHA,w,h,GL_RGBA,GL_UNSIGNED_BYTE,gray);
    size_t before=live_bytes;image(t,w,h,gray);
    assert(t->format==SCE_GXM_TEXTURE_FORMAT_A8L8&&live_bytes-before==pixels*2U);
    assert(native_la->format==t->format&&!memcmp(native_la->data,t->data,pixels*2U));
    exact(t,gray,pixels);exact(native_la,gray,pixels);equal_rgba(rgba,gray,pixels);
    /* Pure rational filtering equivalence after native decoding; format-
     * specific hardware rounding/filter order still requires a device gate. */
    const unsigned weights[]={0,1,17,128,255,256};
    for(unsigned i=0;i<65536U;++i) {
        unsigned j=65535U-i;
        uint32_t left=read_la88((uint8_t *)t->data+i*2U),right=read_la88((uint8_t *)t->data+j*2U);
        const uint8_t *a=(const uint8_t *)&left,*b=(const uint8_t *)&right;
        for(unsigned k=0;k<sizeof weights/sizeof weights[0];++k)
            for(unsigned c=0;c<4;++c)
                assert(a[c]*weights[k]+b[c]*(256U-weights[k]) ==
                       gray[i*4+c]*weights[k]+gray[j*4+c]*(256U-weights[k]));
    }
    cleanup(rgba);cleanup(native_la);++cases;
    /* Initial LA failure: no descriptor/storage published. */
    texture *initial=fresh(33),saved_initial=*initial;fail_alloc=1;
    image(initial,w,h,gray);assert(vgl_error==GL_OUT_OF_MEMORY&&!memcmp(initial,&saved_initial,sizeof saved_initial));
    fail_alloc=0;vgl_error=0;++cases;
    texture saved=*t;before=live_bytes;fail_alloc=1;image(t,w,h,gray);
    assert(vgl_error==GL_OUT_OF_MEMORY&&live_bytes==before&&!memcmp(t,&saved,sizeof saved));
    fail_alloc=0;vgl_error=0;exact(t,gray,pixels);++cases;
    for(unsigned d=1;d<=8;++d) {
        descriptor_calls=0;fail_descriptor=d;before=live_bytes;
        image(t,w,h,gray);assert(vgl_error==GL_INVALID_OPERATION&&live_bytes==before&&!memcmp(t,&saved,sizeof saved));
        exact(t,gray,pixels);++cases;
    }
    fail_descriptor=0;vgl_error=0;
    /* Recent LA image retirement retains its actual bytes until native purge. */
    void *old=t->data;t->last_frame=vgl_framecount;unsigned r=retires;
    image(t,w,h,gray);assert(t->data!=old&&retires==r+1);
    for(unsigned i=0;i<pixels;++i) {
        assert(((uint8_t *)old)[2*i]==gray[4*i]);assert(((uint8_t *)old)[2*i+1]==gray[4*i+3]);
    }
    exact(t,gray,pixels);drains();++cases;
    candidate_glTexParameteri(t,GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    candidate_glTexParameteri(t,GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_MIRRORED_REPEAT);
    assert(t->format==SCE_GXM_TEXTURE_FORMAT_A8L8);saved=*t;fail_alloc=1;
    candidate_glTexParameteri(t,GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
    assert(vgl_error==GL_OUT_OF_MEMORY&&!memcmp(t,&saved,sizeof saved));fail_alloc=0;vgl_error=0;
    candidate_glTexParameteri(t,GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
    equal_rgba(t,gray,pixels);cleanup(t);++cases;
    /* Deopt both SDK failure positions, then native colored subimage. */
    t=fresh(34);image(t,w,h,gray);saved=*t;
    uint8_t patch[4]={19,41,7,93};fail_alloc=1;
    candidate_glTexSubImage2D(t,GL_TEXTURE_2D,0,3,2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,patch);
    assert(vgl_error==GL_OUT_OF_MEMORY&&!memcmp(t,&saved,sizeof saved));fail_alloc=0;vgl_error=0;
    for(unsigned d=1;d<=2;++d) {
        descriptor_calls=0;fail_descriptor=d;before=live_bytes;
        candidate_glTexSubImage2D(t,GL_TEXTURE_2D,0,3,2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,patch);
        assert(vgl_error==GL_INVALID_OPERATION&&live_bytes==before&&!memcmp(t,&saved,sizeof saved));
        exact(t,gray,pixels);++cases;
    }
    fail_descriptor=0;vgl_error=0;
    candidate_glTexSubImage2D(t,GL_TEXTURE_2D,0,3,2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,patch);
    memcpy(gray+4U*(2*w+3),patch,4);equal_rgba(t,gray,pixels);
    memcpy(gray,snapshot,pixels*4U);cleanup(t);++cases;
    /* Attachment failure cannot cache a stale LA GPU pointer. */
    t=fresh(35);image(t,w,h,gray);saved=*t;
    framebuffer fb={0},fb_before=fb;unsigned cc=color_calls;fail_alloc=1;
    candidate_attach(&fb,GL_COLOR_ATTACHMENT0,t,35);
    assert(vgl_error==GL_OUT_OF_MEMORY&&!memcmp(t,&saved,sizeof saved)&&!memcmp(&fb,&fb_before,sizeof fb)&&color_calls==cc);
    fail_alloc=0;vgl_error=0;
    for(unsigned d=1;d<=2;++d) {
        descriptor_calls=0;fail_descriptor=d;before=live_bytes;
        candidate_attach(&fb,GL_COLOR_ATTACHMENT0,t,35);
        assert(vgl_error==GL_INVALID_OPERATION&&live_bytes==before&&color_calls==cc);
        assert(!memcmp(t,&saved,sizeof saved)&&!memcmp(&fb,&fb_before,sizeof fb));++cases;
    }
    fail_descriptor=0;vgl_error=0;
    candidate_attach(&fb,GL_COLOR_ATTACHMENT0,t,35);
    assert(fb.data==t->data&&fb.data==t->gxm_tex.data&&fb.tex==t&&t->ref_counter==1);
    equal_rgba(t,gray,pixels);cleanup(t);++cases;
    /* Native mip allocator stays unentered on a compact later definition. */
    t=fresh(36);image(t,w,h,gray);saved=*t;unsigned m=mip_calls,a=allocs;
    candidate_glTexImage2D_FlatIMPL(t,1,GL_RGBA,w/2,h/2,GL_RGBA,GL_UNSIGNED_BYTE,gray);
    assert(vgl_error==GL_INVALID_OPERATION&&mip_calls==m&&allocs==a&&!memcmp(t,&saved,sizeof saved));
    exact(t,gray,pixels);cleanup(t);vgl_error=0;++cases;
    /* Full colored definition: checked replacement, not stock free-before-OOM. */
    t=fresh(37);image(t,w,h,gray);saved=*t;gray[0]=251;fail_alloc=1;image(t,w,h,gray);
    assert(vgl_error==GL_OUT_OF_MEMORY&&!memcmp(t,&saved,sizeof saved));fail_alloc=0;
    for(unsigned d=1;d<=8;++d) {
        descriptor_calls=0;fail_descriptor=d;before=live_bytes;
        image(t,w,h,gray);assert(vgl_error==GL_INVALID_OPERATION&&live_bytes==before&&!memcmp(t,&saved,sizeof saved));++cases;
    }
    fail_descriptor=0;vgl_error=0;image(t,w,h,gray);equal_rgba(t,gray,pixels);
    memcpy(gray,snapshot,pixels*4U);cleanup(t);++cases;
    /* Transactional format transitions in both directions, one allocation
     * each, without changing source/luma or keeping a full RGBA shadow. */
    t=fresh(38);image(t,w,h,gray);
    for(unsigned i=0;i<pixels;++i) gray[4*i+3]=gray[4*i];
    image(t,w,h,gray);assert(t->format==SCE_GXM_TEXTURE_FORMAT_U8_RRRR);exact(t,gray,pixels);
    memcpy(gray,snapshot,pixels*4U);image(t,w,h,gray);
    assert(t->format==SCE_GXM_TEXTURE_FORMAT_A8L8);exact(t,gray,pixels);cleanup(t);++cases;
    /* fresh() is a fixture re-init, NOT glGenTextures: without a new name the
     * attached/excluded object stays RGBA. Real name reuse: name_suite(). */
    t=fresh(35);image(t,w,h,gray);equal_rgba(t,gray,pixels);cleanup(t);++cases;
    t=fresh(39);image(t,w,h,gray);cleanup(t);t=fresh(39);image(t,w,h,gray);exact(t,gray,pixels);cleanup(t);++cases;
    /* Gray transparent RGB is preserved; colored transparent RGB must reject. */
    for(unsigned c=0;c<3;++c) {
        t=fresh(40+c);gray[c]=199;
        image(t,w,h,gray);equal_rgba(t,gray,pixels);cleanup(t);
        memcpy(gray,snapshot,pixels*4U);++cases;
    }
    /* Expanded RGBA replacement with nonaligned width has deterministic zero
     * padding, with no read past the 511-wide source row. */
    t=fresh(43);image(t,w,h,gray);memset(gray,0x63,511U*3U*4U);
    image(t,511,3,gray);assert(t->format==SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    for(unsigned y=0;y<3;++y) {
        assert(!memcmp((uint8_t *)t->data+y*512U*4U,gray+y*511U*4U,511U*4U));
        for(unsigned c=0;c<4;++c) assert(((uint8_t *)t->data)[(y*512U+511U)*4U+c]==0);
    }
    cleanup(t);++cases;
    free(gray);free(snapshot);
}
/* Exclusion belongs to the GL texture OBJECT. Real extracted
 * glGenTextures/glDeleteTextures and the real _glFramebufferTexture2D detach
 * (tex_id 0) drive every step; stock hands out the LOWEST unused slot, so a
 * deleted object's slot is the next name. An attached object that stock keeps
 * (TEX_VALID + dirty) is never handed out and stays RGBA until the FBO lets it
 * go (N4); one that stock frees at once (sole reference = active FBO) is (N4b). */
static GLuint gen1(void) { GLuint id=0;candidate_glGenTextures(1,&id);assert(id&&id<TEXTURES_NUM);return id; }
static void del1(GLuint id) { candidate_glDeleteTextures(1,&id);drains(); }
static int skipped(const char *reason,unsigned before) {
    char needle[48];snprintf(needle,sizeof needle," reason=%s ",reason);
    return skip_receipts==before+1&&strstr(last_receipt,"phase=skip size=512x512 ")&&strstr(last_receipt,needle);
}
static void name_suite(const uint8_t *gray) {
    const unsigned w=512,h=512,pixels=w*h;
    uint8_t *color=malloc(256U*256U*4U);assert(color);
    for(unsigned i=0;i<256U*256U;++i){color[4*i]=200;color[4*i+1]=10;color[4*i+2]=30;color[4*i+3]=255;}
    /* N1: ordinary coloured texture; the same object stays RGBA on a later
     * equal-channel definition; after delete, the next name compacts. */
    GLuint id=gen1();texture *t=&texture_slots[id];unsigned s;
    assert(t->status==TEX_UNINITIALIZED&&isaac_intensity_state[id]==0);
    image(t,256,256,color);
    assert(t->format==SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR&&(isaac_intensity_state[id]&ISAAC_INTENSITY_EXCLUDED));
    s=skip_receipts;image(t,w,h,gray);equal_rgba(t,gray,pixels);assert(skipped("excluded",s));
    del1(id);assert(t->status==TEX_UNUSED&&!t->data&&(isaac_intensity_state[id]&ISAAC_INTENSITY_EXCLUDED));
    assert(gen1()==id&&isaac_intensity_state[id]==0);
    image(t,w,h,gray);exact(t,gray,pixels);++cases;
    del1(id);assert(t->status==TEX_UNUSED&&isaac_intensity_state[id]==0);
    /* N2: NULL-data render-target definition, deleted, slot reused. */
    assert(gen1()==id);image(t,w,h,NULL);
    assert(t->format==SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR&&(isaac_intensity_state[id]&ISAAC_INTENSITY_EXCLUDED));
    del1(id);assert(gen1()==id&&isaac_intensity_state[id]==0);
    image(t,w,h,gray);exact(t,gray,pixels);++cases;del1(id);
    /* N3: a non-filter/wrap parameter before the first definition keeps THIS
     * object RGBA (bounded skip receipt); deleting it from TEX_UNINITIALIZED
     * and regenerating the name starts clean. */
    assert(gen1()==id);candidate_glTexParameteri(t,GL_TEXTURE_2D,GL_TEXTURE_LOD_BIAS,0);
    assert(t->status==TEX_UNINITIALIZED&&(isaac_intensity_state[id]&ISAAC_INTENSITY_EXCLUDED));
    del1(id);assert(t->status==TEX_UNUSED);assert(gen1()==id&&isaac_intensity_state[id]==0);
    candidate_glTexParameteri(t,GL_TEXTURE_2D,GL_TEXTURE_LOD_BIAS,0);
    s=skip_receipts;image(t,w,h,gray);equal_rgba(t,gray,pixels);assert(skipped("excluded",s));
    del1(id);assert(gen1()==id);image(t,w,h,gray);exact(t,gray,pixels);++cases;
    /* N4: compact page attached (deopt + exclude), deleted while attached:
     * stock keeps it TEX_VALID+dirty, glGenTextures skips it, it stays RGBA.
     * The real detach frees it; then its slot is the next name, clean. */
    framebuffer fb={0};
    candidate_attach(&fb,GL_COLOR_ATTACHMENT0,t,id);
    assert(fb.tex==t&&t->ref_counter==1&&t->format==SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    assert(isaac_intensity_state[id]&ISAAC_INTENSITY_EXCLUDED);equal_rgba(t,gray,pixels);
    assert(!active_read_fb&&!active_write_fb);del1(id);
    assert(t->status==TEX_VALID&&t->dirty&&t->ref_counter==1&&t->data);
    GLuint other=gen1();assert(other>id);texture *u=&texture_slots[other];
    assert(isaac_intensity_state[other]==0&&(isaac_intensity_state[id]&ISAAC_INTENSITY_EXCLUDED));
    equal_rgba(t,gray,pixels);
    image(u,w,h,gray);exact(u,gray,pixels);
    candidate_attach(&fb,GL_COLOR_ATTACHMENT0,&texture_slots[0],0);
    drains();assert(!fb.tex&&t->status==TEX_UNUSED&&!t->data&&t->ref_counter==0);
    assert(gen1()==id&&isaac_intensity_state[id]==0);
    image(t,w,h,gray);exact(t,gray,pixels);++cases;
    del1(id);del1(other);
    assert(isaac_intensity_state[id]==0&&isaac_intensity_state[other]==0);
    /* N4b: deleted while its only reference is the ACTIVE
     * write (k=0) or read (k=1) FBO, stock frees it at once (gpu_free_texture,
     * fb->tex=NULL) and leaves ref_counter=1. The slot is the next name;
     * glGenTextures zeroes counter and mark, the grey page compacts, and the
     * old FBO's later detach no longer reaches the slot. */
    for(unsigned k=0;k<2;++k) {
        framebuffer active={0};
        assert(gen1()==id);image(t,w,h,gray);exact(t,gray,pixels);
        candidate_attach(&active,GL_COLOR_ATTACHMENT0,t,id);
        assert(active.tex==t&&t->ref_counter==1&&t->format==SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
        assert(isaac_intensity_state[id]==ISAAC_INTENSITY_EXCLUDED);equal_rgba(t,gray,pixels);
        if(k) active_read_fb=&active; else active_write_fb=&active;
        del1(id);
        assert(t->status==TEX_UNUSED&&!t->data&&t->ref_counter==1&&!active.tex);
        assert(isaac_intensity_state[id]==ISAAC_INTENSITY_EXCLUDED);
        active_read_fb=active_write_fb=NULL;
        assert(gen1()==id&&isaac_intensity_state[id]==0&&t->ref_counter==0);
        image(t,w,h,gray);exact(t,gray,pixels);
        candidate_attach(&active,GL_COLOR_ATTACHMENT0,&texture_slots[0],0);
        assert(!active.tex&&t->ref_counter==0&&t->status==TEX_VALID);exact(t,gray,pixels);
        del1(id);assert(t->status==TEX_UNUSED&&isaac_intensity_state[id]==0);++cases;
    }
    /* N5: one glGenTextures call resets every name it hands out. */
    isaac_intensity_state[id]=isaac_intensity_state[other]=ISAAC_INTENSITY_EXCLUDED;
    GLuint pair[2]={0,0};candidate_glGenTextures(2,pair);
    assert(pair[0]==id&&pair[1]==other&&!isaac_intensity_state[id]&&!isaac_intensity_state[other]);
    candidate_glDeleteTextures(2,pair);drains();++cases;
    /* Receipts are bounded per admitted shape and kind. */
    texture *v=fresh(44);
    while(isaac_intensity_skip_lines[1]<ISAAC_INTENSITY_LOG_CAP) {
        s=skip_receipts;isaac_intensity_state[44]=ISAAC_INTENSITY_EXCLUDED;
        image(v,w,h,gray);assert(skipped("excluded",s));cleanup(v);v=fresh(44);
    }
    {char cap[24];snprintf(cap,sizeof cap," n=%u/%u\n",ISAAC_INTENSITY_LOG_CAP,ISAAC_INTENSITY_LOG_CAP);
     assert(strstr(last_receipt,cap));}
    s=skip_receipts;uint32_t counted=word(ISAAC_INTENSITY_W_SKIP_BASE+1),gen=word(ISAAC_INTENSITY_W_GEN);
    for(unsigned k=0;k<8;++k) {
        isaac_intensity_state[44]=ISAAC_INTENSITY_EXCLUDED;image(v,w,h,gray);
        equal_rgba(v,gray,pixels);cleanup(v);v=fresh(44);
    }
    /* Past the cap: no tty line and no native line, but every skip counted. */
    assert(skip_receipts==s&&word(ISAAC_INTENSITY_W_SKIP_BASE+1)==counted+8&&word(ISAAC_INTENSITY_W_GEN)==gen);
    uint8_t *colored=malloc(pixels*4U);assert(colored);memcpy(colored,gray,pixels*4U);colored[1]^=1;
    while(isaac_intensity_reject_lines[1]<ISAAC_INTENSITY_LOG_CAP) {
        s=reject_receipts;isaac_intensity_state[44]=0;image(v,w,h,colored);
        assert(reject_receipts==s+1&&strstr(last_receipt,"phase=reject size=512x512 pixel=0 "));cleanup(v);v=fresh(44);
    }
    s=reject_receipts;counted=word(ISAAC_INTENSITY_W_REJECT_BASE+1);gen=word(ISAAC_INTENSITY_W_GEN);
    for(unsigned k=0;k<8;++k) {
        isaac_intensity_state[44]=0;image(v,w,h,colored);equal_rgba(v,colored,pixels);cleanup(v);v=fresh(44);
    }
    assert(reject_receipts==s&&word(ISAAC_INTENSITY_W_REJECT_BASE+1)==counted+8&&word(ISAAC_INTENSITY_W_GEN)==gen);
    /* Other shapes keep their own budget: one 2048x1024 skip still prints. */
    assert(isaac_intensity_skip_lines[2]==0);
    {
        uint8_t *big=calloc(2048U*1024U,4U);assert(big);
        s=skip_receipts;isaac_intensity_state[44]=ISAAC_INTENSITY_EXCLUDED;
        image(v,2048,1024,big);
        assert(skip_receipts==s+1&&strstr(last_receipt,"phase=skip size=2048x1024 slot=44 reason=excluded n=1/"));
        cleanup(v);free(big);
    }
    isaac_intensity_state[44]=0;
    free(colored);free(color);++cases;
}
int main(int argc, char **argv) {
    if(argc==4) {
        unsigned fw=(unsigned)strtoul(argv[2],NULL,10),fh=(unsigned)strtoul(argv[3],NULL,10);
        size_t bytes=(size_t)fw*fh*4U;uint8_t *data=malloc(bytes),*saved_data=malloc(bytes);
        assert(data&&saved_data);FILE *f=fopen(argv[1],"rb");assert(f);
        assert(fread(data,1,bytes,f)==bytes&&fgetc(f)==EOF);fclose(f);memcpy(saved_data,data,bytes);
        texture *page=fresh(1);image(page,(int)fw,(int)fh,data);
        size_t page_bytes=live_bytes;
        if(page->format==SCE_GXM_TEXTURE_FORMAT_U8_RRRR || page->format==SCE_GXM_TEXTURE_FORMAT_A8L8)
            exact(page,saved_data,bytes/4);
        else equal_rgba(page,saved_data,bytes/4);
        /* The real pinned native GL_LUMINANCE_ALPHA upload itself must produce
         * identical {L,A} bytes, not only our independent source comparison. */
        if(page->format==SCE_GXM_TEXTURE_FORMAT_A8L8) {
            texture *native_la=fresh(2);
            reference_glTexImage2D_FlatIMPL(native_la,0,GL_LUMINANCE_ALPHA,
                (int)fw,(int)fh,GL_RGBA,GL_UNSIGNED_BYTE,data);
            assert(native_la->format==page->format &&
                   !memcmp(native_la->data,page->data,page_bytes));
            exact(native_la,saved_data,bytes/4);cleanup(native_la);
        }
        assert(!memcmp(data,saved_data,bytes));
        printf("real-page reconstructed upload PASS: %ux%u GPU payload=%zu B storage=%s source=unchanged\n",
            fw,fh,page_bytes,page->format==SCE_GXM_TEXTURE_FORMAT_U8_RRRR?"U8_RRRR":
                page->format==SCE_GXM_TEXTURE_FORMAT_A8L8?"A8L8":"RGBA");
        assert(armed_receipts==1&&native_armed==1);
        cleanup(page);assert(!live_bytes&&!word(ISAAC_INTENSITY_W_LIVE_PAGES));free(saved_data);free(data);return 0;
    }
    assert(argc==1);
    const unsigned w=512,h=512,pixels=w*h;
    uint8_t *src=malloc(4096U*2048U*4U);assert(src);
    for(size_t i=0;i<4096U*2048U;++i) memset(src+4*i,(int)(i&255U),4);
    /* First upload failure also preserves an uninitialized object exactly;
     * it never publishes a descriptor pointing to the failed allocation. */
    texture *initial=fresh(25),initial_saved=*initial;
    fail_alloc=1;image(initial,w,h,src);
    assert(vgl_error==GL_OUT_OF_MEMORY&&!memcmp(initial,&initial_saved,sizeof initial_saved));
    assert(live_bytes==0 && isaac_intensity_state[25]==0);
    fail_alloc=0;vgl_error=0;++cases;
    /* Complete byte equivalence includes every alpha value, including 0/255. */
    const unsigned shapes[][2]={{512,512},{2048,1024},{2048,2048},{4096,2048}};
    for(unsigned k=0;k<4;++k) {
        texture *t=fresh(k+1);size_t n=(size_t)shapes[k][0]*shapes[k][1];
        size_t prior=live_bytes;
        image(t,shapes[k][0],shapes[k][1],src);
        assert(live_bytes-prior==n && last_alloc==n);exact(t,src,n);
        /* U8 channel replication commutes with bilinear interpolation: exact
         * rational weighted samples, NOT a model of GXM filtering precision. */
        for(unsigned a=0;a<256;++a) for(unsigned b=0;b<256;++b)
            for(unsigned c=0;c<4;++c)
                assert((unsigned)src[4*a+c]*17U+(unsigned)src[4*b+c]*239U ==
                       ((uint8_t *)t->data)[a]*17U+((uint8_t *)t->data)[b]*239U);
        cleanup(t);++cases;
    }
    /* First/last/interior and fully transparent colored texels are rejected;
     * fresh native reference uploads have exactly the same bytes/source. */
    const unsigned positions[]={0,12345,pixels-1};
    for(unsigned k=0;k<3;++k) for(unsigned channel=0;channel<4;++channel) {
        unsigned offset=positions[k]*4U+channel;src[offset]^=1;
        texture *a=fresh(10),*b=fresh(11);
        memset(isaac_intensity_state,0,sizeof isaac_intensity_state);
        reference_glTexImage2D_FlatIMPL(a,0,GL_RGBA,w,h,GL_RGBA,GL_UNSIGNED_BYTE,src);
        image(b,w,h,src);
        if(channel==3) {
            assert(b->format==SCE_GXM_TEXTURE_FORMAT_A8L8);exact(b,src,pixels);
            for(unsigned i=0;i<pixels;++i)
                assert(read_la88((uint8_t *)b->data+2*i)==read_rgba8888((uint8_t *)a->data+4*i));
        } else {
            equal_rgba(b,src,pixels);assert(!memcmp(a->data,b->data,pixels*4));
        }
        cleanup(a);cleanup(b);src[offset]^=1;++cases;
    }
    /* Fixture reset only; production resets in glGenTextures (name_suite). */
    memset(isaac_intensity_state,0,sizeof isaac_intensity_state);
    texture *t=fresh(12);image(t,w,h,src);void *old=t->data;
    texture saved=*t;unsigned nalloc=allocs;
    fail_alloc=1;image(t,w,h,src);
    assert(vgl_error==GL_OUT_OF_MEMORY&&allocs==nalloc+1&&!memcmp(t,&saved,sizeof saved));
    exact(t,src,pixels);fail_alloc=0;vgl_error=0;++cases;
    /* Every checked SDK init/setter failure rolls back its sole unpublished
     * allocation, leaving live native descriptor and bytes unchanged. */
    for(unsigned call=1;call<=8;++call) {
        descriptor_calls=0;fail_descriptor=call;size_t prior=live_bytes;
        image(t,w,h,src);assert(vgl_error==GL_INVALID_OPERATION);
        assert(live_bytes==prior&&!memcmp(t,&saved,sizeof saved));exact(t,src,pixels);++cases;
    }
    fail_descriptor=0;descriptor_calls=0;vgl_error=0;
    /* A second equal-channel definition is another U8 transaction, never a
     * pointless 32 MiB RGBA deopt. In-flight old pointer stays alive to purge. */
    t->last_frame=vgl_framecount;unsigned old_retires=retires;
    image(t,w,h,src);assert(t->data!=old&&retires==old_retires+1);exact(t,src,pixels);
    for(unsigned i=0;i<pixels;++i) assert(((uint8_t *)old)[i]==(uint8_t)i);
    drains();++cases;
    /* Filter/wrap setters keep exact U8 pixels. Unknown/mipmap state deopts. */
    candidate_glTexParameteri(t,GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    candidate_glTexParameteri(t,GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    assert(t->format==SCE_GXM_TEXTURE_FORMAT_U8_RRRR);exact(t,src,pixels);
    saved=*t;fail_alloc=1;
    candidate_glTexParameteri(t,GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
    assert(vgl_error==GL_OUT_OF_MEMORY&&!memcmp(t,&saved,sizeof saved));fail_alloc=0;vgl_error=0;
    candidate_glTexParameteri(t,GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
    equal_rgba(t,src,pixels);assert(t->use_mips);cleanup(t);++cases;
    /* Colored subimage: native writer must see RGBA, not write only red into U8. */
    t=fresh(13);image(t,w,h,src);saved=*t;
    uint8_t patch[4]={3,17,29,31};fail_alloc=1;
    candidate_glTexSubImage2D(t,GL_TEXTURE_2D,0,4,7,1,1,GL_RGBA,GL_UNSIGNED_BYTE,patch);
    assert(vgl_error==GL_OUT_OF_MEMORY&&!memcmp(t,&saved,sizeof saved));exact(t,src,pixels);
    fail_alloc=0;vgl_error=0;
    candidate_glTexSubImage2D(t,GL_TEXTURE_2D,0,4,7,1,1,GL_RGBA,GL_UNSIGNED_BYTE,patch);
    assert(!memcmp((uint8_t *)t->data+4U*(7*w+4),patch,4));
    assert(t->format==SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);cleanup(t);++cases;
    /* Later mip definition is explicitly refused: stock mip allocation is
     * not transactional. No allocation, native mip call or owner mutation. */
    t=fresh(14);image(t,w,h,src);saved=*t;unsigned m=mip_calls;
    candidate_glTexImage2D_FlatIMPL(t,1,GL_RGBA,w/2,h/2,GL_RGBA,GL_UNSIGNED_BYTE,src);
    assert(mip_calls==m&&vgl_error==GL_INVALID_OPERATION&&!memcmp(t,&saved,sizeof saved));
    exact(t,src,pixels);cleanup(t);vgl_error=0;++cases;
    /* A full colored replacement must not retire U8 on allocation/SDK
     * failure, and succeeds without first allocating a full-RGBA shadow. */
    t=fresh(24);image(t,w,h,src);saved=*t;src[0]=7;fail_alloc=1;
    image(t,w,h,src);assert(vgl_error==GL_OUT_OF_MEMORY&&!memcmp(t,&saved,sizeof saved));
    fail_alloc=0;
    for(unsigned d=1;d<=8;++d) {
        descriptor_calls=0;fail_descriptor=d;size_t prior=live_bytes;
        image(t,w,h,src);assert(vgl_error==GL_INVALID_OPERATION);
        assert(live_bytes==prior&&!memcmp(t,&saved,sizeof saved));++cases;
    }
    fail_descriptor=0;vgl_error=0;
    image(t,w,h,src);equal_rgba(t,src,pixels);src[0]=0;cleanup(t);++cases;
    /* FBO attachment cannot cache a stale U8 data pointer. Failure is before
     * ANY framebuffer/refcounter/surface change; success attaches expanded data. */
    t=fresh(15);image(t,w,h,src);saved=*t;
    framebuffer fb={0},fb_saved=fb;unsigned cc=color_calls;fail_alloc=1;
    candidate_attach(&fb,GL_COLOR_ATTACHMENT0,t,15);
    assert(vgl_error==GL_OUT_OF_MEMORY&&!memcmp(&fb,&fb_saved,sizeof fb)&&!memcmp(t,&saved,sizeof saved));
    assert(color_calls==cc);fail_alloc=0;vgl_error=0;
    candidate_attach(&fb,GL_COLOR_ATTACHMENT0,t,15);
    assert(fb.data==t->data&&fb.data==t->gxm_tex.data&&fb.tex==t&&t->ref_counter==1&&color_calls==cc+1);
    equal_rgba(t,src,pixels);cleanup(t);++cases;
    /* Deopt descriptor failures keep the old object; no retired pointer. */
    for(unsigned d=1;d<=2;++d) {
        t=fresh(16+d);image(t,w,h,src);saved=*t;size_t prior=live_bytes;
        descriptor_calls=0;fail_descriptor=d;
        assert(isaacIntensityPrepare(t,1)==GL_INVALID_OPERATION);
        assert(live_bytes==prior&&!memcmp(t,&saved,sizeof saved));exact(t,src,pixels);
        fail_descriptor=0;cleanup(t);++cases;
    }
    /* Attached-before-upload objects never transform, ordinary color targets
     * remain on their original owner path for the object's lifetime. */
    t=fresh(21);assert(isaacIntensityPrepare(t,1)==GL_NO_ERROR);image(t,w,h,src);
    equal_rgba(t,src,pixels);cleanup(t);++cases;
    /* Delete hook drops compact marker; reused native slot has no old owner. */
    t=fresh(22);image(t,w,h,src);cleanup(t);t=fresh(22);image(t,w,h,src);exact(t,src,pixels);cleanup(t);++cases;
    /* Admission guards (no allocation or source modification in helper). */
    t=fresh(23);unsigned calls=allocs;
    assert(!isaac_intensity_try_upload(t,0,GL_RGBA,511,512,GL_RGBA,GL_UNSIGNED_BYTE,src,0));
    assert(!isaac_intensity_try_upload(t,1,GL_RGBA,w,h,GL_RGBA,GL_UNSIGNED_BYTE,src,0));
    assert(!isaac_intensity_try_upload(t,0,GL_RGBA,w,h,GL_RGBA,GL_UNSIGNED_BYTE,src,w+1));
    assert(!isaac_intensity_try_upload(t,0,GL_RGBA,w,h,GL_RGBA,GL_UNSIGNED_BYTE,NULL,0));
    assert(allocs==calls);
    for(size_t i=0;i<4096U*2048U;++i) for(unsigned c=0;c<4;++c) assert(src[i*4+c]==(uint8_t)i);
    gray_suite();
    /* Name-lifetime and receipt-bound cases on an all-equal 512x512 page. */
    name_suite(src);
    assert(live_bytes==0);free(src);durable();
    assert(!word(ISAAC_INTENSITY_W_LIVE_PAGES)&&!word(ISAAC_INTENSITY_W_LIVE_BYTES));
    /* Hundreds of admission checks above, exactly one arming receipt. The
     * runtime reader writes one native line per read that moved word 0 (plus
     * the armed line), each with the cumulative words: several tty lines
     * between two reads share one native line, none is lost from the counts. */
    assert(armed_receipts==1&&native_armed==1&&native_lines==1+durable_changes);
    assert(durable_changes<=tty_lines&&durable_multi>0&&word(ISAAC_INTENSITY_W_GEN)==tty_lines);
    /* Every event kind reached the reader at least once (else the per-kind
     * equalities in durable() are vacuous for it). */
    assert(tty_upload&&tty_failed&&tty_deopt&&tty_deopt_failed&&tty_redefine&&skip_receipts&&reject_receipts);
    assert(word(ISAAC_INTENSITY_W_U8)&&word(ISAAC_INTENSITY_W_A8L8));
    printf("native-body storage oracle PASS: %u cases + all 65536 (L,A) pairs; U8/A8L8 actual callbacks, colored/transparent boundaries, source identity, RGBA mutation/mip/FBO boundaries, OOM + 38 SDK failure positions, format transitions/padding/deferred old data; armed receipt=%u; glGenTextures name-lifetime reset (ordinary/NULL-RT/parameter/attached/active-FBO-delete slot reuse) + bounded skip/reject receipts (cap %u per shape; skip=%u reject=%u); peak fixture allocations=%zu B (not Vita RAM)\n",cases,armed_receipts,ISAAC_INTENSITY_LOG_CAP,skip_receipts,reject_receipts,peak_bytes);
    printf("runtime reader (gl_vita_backend.c) PASS: native lines=%u = 1 armed + %u reads that moved word 0 (%u of them by >1), covering %u tty lines (upload=%u u8=%u a8l8=%u upload-failed=%u deopt=%u deopt-failed=%u redefine-rejected=%u); words checked after every step against tty kinds and live slots\n",
        native_lines,durable_changes,durable_multi,tty_lines,tty_upload,(unsigned)word(ISAAC_INTENSITY_W_U8),
        (unsigned)word(ISAAC_INTENSITY_W_A8L8),tty_failed,tty_deopt,tty_deopt_failed,tty_redefine);
    return 0;
}
