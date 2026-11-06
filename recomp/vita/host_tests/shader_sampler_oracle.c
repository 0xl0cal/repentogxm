/* Fresh original translated body -> production GL adapters -> real Vita
 * typed wrappers -> native API sinks, versus the exact sampler replay.
 * Shared native fixtures are regenerated from gl_vita_backend_oracle.c.
 * No GPU/FPS claim; native driver/compiler/scene acceptance stays separate. */
#include <windows.h>
#include <assert.h>
#include "guest.h"
#include "gl_bridge.h"
#include "gl_vita_backend.h"
static uint32_t sampler_mode, sampler_events[16][4], sampler_n, sampler_dynamic;
static int sampler_priority=1;
static uint32_t sampler_mutate, sampler_self, sampler_image;
static int32_t sampler_location;
static int32_t sampler_lookup(uint32_t program,const char *name)
{
    uint32_t hash=2166136261u;
    if(sampler_location<0 || !program) return -1;
    while(*name) {hash=(hash^(uint8_t)*name++)*16777619u;}
    return (int32_t)((hash ^ (program*17u) ^ (uint32_t)sampler_location)&0x7fffu);
}
static void sampler_hook(uint32_t kind,uint32_t a,uint32_t b,uint32_t d)
{
    if (!sampler_mode) return;
    assert(sampler_n < 16u);
    sampler_events[sampler_n][0]=kind;sampler_events[sampler_n][1]=a;
    sampler_events[sampler_n][2]=b;sampler_events[sampler_n++][3]=d;
    if(kind==1u && sampler_mutate) {
        st32(sampler_image+0x20u,2u);st32(sampler_image+0x7cu,919u);
    }
    if(kind==2u && sampler_mutate) st32(sampler_self+0x28u,23u);
}
#include "sampler_native_fixture.inc"
#undef glUniform1i
int guest_gl_dynamic_first_holds(void) { return sampler_priority; }
/* Link-only cold TLS/import closure. No case may execute these; the actual
 * GL adapters, native wrappers and checked stack bodies execute below. */
unsigned g_host_import_calls, g_host_dynamic_calls;
int guest_host_import(CPU *__restrict c,const char *name)
{ (void)c;(void)name;assert(!"unexpected import");return 0; }
int guest_host_dynamic(CPU *__restrict c,uint32_t token)
{ (void)c;(void)token;assert(!"unexpected cold dispatch");return 0; }
int isaac_vita_gl_dynamic_counted(CPU *__restrict c,uint32_t token,unsigned *count)
{ (void)c;(void)token;(void)count;assert(!"unexpected cold TLS GL dispatch");return 0; }
void guest_call(CPU *__restrict c,uint32_t token)
{
    if (!guest_gl_dispatch_counted(c,token,&sampler_dynamic))
        guest_fault(c,token,"sampler fixture: unknown dynamic token");
}
void orig_sub_00567430(CPU *__restrict);
void orig_mem_sub_00567430(CPU *__restrict);
void __wrap_sub_00567430(CPU *__restrict);
void mem_wrap_sub_00567430(CPU *__restrict);
int oracle_sampler_probe(CPU *);
static uint32_t sampler_stack[160];
static CPU sampler_cpu;
typedef struct result {
    CPU cpu;
    uint32_t low, events[16][4], n, dynamic, frame[4], image[32];
} result;
static void prepare(uint32_t unit,uint32_t kind,int32_t location,unsigned hostile,unsigned mutation)
{
    memset(&sampler_cpu,0,sizeof sampler_cpu);memset(sampler_stack,0xa5,sizeof sampler_stack);
    sampler_cpu.eax=0x12345678u;sampler_cpu.ecx=sampler_self;
    sampler_cpu.edx=0x76543210u;sampler_cpu.ebx=0x89abcdefu;
    sampler_cpu.esi=0x10203040u;sampler_cpu.edi=0xfedcba98u;sampler_cpu.ebp=0x99887766u;
    sampler_cpu.stack_floor=(uint32_t)(uintptr_t)sampler_stack;
    sampler_cpu.stack_ceiling=(uint32_t)(uintptr_t)(sampler_stack+160);
    sampler_cpu.stack_owner=&sampler_cpu;sampler_cpu.stack_low_water=sampler_cpu.stack_ceiling;
    sampler_cpu.esp=(uint32_t)(uintptr_t)(sampler_stack+100);
    st32(sampler_cpu.esp,0x5672b1u);
    st32(sampler_cpu.esp+4u,GUEST_IMAGE_BASE+0x00800200u);
    st32(sampler_cpu.esp+8u,unit);st32(sampler_cpu.esp+12u,sampler_image);
    memset((void *)(uintptr_t)sampler_self,0,44u);
    st32(sampler_self,GUEST_IMAGE_BASE+0x0075e0a8u);st32(sampler_self+0x28u,7u);
    memset((void *)(uintptr_t)sampler_image,0,128u);
    st32(sampler_image+0x20u,kind);st32(sampler_image+0x78u,101u);st32(sampler_image+0x7cu,202u);
    strcpy((char *)(uintptr_t)(GUEST_IMAGE_BASE+0x00800200u),"Texture0");
    /* A foreign vtable is a valid original-body input, but not an eligible
     * replay. Heap/source-pointer names and stack aliasing are fallback too. */
    if(hostile==1u) st32(sampler_self,0u);
    if(hostile==2u) st32(sampler_cpu.esp+4u,(uint32_t)(uintptr_t)"Texture0");
    if(hostile==3u) {st32(sampler_cpu.esp+12u,sampler_cpu.esp+32u);st32(sampler_cpu.esp+64u,kind);}
    if(hostile==4u) st32(sampler_self+0x28u,0u);
    if(hostile==5u) strcpy((char *)(uintptr_t)(GUEST_IMAGE_BASE+0x00800200u),"MissingOrRelinked");
    sampler_n=sampler_dynamic=0u;memset(sampler_events,0,sizeof sampler_events);
    sampler_location=location;s_native_uniform_location=location;
    sampler_mutate=mutation;sampler_mode=1u;
    gl_vita_backend_install();
    st32(GUEST_IMAGE_BASE+0x007c293cu,guest_gl_resolve("glActiveTexture"));
    st32(GUEST_IMAGE_BASE+0x007c2978u,guest_gl_resolve("glBindTexture"));
    st32(GUEST_IMAGE_BASE+0x007bf9bcu,guest_gl_resolve("glGetUniformLocation"));
    st32(GUEST_IMAGE_BASE+0x007bf96cu,guest_gl_resolve("glUniform1i"));
    if(hostile==6u) st32(GUEST_IMAGE_BASE+0x007c293cu,guest_gl_resolve("glCullFace"));
}
static void save(result *out)
{
    memcpy(&out->cpu,&sampler_cpu,sizeof sampler_cpu);out->low=sampler_cpu.stack_low_water;
    memcpy(out->events,sampler_events,sizeof sampler_events);out->n=sampler_n;out->dynamic=sampler_dynamic;
    memcpy(out->frame,sampler_stack+100,sizeof out->frame);
    memcpy(out->image,(void *)(uintptr_t)sampler_image,sizeof out->image);
    gl_vita_backend_uninstall();
}
static void decline_controls(void)
{
    unsigned k;
    for(k=0u;k<14u;++k) {
        CPU before;uint32_t memory[160];guest_gl_backend backend;
        prepare(0u,1u,7,0u,0u);sampler_priority=1;
        switch(k) {
        case 0u:sampler_cpu.stack_owner=NULL;break;
        case 1u:sampler_cpu.esp=sampler_cpu.stack_floor-4u;break;
        case 2u:sampler_cpu.esp=sampler_cpu.stack_floor+24u;break;
        case 3u:sampler_cpu.esp=sampler_cpu.stack_ceiling-12u;break;
        case 4u:sampler_cpu.ecx=0u;break;
        case 5u:st32(sampler_cpu.esp+12u,0u);break;
        case 6u:st32(sampler_cpu.esp+12u,UINT32_MAX-63u);break;
        case 7u:st32(sampler_cpu.esp+4u,0u);break;
        case 8u:sampler_priority=0;break;
        case 9u:st32(GUEST_IMAGE_BASE+0x007bf96cu,0u);break;
        case 10u:backend=*guest_gl_installed_backend();backend.glUniform1i=NULL;guest_gl_install_backend(&backend);break;
        case 11u:gl_vita_backend_uninstall();break;
        case 12u:assert(guest_gl_owned_enter(&sampler_cpu));break;
        case 13u:sampler_cpu.stack_floor=GUEST_IMAGE_BASE+0x00800000u;
                 sampler_cpu.stack_ceiling=GUEST_IMAGE_BASE+0x00803000u;
                 sampler_cpu.esp=GUEST_IMAGE_BASE+0x00800800u;break;
        }
        memcpy(&before,&sampler_cpu,sizeof before);memcpy(memory,sampler_stack,sizeof memory);
        assert(!oracle_sampler_probe(&sampler_cpu));
        assert(!memcmp(&before,&sampler_cpu,sizeof before));
        assert(!memcmp(memory,sampler_stack,sizeof memory));
        assert(!sampler_n && !sampler_dynamic);
        if(k==12u)guest_gl_owned_leave(&sampler_cpu,guest_gl_resolve("glUniform1i"));
        gl_vita_backend_uninstall();sampler_priority=1;
    }
    puts("PASS sampler fail-closed: 14 owner/frame/image-stack/null/overflow/priority/slot/partial/uninstalled/busy controls, no CPU/memory/API changes");
}
int main(void)
{
    unsigned state,hostile,mutation,mem,cases=0u,elided=0u;
    result a,b;
    assert(VirtualAlloc((void *)(uintptr_t)GUEST_IMAGE_BASE,0x0085f000u,
           MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)==(void *)(uintptr_t)GUEST_IMAGE_BASE);
    sampler_self=GUEST_IMAGE_BASE+0x00800000u;sampler_image=GUEST_IMAGE_BASE+0x00800100u;
    for(mem=0u;mem<2u;++mem) for(state=0u;state<8u;++state)
    for(hostile=0u;hostile<7u;++hostile) for(mutation=0u;mutation<2u;++mutation) {
        uint32_t units[]={0u,1u,2u,15u,16u,UINT32_MAX,0x80000000u,7u};
        uint32_t kind=(state&1u)?2u:1u;
        int32_t location=(state&2u)?-1:(int32_t)(state*7u);
        prepare(units[state],kind,location,hostile,mutation);
        if(mem)orig_mem_sub_00567430(&sampler_cpu);else orig_sub_00567430(&sampler_cpu);save(&a);
        prepare(units[state],kind,location,hostile,mutation);
        if(mem)mem_wrap_sub_00567430(&sampler_cpu);else __wrap_sub_00567430(&sampler_cpu);save(&b);
        if(memcmp(&a.cpu,&b.cpu,sizeof a.cpu)) {
            unsigned j;const uint32_t *ra=(const uint32_t *)&a.cpu,*rb=(const uint32_t *)&b.cpu;
            printf("case mem=%u state=%u hostile=%u mutate=%u dyn=%u/%u\n",mem,state,hostile,mutation,a.dynamic,b.dynamic);
            for(j=0u;j<sizeof a.cpu/4u;++j)if(ra[j]!=rb[j])printf("CPU +%x ref=%08x opt=%08x\n",j*4u,ra[j],rb[j]);
            fflush(stdout);
        }
        assert(!memcmp(&a.cpu,&b.cpu,sizeof a.cpu));assert(a.low==b.low);
        assert(a.n==b.n && !memcmp(a.events,b.events,sizeof a.events));
        assert(!memcmp(a.frame,b.frame,sizeof a.frame) && !memcmp(a.image,b.image,sizeof a.image));
        if(hostile==1u || hostile==2u || hostile==3u || hostile==6u) assert(a.dynamic==b.dynamic);
        else {assert(b.dynamic==0u);elided+=a.dynamic;}
        ++cases;
    }
    printf("PASS sampler ORIGINAL-body differential: cases=%u, dynamic crossings elided=%u, both GPR modes, unit/capability/negative-location/default/mutation/source-pointer/stack-alias boundaries\n",cases,elided);
    decline_controls();
    return 0;
}
