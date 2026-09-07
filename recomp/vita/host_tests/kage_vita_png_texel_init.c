/* Focused real-memory-handler/scratch and freshly generated row-loop check.
 * The driver inserts only the unchanged ImagePng clear/row-pointer/read loop.
 * Valid IDAT data runs through the production native PNG decoder first. */
#include "guest.h"
#include "host_vita_memory.h"
#include "host_vita_heap.h"
#include "host_vita_native_png.h"
#include "host_vita_png_texel_init.h"
#include "host_vita_texel_scratch.h"
#include "kage_vita_texture_memory.h"
#include <psp2/kernel/sysmem.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

enum { BUFFER=0x70000000U, OBJECT=0x22000000U, PNG=0x22100000U,
       INFO=0x22200000U, TABLE=0x22300000U, STACK=0x23000000U,
       FRAME=STACK+0x8000U, REGION=0x10000U };
static uint8_t *pixels, *object, *pngmem, *infomem, *stackmem, *rowtable;
static uint8_t *decoded;
static uint32_t rowbytes, rowstride, height;
static unsigned candidate, imports, rows, table_frees, faults, published;
static unsigned mem_live, lease_calls, lease_live, deny_lease, fail_release;
static unsigned checking_clear, expected_elision, admitted_clears;
static unsigned abort_row=UINT32_MAX, comparisons;
static jmp_buf abort_env;
int original_memory_import_counted(CPU *,const char *,unsigned *);

static void fail(int line,const char *expr) {
    fprintf(stderr,"PNG texel init line %d: %s\n",line,expr); exit(1);
}
#define CHECK(x) do { if(!(x)) fail(__LINE__,#x); } while(0)
int guest_stack_violation(CPU *c,uint32_t pc,uint32_t kind,uint32_t a,uint32_t n) {
    (void)pc;(void)kind;(void)a;(void)n; c->fault="stack"; ++faults; return 0;
}
int guest_stack_owner_violation(CPU *c,uint32_t pc) {
    (void)pc; c->fault="stack-owner"; ++faults; return 0;
}
void guest_fault(CPU *c,uint32_t a,const char *why) { (void)a; c->fault=why; ++faults; }
uint32_t isaac_vita_guest_heap_lease_exact_range(const void *base,const void *range,size_t n) {
    unsigned i;
    const uint32_t bases[3]={OBJECT,PNG,INFO}, sizes[3]={0x8cU,0x20cU,0xb8U};
    ++lease_calls; errno=ERANGE;
    for(i=0;i<3;++i) if((uintptr_t)base==bases[i] && range==base && n==sizes[i]) {
        if(deny_lease==i+1U) return 0;
        CHECK(!(lease_live&(1U<<i))); lease_live|=1U<<i; return i+1U;
    }
    return 0;
}
int isaac_vita_guest_heap_lease_release(uint32_t token) {
    CHECK(token>=1U && token<=3U && (lease_live&(1U<<(token-1U))));
    lease_live&=~(1U<<(token-1U)); errno=ERANGE; return token!=fail_release;
}
SceUID sceKernelAllocMemBlock(const char *name,SceKernelMemBlockType type,SceSize n,
                            SceKernelAllocMemBlockOpt *opt) {
    CHECK(name && !opt && !mem_live && type==SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
    CHECK(n==KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES); mem_live=1; return 1;
}
int sceKernelGetMemBlockBase(SceUID uid,void **out) { CHECK(uid==1 && mem_live); *out=pixels; return 0; }
int sceKernelFreeMemBlock(SceUID uid) { CHECK(uid==1 && mem_live); mem_live=0; return 0; }
void isaac_vita_log(const char *format,...) { (void)format; }

static void *map_at(uint32_t base,size_t size) {
#ifdef _WIN32
    void *p=VirtualAlloc((void *)(uintptr_t)base,size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
#else
    void *p=mmap((void *)(uintptr_t)base,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
#endif
    CHECK((uintptr_t)p==base); return p;
}
static void acquire(unsigned n,unsigned owner) {
    isaac_vita_texel_scratch_decision d;
    CHECK(isaac_vita_texel_scratch_malloc(owner,n,STACK,STACK+REGION,&d)==1);
    CHECK(d.pointer==pixels);
}
static void release_buffer(void) {
    isaac_vita_texel_scratch_decision d;
    CHECK(isaac_vita_texel_scratch_free(pixels,&d)==1);
}
static void setup(CPU *c,unsigned w,unsigned h,unsigned pw,unsigned ph,unsigned gamma) {
    memset(c,0,sizeof *c); memset(object,0,REGION); memset(pngmem,0,REGION);
    memset(infomem,0,REGION); memset(stackmem,0xa7,REGION); memset(rowtable,0xc8,REGION);
    c->stack_owner=c; c->stack_floor=STACK; c->stack_ceiling=STACK+REGION;
    c->stack_low_water=FRAME-0x458U; c->esp=FRAME-0x448U; c->ebp=FRAME;
    c->eax=BUFFER; c->edi=pw*ph*4U; c->esi=OBJECT; c->ebx=PNG;
    c->ecx=0x987e8170U; c->edx=0x1234U; c->fl=(guest_flags){0};
    st32(FRAME+4,gamma?0x5a0d62U:0x5a0c7eU);
    st32(FRAME-0x430U,OBJECT); st32(FRAME-0x428U,OBJECT+0x80U);
    st32(FRAME-0x424U,BUFFER); st32(FRAME-0x418U,PNG); st32(FRAME-0x414U,INFO);
    st32(FRAME-0x410U,4U); st32(OBJECT,GUEST_IMAGE_BASE+0x766028U);
    st16(OBJECT+0x80U,(uint16_t)w); st16(OBJECT+0x82U,(uint16_t)h);
    st16(OBJECT+0x84U,(uint16_t)pw); st16(OBJECT+0x86U,(uint16_t)ph); st32(OBJECT+0x88U,4U);
    st32(PNG+ISAAC_NP_PNG_FLAGS,ISAAC_NP_FLAG_ROW_INIT);
    st32(PNG+ISAAC_NP_PNG_MODE,ISAAC_NP_MODE_HAVE_IDAT);
    st32(PNG+ISAAC_NP_PNG_WIDTH,w); st32(PNG+ISAAC_NP_PNG_HEIGHT,h);
    st32(PNG+ISAAC_NP_PNG_ROWBYTES,w*4U); st32(PNG+ISAAC_NP_PNG_IROWBYTES,w*4U+1U);
    st32(PNG+ISAAC_NP_PNG_IWIDTH,w); st8(PNG+ISAAC_NP_PNG_BIT_DEPTH,8U);
    st8(PNG+ISAAC_NP_PNG_COLOR_TYPE,6U); st8(PNG+ISAAC_NP_PNG_CHANNELS,4U);
    st8(PNG+ISAAC_NP_PNG_PIXEL_DEPTH,32U);
    st32(PNG+ISAAC_NP_PNG_READ_DATA_FN,GUEST_IMAGE_BASE+ISAAC_NP_DEFAULT_READ_FN_RVA);
    st32(PNG+ISAAC_NP_PNG_TRANSFORMATIONS,gamma?ISAAC_NP_TRANSFORM_GAMMA:0U);
    st32(INFO,w); st32(INFO+4U,h); st32(INFO+0xcU,w*4U); st8(INFO+0x18U,8U); st8(INFO+0x19U,6U);
    rowbytes=w*4U; rowstride=pw*4U; height=h;
    imports=rows=table_frees=faults=published=lease_calls=0;
    CHECK(!lease_live); deny_lease=fail_release=0;
}
static void at_memset(CPU *c) {
    gpush(c,c->edi); gpush(c,0U); gpush(c,c->eax); gpush(c,ISAAC_VITA_PNG_TEXEL_INIT_RETURN_RVA);
}
void sub_005ec152(CPU *c) {
    unsigned n=ld32(c->esp+12U),i;
    int ok=candidate?isaac_vita_memory_import_counted(c,ISAAC_VITA_MEMORY_MEMSET_NAME,&imports):
        original_memory_import_counted(c,ISAAC_VITA_MEMORY_MEMSET_NAME,&imports);
    CHECK(ok);
    if(checking_clear) {
        CHECK(!c->fault);
        for(i=0;i<n;++i)CHECK(pixels[i]==(expected_elision?0xd3U:0U));
        admitted_clears+=expected_elision; checking_clear=0U;
    }
}
void sub_005eb09c(CPU *c) { CHECK(ld32(c->esp+4U)==height*4U); c->eax=TABLE; (void)gpop(c); }
void sub_005eace5(CPU *c) { CHECK(ld32(c->esp+4U)==TABLE); ++table_frees; (void)gpop(c); }
void sub_005b1500(CPU *c) {
    CHECK(c->ecx==PNG && c->edx==BUFFER+rows*rowstride);
    if(rows==abort_row) longjmp(abort_env,1);
    memcpy((void *)(uintptr_t)c->edx,decoded+(size_t)rows*(rowbytes+1U)+1U,rowbytes);
    ++rows; st32(PNG+ISAAC_NP_PNG_ROW_NUMBER,rows); (void)gpop(c);
}
/* FRESH_ROW_LOOP */

static void compare_rows(unsigned w,unsigned h,unsigned pw,unsigned ph,unsigned gamma) {
    CPU c,answer; unsigned n=pw*ph*4U;
    uint8_t *expected=malloc(n+32U), *saved_stack=malloc(REGION), *saved_png=malloc(REGION);
    CHECK(expected && saved_stack && saved_png);
    setup(&c,w,h,pw,ph,gamma); acquire(n,KAGE_VITA_TEXEL_PNG_LOADER_RETURN);
    memset(pixels,0xd3,n+32U); candidate=0;checking_clear=1U;expected_elision=0U;
    row_loop(&c); answer=c;
    CHECK(rows==h && table_frees==1U && imports==1U);
    memcpy(expected,pixels,n+32U); memcpy(saved_stack,stackmem,REGION); memcpy(saved_png,pngmem,REGION);
    release_buffer();
    setup(&c,w,h,pw,ph,gamma); acquire(n,KAGE_VITA_TEXEL_PNG_LOADER_RETURN);
    memset(pixels,0xd3,n+32U); candidate=1; errno=EDOM;
    checking_clear=1U;expected_elision=(w==pw && h==ph);row_loop(&c);
    CHECK(errno==EDOM && !faults && !lease_live && rows==h && table_frees==1U && imports==1U);
    CHECK(!memcmp(&c,&answer,sizeof c)); CHECK(!memcmp(expected,pixels,n+32U));
    CHECK(!memcmp(saved_stack,stackmem,REGION) && !memcmp(saved_png,pngmem,REGION));
    ++published; CHECK(published==1U); release_buffer();
    free(expected); free(saved_stack); free(saved_png); ++comparisons;
}

static void reject_unchanged(CPU *c) {
    CPU old=*c; uint8_t saved[512]; memcpy(saved,pixels,sizeof saved);
    errno=EDOM; CHECK(isaac_vita_png_texel_init_try(c,BUFFER,0,256U)==0);
    CHECK(errno==EDOM && !memcmp(c,&old,sizeof old) && !lease_live);
    CHECK(!memcmp(saved,pixels,sizeof saved));
}
static void guards(void) {
    CPU c; unsigned i;
    for(i=0;i<37U;++i) {
        setup(&c,8,8,8,8,0); acquire(256U,KAGE_VITA_TEXEL_PNG_LOADER_RETURN); at_memset(&c);
        memset(pixels,0xd3,512U);
        switch(i) {
        case 0:st32(c.esp,0x5a0ef0U);break;
        case 1:st32(FRAME+4U,0x1111U);break;
        case 2:st32(FRAME-0x430U,0U);break;
        case 3:st32(FRAME-0x418U,0U);break;
        case 4:st32(FRAME-0x424U,BUFFER+4U);break;
        case 5:st32(FRAME-0x410U,3U);break;
        case 6:c.edi++;break; case 7:c.eax++;break;
        case 8:st32(OBJECT,0U);break; case 9:st16(OBJECT+0x80U,0U);break;
        case 10:st16(OBJECT+0x84U,16U);break; case 11:st16(OBJECT+0x86U,16U);break;
        case 12:st32(OBJECT+0x88U,3U);break;
        case 13:st32(PNG+ISAAC_NP_PNG_FLAGS,0U);break;
        case 14:st32(PNG+ISAAC_NP_PNG_MODE,0U);break;
        case 15:st8(PNG+ISAAC_NP_PNG_INTERLACED,1U);break;
        case 16:st8(PNG+ISAAC_NP_PNG_PASS,1U);break;
        case 17:st32(PNG+ISAAC_NP_PNG_ROW_NUMBER,1U);break;
        case 18:st8(PNG+ISAAC_NP_PNG_BIT_DEPTH,16U);break;
        case 19:st8(PNG+ISAAC_NP_PNG_COLOR_TYPE,3U);break;
        case 20:st8(PNG+ISAAC_NP_PNG_CHANNELS,3U);break;
        case 21:st8(PNG+ISAAC_NP_PNG_PIXEL_DEPTH,24U);break;
        case 22:st32(PNG+ISAAC_NP_PNG_WIDTH,9U);break;
        case 23:st32(PNG+ISAAC_NP_PNG_IROWBYTES,33U+4U);break;
        case 24:st32(PNG+ISAAC_NP_PNG_TRANSFORMATIONS,ISAAC_NP_TRANSFORM_INTERLACE);break;
        case 25:st32(PNG+ISAAC_NP_PNG_READ_DATA_FN,0U);break;
        case 26:st32(PNG+ISAAC_NP_PNG_READ_ROW_FN,1U);break;
        case 27:st32(PNG+0x40U,1U);break; case 28:st32(PNG+0x44U,1U);break;
        case 29:st32(PNG+0x48U,1U);break; case 30:st32(INFO+0xcU,31U);break;
        case 31:st8(INFO+0x18U,4U);break; case 32:st8(INFO+0x19U,0U);break;
        case 33:deny_lease=1U;break; case 34:deny_lease=2U;break; case 35:deny_lease=3U;break;
        case 36:c.esp-=4U;st32(c.esp,ISAAC_VITA_PNG_TEXEL_INIT_RETURN_RVA);break;
        }
        reject_unchanged(&c); release_buffer();
    }
    setup(&c,8,8,8,8,0); acquire(256U,KAGE_VITA_TEXEL_PNG_LOADER_RETURN); at_memset(&c);
    CHECK(isaac_vita_texel_scratch_oracle_hold_lock()); reject_unchanged(&c);
    isaac_vita_texel_scratch_oracle_drop_lock(); release_buffer(); reject_unchanged(&c);
    acquire(256U,KAGE_VITA_TEXEL_LOADER_RETURN_4); reject_unchanged(&c); release_buffer();
    acquire(512U,KAGE_VITA_TEXEL_PNG_LOADER_RETURN); reject_unchanged(&c); release_buffer();
    acquire(256U,KAGE_VITA_TEXEL_PNG_LOADER_RETURN);
    { CPU old=c; CHECK(isaac_vita_png_texel_init_try(&c,BUFFER,0,256U)==1);
      CHECK(!memcmp(&old,&c,sizeof c) && !lease_live); }
    fail_release=2U; candidate=1;
    { CPU old=c; unsigned old_imports=imports;
      memcpy(pixels,"unchanged",10U); errno=EDOM; sub_005ec152(&c);
      CHECK(c.fault && faults==1U && c.esp==old.esp && c.eax==old.eax);
      CHECK(!memcmp(pixels,"unchanged",10U) && !lease_live && imports==old_imports+1U && errno==EDOM); }
    fail_release=0; release_buffer();
    /* Unrelated/zero/nonzero-fill calls must not enter metadata leasing. */
    for(i=0;i<3U;++i) {
        setup(&c,8,8,8,8,0); at_memset(&c); memset(pixels,0xd3,256U);
        if(i==0U) st32(c.esp,0x1234U);
        if(i==1U) st32(c.esp+12U,0U);
        if(i==2U) st32(c.esp+8U,0x55U);
        sub_005ec152(&c); CHECK(!lease_calls && imports==1U && !faults);
        CHECK(pixels[0]==(i==0U?0U:i==1U?0xd3U:0x55U));
    }
    /* Entire destination validation precedes eligibility and keeps its fault. */
    setup(&c,8,8,8,8,0); at_memset(&c); st32(c.esp+4U,0U);
    { unsigned esp=c.esp; sub_005ec152(&c); CHECK(c.fault && c.esp==esp && !lease_calls && imports==1U); }
}
typedef struct { const uint8_t *data; uint32_t size,pos; } Reader;
static uint32_t read_bytes(void *ctx,uint8_t *out,uint32_t n) {
    Reader *r=ctx; uint32_t left=r->size-r->pos; if(n>left)n=left;
    memcpy(out,r->data+r->pos,n); r->pos+=n; return n;
}
static void valid_cases(const char *path) {
    FILE *f=fopen(path,"rb"); unsigned records, rec; CHECK(f);
    CHECK(fread(&records,4,1,f)==1U);
    for(rec=0;rec<records;++rec) {
        uint32_t v[6], y, x; uint8_t *idat,*expected,*last,*staging,*state,table[256];
        isaac_np_params p; isaac_np_work work; isaac_np_result result; Reader r;
        CHECK(fread(v,4,6,f)==6U); idat=malloc(v[5]); expected=malloc((size_t)v[0]*v[1]*4U);
        decoded=malloc((size_t)v[1]*(v[0]*4U+1U)); last=malloc(v[0]*4U);
        staging=malloc(ISAAC_NP_STAGING_BYTES); state=malloc(ISAAC_NP_TINFL_STATE_BYTES);
        CHECK(idat && expected && decoded && last && staging && state);
        CHECK(fread(idat,1,v[5],f)==v[5]); CHECK(fread(expected,1,(size_t)v[0]*v[1]*4U,f)==(size_t)v[0]*v[1]*4U);
        memset(&p,0,sizeof p); p.width=v[0];p.height=v[1];p.channels=4;p.color_type=6;
        p.rowbytes=v[0]*4U;p.first_remaining=v[5];p.crc_entry=isaac_np_crc32(0U,"IDAT",4U);
        for(x=0;x<256U;++x)table[x]=(uint8_t)(255U-x);p.gamma_table=v[4]?table:NULL;
        work=(isaac_np_work){decoded,staging,ISAAC_NP_STAGING_BYTES,state,last,NULL};
        r=(Reader){idat,v[5],0}; CHECK(isaac_np_decode(&p,read_bytes,&r,&work,&result)==ISAAC_NP_OK);
        for(y=0;y<v[1];++y)CHECK(!memcmp(decoded+(size_t)y*(p.rowbytes+1U)+1U,expected+(size_t)y*p.rowbytes,p.rowbytes));
        compare_rows(v[0],v[1],v[2],v[3],v[4]);
        /* Existing longjmp-style abort before an intermediate row: no publish;
         * already-written rows match, private untouched suffix may differ. */
        if(v[0]==8U && v[1]==8U && !v[4]) {
            unsigned mode; uint8_t prefix[64];
            for(mode=0;mode<2U;++mode) { CPU c;
                setup(&c,8,8,8,8,0);acquire(256U,KAGE_VITA_TEXEL_PNG_LOADER_RETURN);
                memset(pixels,0xd3,256U);candidate=mode;abort_row=2U;
                if(!setjmp(abort_env)){row_loop(&c);CHECK(0);}
                CHECK(rows==2U && !published && !lease_live && !faults);
                if(!mode)memcpy(prefix,pixels,64U);else CHECK(!memcmp(prefix,pixels,64U));
                release_buffer();abort_row=UINT32_MAX;
            }
        }
        free(idat);free(expected);free(decoded);free(last);free(staging);free(state);
    }
    CHECK(fgetc(f)==EOF);fclose(f);
}
int main(int argc,char **argv) {
    CHECK(argc==2);
    pixels=map_at(BUFFER,KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES);
    object=map_at(OBJECT,REGION);pngmem=map_at(PNG,REGION);infomem=map_at(INFO,REGION);
    stackmem=map_at(STACK,REGION);rowtable=map_at(TABLE,REGION);
    valid_cases(argv[1]); guards(); CHECK(!lease_live && admitted_clears==8U);
    CHECK(isaac_vita_texel_scratch_oracle_reset()==0);
    printf("PNG texel init PASS: %u valid decoded-image row loops, %u admitted clears, 37 metadata rejections, "
           "scratch/fault/cleanup/import/errno; gpr=%d flags=%d guard=%d\n",
           comparisons,admitted_clears,GUEST_GPR_LOCAL,GUEST_FLAGS_LOCAL,GUEST_GENERATED_STACK_GUARD);
    return 0;
}
