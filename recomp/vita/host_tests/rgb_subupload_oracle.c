/* Actual pinned/patched _glTexSubImage2D bodies and stock callbacks are generated
 * into the include on EVERY run. SDK/allocation boundaries below are test doubles. */
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rgb-subupload-enums.h"
typedef uint32_t GLenum, SceGxmTextureFormat;
typedef int32_t GLint, GLsizei;
typedef uint8_t GLboolean;
typedef void GLvoid;
typedef struct { uint32_t format, width, height, kind; void *data; } SceGxmTexture;
typedef struct texture {
    SceGxmTexture gxm_tex;
    uint32_t mip_count, last_frame, status;
    void *data;
} texture;
#define TEX_VALID 1U /* opaque status in the SDK double */
#define OBJ_NOT_USED UINT32_MAX
#define FRAME_PURGE_FREQ 4U
#define VGL_ALIGN(v,a) (((v)+(a)-1U)&~((a)-1U))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define SET_GL_ERROR(e) { last_error=(e); return; }
#define SET_GL_ERROR_WITH_VALUE(e,v) { (void)(v); last_error=(e); return; }

enum { CAPACITY = 4*1024*1024, GUARD = 32 };
static _Alignas(16) uint8_t old_mem[CAPACITY+2*GUARD], new_mem[CAPACITY+2*GUARD];
static _Alignas(16) uint8_t input[CAPACITY+32], images[2][CAPACITY];
static uint32_t unpack_row_len, vgl_framecount, last_error;
static uint32_t allocs, retires, binds, copies, copy_bytes, cubes, fail_alloc;
static jmp_buf allocation_fault;
static uint32_t vglGetTexFormat(const SceGxmTexture *t) { return t->format; }
static void vglGetTexSizes(const SceGxmTexture *t, uint32_t *w, uint32_t *h) { *w=t->width; *h=t->height; }
static uint32_t sceGxmTextureGetType(const SceGxmTexture *t) { return t->kind; }
static int sceGxmTextureSetData(SceGxmTexture *t, void *p) { ++binds; t->data=p; return 0; }
static uint32_t nearest_po2(uint32_t n) { uint32_t r=1; while(r<n) r*=2; return r; }
static uint8_t tex_format_to_bytespp(uint32_t f) {
    if (f==SCE_GXM_TEXTURE_FORMAT_U8U8U8_BGR || f==SCE_GXM_TEXTURE_FORMAT_U8U8U8_RGB) return 3;
    if (f==SCE_GXM_TEXTURE_FORMAT_U8U8_GR || f==SCE_GXM_TEXTURE_FORMAT_A8L8 ||
        f==SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB || f==SCE_GXM_TEXTURE_FORMAT_U4U4U4U4_RGBA ||
        f==SCE_GXM_TEXTURE_FORMAT_U5U5U5U1_RGBA) return 2;
    if (f==SCE_GXM_TEXTURE_FORMAT_L8 || f==SCE_GXM_TEXTURE_FORMAT_U8_RRRR ||
        f==SCE_GXM_TEXTURE_FORMAT_U8_R111 || f==SCE_GXM_TEXTURE_FORMAT_P8_ABGR) return 1;
    return 4;
}
static void *gpu_alloc_mapped_for_gpu(uint32_t size) { assert(size<=CAPACITY); ++allocs; return fail_alloc?NULL:new_mem+GUARD; }
static void vgl_fast_memcpy(void *d, const void *s, size_t n) {
    ++copies; copy_bytes+=(uint32_t)n;
    if(!d) longjmp(allocation_fault,1); /* models the existing NULL copy boundary */
    memcpy(d,s,n);
}
static void gpu_free_texture_data(texture *t) { assert(t->data==old_mem+GUARD); ++retires; }
static void _glTexImage2D_CubeIMPL(texture *t, GLint l, GLenum f, GLsizei w, GLsizei h,
    GLenum f2, GLenum type, const void *pixels, GLenum face) {
    (void)t;(void)l;(void)f;(void)w;(void)h;(void)f2;(void)type;(void)pixels;(void)face; ++cubes;
}
static void vgl_log(const char *f, ...) { (void)f; }
#ifdef HAVE_ISAAC_P8_SAFE_UPLOAD
static GLenum isaac_p8_promote_rgba_subimage(texture *t, GLenum target, GLint level,
    GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum type, const void *p, uint32_t row) {
    (void)t;(void)target;(void)level;(void)x;(void)y;(void)w;(void)h;(void)f;(void)type;(void)p;(void)row;
    return GL_INVALID_OPERATION; /* unchanged dependency, failure propagation checked */
}
#endif
#include "rgb-subupload-generated.h"

typedef void (*Upload)(texture *, GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
typedef struct Case {
    uint32_t ow,oh,mips,used,row,format,type,native_format,native_type,status;
    int32_t level,x,y,w,h;
    uint32_t alignment,alias,oom;
} Case;
typedef struct Result { uint32_t error,oom,allocs,retires,binds,copies,bytes,last_frame,size,stats[6]; } Result;

static uint32_t storage_bytes(const Case *c) {
    uint32_t bpp=tex_format_to_bytespp(c->native_format), w=c->ow,h=c->oh,n=0;
    if(c->mips<=1) return VGL_ALIGN(w,8)*h*bpp;
    w=nearest_po2(w); h=nearest_po2(h);
    for(uint32_t i=0;i<c->mips;++i) { n+=MAX(w,8)*h*bpp; w/=2; h/=2; }
    return n;
}
static Result execute(const Case *c, Upload call, uint32_t result_index) {
    Result r={0};
    uint32_t size=storage_bytes(c);
    assert(size<CAPACITY);
    memset(old_mem,0xD7,size+2*GUARD); memset(new_mem,0xA5,size+2*GUARD);
    for(uint32_t i=0;i<size;++i) old_mem[GUARD+i]=(uint8_t)(i*19U+103U);
    uint32_t input_size=(c->row?c->row:c->ow)*c->oh*4U+32U;
    assert(input_size<=sizeof input);
    for(uint32_t i=0;i<input_size;++i) input[i]=(uint8_t)(i*37U+51U);
    texture t={{c->native_format,c->ow,c->oh,c->native_type,old_mem+GUARD},
               c->mips,c->used?10U:OBJ_NOT_USED,c->status,old_mem+GUARD};
    vgl_framecount=11U; unpack_row_len=c->row; fail_alloc=c->oom;
    last_error=allocs=retires=binds=copies=copy_bytes=cubes=0;
    isaac_rgb_subupload_entries=isaac_rgb_subupload_rects=isaac_rgb_subupload_elisions=0;
    isaac_rgb_subupload_elided_bytes=0;
    if(setjmp(allocation_fault)==0)
        call(&t,GL_TEXTURE_2D,c->level,c->x,c->y,c->w,c->h,c->format,c->type,
             c->alias?old_mem+GUARD:input+c->alignment);
    else r.oom=1U;
    r.error=last_error; r.allocs=allocs; r.retires=retires; r.binds=binds;
    r.copies=copies; r.bytes=copy_bytes; r.last_frame=t.last_frame; r.size=size;
    vglIsaacRgbSubuploadSnapshot(r.stats);
    memcpy(images[result_index],t.data,size);
    for(uint32_t i=0;i<GUARD;++i) {
        assert(old_mem[i]==0xD7 && old_mem[GUARD+size+i]==0xD7);
        assert(new_mem[i]==0xA5 && new_mem[GUARD+size+i]==0xA5);
    }
    if(t.data!=old_mem+GUARD)
        for(uint32_t i=0;i<size;++i) assert(old_mem[GUARD+i]==(uint8_t)(i*19U+103U));
    assert(t.gxm_tex.data==t.data); /* normal descriptor update still occurred */
    return r;
}
static unsigned cases, elisions, conversions;
static void compare(Case c, int expect_elision, int expect_conversion) {
    Result a=execute(&c,reference_subupload,0), b=execute(&c,candidate_subupload,1);
    assert(a.error==b.error && a.oom==b.oom);
    assert(a.allocs==b.allocs && a.retires==b.retires && a.binds==b.binds);
    assert(a.last_frame==b.last_frame && a.size==b.size);
    assert(memcmp(images[0],images[1],a.size)==0);
    assert(b.stats[0]==1U && b.stats[2]==(uint32_t)expect_elision);
    assert(b.stats[1]==(uint32_t)expect_conversion);
    assert(b.stats[3]==(expect_elision?a.size:0U) && b.stats[4]==0U);
    assert(b.stats[5]==ISAAC_RGB_SUBUPLOAD_NEON);
    assert(a.bytes==b.bytes+(expect_elision?a.size:0U));
    assert(a.copies==b.copies+(expect_elision?1U:0U));
    ++cases; elisions+=b.stats[2]; conversions+=b.stats[1];
}
int main(void) {
    Case base={32,16,1,1,0,GL_RGB,GL_UNSIGNED_BYTE,
        SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR,SCE_GXM_TEXTURE_LINEAR,TEX_VALID,0,0,0,32,16,0,0,0};
    for(uint32_t align=0;align<16;++align) {
        Case c=base; c.alignment=align; compare(c,1,1);
        c.used=0; compare(c,0,1);
        c=base; c.alignment=align; c.x=3;c.y=2;c.w=17;c.h=7;c.row=23;compare(c,0,1);
        c=base; c.alignment=align;c.row=40;compare(c,1,1);
    }
    for(int width=1;width<=65;++width) {
        Case c=base;c.ow=(uint32_t)width;c.w=width;c.oh=3;c.h=3;
        compare(c,width%8==0,1); /* entire padding must survive non-aligned widths */
    }
    Case c=base;c.ow=960;c.oh=544;c.w=960;c.h=544;compare(c,1,1);
    c=base;c.mips=0;compare(c,1,1);
    c=base;c.mips=3;compare(c,0,1); /* retain every other mip during level-zero upload */
    /* Stock COW+level>0 reads an uninitialized mip_stride. Test defined mip
     * cases without COW; the new conversion path deliberately requires level 0. */
    c.used=0;c.level=1;c.w=16;c.h=8;compare(c,0,0);
    c.level=2;c.w=8;c.h=4;compare(c,0,0);
    c=base;c.row=17;compare(c,0,0);
    c=base;c.alias=1;c.used=0;compare(c,0,0);
    c.used=1;compare(c,1,1); /* source is old GPU buffer; new buffer is disjoint */
    c=base;c.format=GL_BGR;compare(c,0,0);
    c=base;c.format=GL_RGBA;compare(c,0,0);
    c=base;c.format=3;compare(c,0,1); /* legacy normalization happens after COW */
    c=base;c.native_format=SCE_GXM_TEXTURE_FORMAT_U8U8U8_BGR;compare(c,0,0);
    c=base;c.native_type=SCE_GXM_TEXTURE_SWIZZLED;compare(c,0,1);
    c=base;c.type=GL_FLOAT;compare(c,0,0); /* invalid enum AFTER COW keeps old contents */
    c=base;c.format=0xFFFFFFFFU;compare(c,0,0);
    c=base;c.w=0;compare(c,0,0);
    c=base;c.h=0;compare(c,0,0);
    c=base;c.level=-1;compare(c,0,0);
#ifndef SKIP_ERROR_HANDLING
    /* NO_DEBUG deliberately omits these checks in stock vitaGL. The negative
     * rectangles would be out-of-bounds in the reference, not equality cases. */
    c=base;c.x=-1;compare(c,0,0);
    c=base;c.w=33;compare(c,0,0);
    c=base;c.status=0;compare(c,0,0);
#endif
    c=base;c.oom=1;compare(c,0,0);
    printf("RGB subupload native-body oracle: PASS cases=%u elisions=%u conversions=%u; "
           "all pixels/padding, old GPU data, allocation/retirement/binding/error/NULL-copy boundaries identical\n",
           cases,elisions,conversions);
    return 0;
}
