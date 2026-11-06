/* Test only: fresh J835 Sprite GetAnimation/string-copy/delete bodies, real
 * Lua 5.3.3 and production bridge/heap. No engine replacement or diagnostic.
 * The compiler-cookie helper is an explicit peripheral fixture: the checked
 * happy-path predicate is copied from the read 5eacd7 body; mismatch fails. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <windows.h>
#include "lua.h"
#include "lauxlib.h"
#include "host_vita_lua.h"
#include "host_vita_heap.h"
#include "host_vita_import_id.h"
#include "host_vita_startup.h"

void sub_00445170(CPU *__restrict c);
void sub_003fa3b0(CPU *__restrict c);
void sub_00007de0(CPU *__restrict c);
void sub_005eb08e(CPU *__restrict c);
void sub_000079f0(CPU *__restrict c);
void sub_00443b10(CPU *__restrict c);
void sub_005fc38b(CPU *__restrict c);
void sub_005fc39b(CPU *__restrict c);
void sub_005fc3fb(CPU *__restrict c);
void sub_005fc40b(CPU *__restrict c);
void __real_sub_003f8e30(CPU *__restrict c);
void __real_sub_003f8c80(CPU *__restrict c);
#if defined(ISAAC_VITA_LUA_NATIVE_GETCLASS)
#include "host_vita_lua_getclass.h"
#endif
_Static_assert(sizeof(void *) == 4, "J835 userdata/string ABI is ILP32");
#define VA(r) ((uint32_t)GUEST_IMAGE_BASE+(uint32_t)(r))
#define TOKEN 0xfff16000U
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"string lifetime line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static CPU cpu;
_Alignas(16) static uint32_t stack[8192];
_Alignas(16) static unsigned char sprite[0x38]; /* only read field: +0x34 */
static uint32_t source_string[6]; /* 79f0: inline16, length+16, capacity+20 */
static unsigned char source_bytes[9000];
static lua_State *L;
static unsigned reference_mode, cpp_allocs, cpp_frees, cpp_live, cpp_peak;
#if defined(ISAAC_HEAP_FASTPATH_FIXTURE)
int fixture_setup(void);
int fixture_init(uint32_t,uint32_t);
#endif
static unsigned char *cpp_blocks[16];
static unsigned unexpected, cookie_calls;
unsigned char *g_guest_coverage_functions; /* no coverage storage in this host oracle */
static uint32_t *active_holder;
static unsigned unrefs, finalized, action_active;
static jmp_buf action_stop;

void isaac_vita_log(const char *format, ...) { (void)format; }
void guest_fault(CPU *__restrict c,uint32_t at,const char *what)
{
    c->fault_addr=at; c->fault=what;
    if(action_active) longjmp(action_stop,1);
    ++unexpected;
}
void guest_int3(CPU *__restrict c,uint32_t at) { guest_fault(c,at,"unexpected INT3"); }
int guest_stack_violation(CPU *__restrict c,uint32_t pc,uint32_t kind,uint32_t at,uint32_t size)
{ (void)pc;(void)kind;(void)size;guest_fault(c,at,"stack violation");return 0; }
int guest_stack_owner_violation(CPU *__restrict c,uint32_t pc)
{ guest_fault(c,pc,"stack owner");return 0; }
int isaac_vita_startup_map_path(CPU *__restrict c,uint32_t p,char *out,uint32_t cap)
{ (void)out;(void)cap;guest_fault(c,p,"no path fixture");return 0; }

static void note_cpp_alloc(void *p)
{
    unsigned i;
    if (!p) { ++unexpected;return; }
    for(i=0;i<16;++i) if (!cpp_blocks[i]) break;
    if(i==16) { ++unexpected;return; }
    cpp_blocks[i]=p; ++cpp_allocs; ++cpp_live;
    if(cpp_live>cpp_peak) cpp_peak=cpp_live;
}
static int cpp_owned(void *p)
{
    unsigned i;
    if(!p)return -1;
    for(i=0;i<16;++i) if(cpp_blocks[i]==p) return (int)i;
    return -1;
}
static void cpp_retire(void *p)
{
    int slot=cpp_owned(p);
    if(slot<0 || isaac_vita_guest_heap_owns(p)) { ++unexpected;return; }
    cpp_blocks[slot]=NULL; ++cpp_frees; --cpp_live;
}

void sub_003f8e30(CPU *__restrict c)
{
#if defined(ISAAC_VITA_LUA_NATIVE_GETCLASS)
    __wrap_sub_003f8e30(c);
#else
    __real_sub_003f8e30(c);
#endif
}
void sub_003f8c80(CPU *__restrict c)
{
#if defined(ISAAC_VITA_LUA_NATIVE_GETCLASS)
    __wrap_sub_003f8c80(c);
#else
    __real_sub_003f8c80(c);
#endif
}
void sub_005eacd7(CPU *__restrict c)
{
    ++cookie_calls;
    if(c->ecx!=ld32(VA(0x7aa3b4U))) guest_fault(c,0x5eacd7U,"cookie mismatch");
    (void)gpop(c);
}

/* Independently checked lifecycle reference for the read 445170 body. The
 * actual original C++ getter is shared, but the boxing/move/cleanup consumer
 * is native Lua C API. It does not replace the generated consumer in the test. */
static void reference_callback(CPU *c)
{
    uint32_t saved=c->esp, old_ecx=c->ecx, old_edx=c->edx;
    uint32_t value[6]={0U,0U,0U,0U,0U,15U};
    uint32_t ud;
    const char *bytes;
    size_t len;
    gpush(c,1U);gpush(c,VA(0x80684bU));gpush(c,0xfff15000U);
    c->ecx=(uint32_t)(uintptr_t)L;c->edx=1U;sub_003f8e30(c);
    c->esp+=8U;ud=c->eax;
    gpush(c,(uint32_t)(uintptr_t)value);gpush(c,0xfff15004U);
    c->ecx=ld32(ud+4U);sub_003fa3b0(c);
    bytes=value[5]<16U?(const char *)value:(const char *)(uintptr_t)value[0];
    len=value[4];
    lua_pushlstring(L,bytes,len);
    if(value[5]>=16U) {
        uint32_t raw=value[0], request=value[5]+1U;
        if(request>=4096U) { raw=ld32(raw-4U);request+=35U; }
        gpush(c,request);gpush(c,raw);gpush(c,0xfff15008U);
        sub_005eb08e(c);c->esp+=8U;
    }
    c->ecx=old_ecx;c->edx=old_edx;c->esp=saved;c->eax=1U;(void)gpop(c);
}
void guest_call(CPU *__restrict c,uint32_t target)
{
    if(target-TOKEN<67U) {
        if(target-TOKEN==62U && active_holder) {
            /* Probe BEFORE real luaL_unref: the original destructor must
             * already have retired/reset its string, and use full words. */
            if(cpp_live || active_holder[6]!=0U || active_holder[7]!=15U ||
                    ((unsigned char *)(active_holder+2))[0]!=0U ||
                    ld32(c->esp+4U)!=active_holder[0] ||
                    ld32(c->esp+8U)!=(uint32_t)LUA_REGISTRYINDEX ||
                    ld32(c->esp+12U)!=active_holder[1])
                guest_fault(c,target,"LuaRef cleanup/order/ABI mismatch");
            ++unrefs;
        }
        if(!isaac_vita_lua_import_indexed(c,target-TOKEN,NULL))
            guest_fault(c,target,"Lua import rejected");
    } else if(guest_direct_translated_target(target,0x445170U)) {
        if(reference_mode) reference_callback(c); else sub_00445170(c);
    } else if(guest_direct_translated_target(target,0x3fa3b0U)) {
        sub_003fa3b0(c);
    } else if(guest_direct_translated_target(target,0x606504U)) {
        if(!isaac_vita_heap_import_indexed(c,3U,NULL)) guest_fault(c,target,"malloc rejected");
        if(!c->fault) note_cpp_alloc((void *)(uintptr_t)c->eax);
    } else if(guest_direct_translated_target(target,0x6064fcU)) {
        void *p=(void *)(uintptr_t)ld32(c->esp+4U);
        if(!isaac_vita_heap_import_indexed(c,1U,NULL)) guest_fault(c,target,"free rejected");
        if(!c->fault && p) cpp_retire(p);
    } else if(guest_direct_translated_target(target,0x6064f8U)) {
        (void)isaac_vita_heap_import_indexed(c,0U,NULL);
    } else if(guest_direct_translated_target(target,0x606488U)) {
        uint32_t dest=ld32(c->esp+4U),src=ld32(c->esp+8U),n=ld32(c->esp+12U);
        memcpy((void *)(uintptr_t)dest,(void *)(uintptr_t)src,n);c->eax=dest;(void)gpop(c);
    } else if(guest_direct_translated_target(target,0x6065acU)) {
        guest_fault(c,target,"original invalid-parameter guard");
    } else { guest_fault(c,target,"unexpected string-lifetime dispatch"); }
}
static int import_call(unsigned index,const uint32_t *args,unsigned n)
{
    uint32_t before=cpu.esp;unsigned i;
    for(i=n;i;--i)gpush(&cpu,args[i-1]);gpush(&cpu,0xfff14000U);
    if(!isaac_vita_lua_import_indexed(&cpu,index,NULL)||cpu.fault||cpu.esp!=before-n*4U)return 0;
    cpu.esp+=n*4U;return 1;
}
static int getter_status(void)
{
    uint32_t args[]={ (uint32_t)(uintptr_t)L,1U,1U,0U,0U,0U };
    lua_getglobal(L,"GetAnimation");lua_getglobal(L,"object");
    return import_call(59U,args,6U)?(int)cpu.eax:-1;
}
static void fixture(unsigned n)
{
    unsigned i;
    memset(source_string,0,sizeof source_string);
    for(i=0;i<n;++i) source_bytes[i]=(unsigned char)('A'+i%23U);
    source_bytes[n]=0;
    source_string[4]=n;source_string[5]=n<16U?15U:(n|15U);
    if(n<16U)memcpy(source_string,source_bytes,n+1U);
    else source_string[0]=(uint32_t)(uintptr_t)source_bytes;
    st32((uint32_t)(uintptr_t)sprite+0x34U,(uint32_t)(uintptr_t)source_string);
}
static int witness_finalizer(lua_State *state)
{ (void)state;++finalized;return 0; }
static int full_collect(void)
{
    uint32_t args[]={(uint32_t)(uintptr_t)L,LUA_GCCOLLECT,0U};
    return import_call(58U,args,3U);
}
static void held_value(void)
{
    lua_newuserdata(L,16U);lua_newtable(L);
    lua_pushcfunction(L,witness_finalizer);lua_setfield(L,-2,"__gc");
    lua_setmetatable(L,-2);
}
static int check_holder(guest_fn action,uint32_t disp,unsigned length,int invalid)
{
    uint32_t saved_esp=cpu.esp,saved_ebp=cpu.ebp;
    uint32_t before_unref=unrefs, before_finalized=finalized;
    uint32_t raw=0U,aligned=0U;
    int ref,escaped;
    fixture(length);
    cpu.ebp=cpu.stack_ceiling-256U;
    cpu.esp=cpu.stack_ceiling-1024U;
    active_holder=(uint32_t *)(uintptr_t)(cpu.ebp-disp);
    memset(active_holder,0,32U);
    held_value();ref=luaL_ref(L,LUA_REGISTRYINDEX);
    CHECK(ref>65535 && (ref & 65535)>2);
    active_holder[0]=(uint32_t)(uintptr_t)L;active_holder[1]=(uint32_t)ref;
    gpush(&cpu,(uint32_t)(uintptr_t)source_string);gpush(&cpu,0xfff12000U);
    cpu.ecx=(uint32_t)(uintptr_t)(active_holder+2);sub_000079f0(&cpu);
    CHECK(cpp_live==(length>=16U?1U:0U) && !unexpected);
    if(invalid) {
        aligned=active_holder[2];raw=ld32(aligned-4U);
        st32(aligned-4U,aligned-128U); /* deliberately invalid OWN fixture header */
    }
    action_active=1U;escaped=setjmp(action_stop);
    if(!escaped) {
        cpu.ecx=(uint32_t)(uintptr_t)active_holder;
        gpush(&cpu,0xfff12004U);action(&cpu);
    }
    action_active=0U;
    if(invalid) {
        CHECK(escaped==1 && cpu.fault_addr==VA(0x6065acU) &&
              cpp_live==1U && unrefs==before_unref);
        lua_rawgeti(L,LUA_REGISTRYINDEX,ref);CHECK(lua_type(L,-1)==LUA_TUSERDATA);lua_pop(L,1);
        st32(aligned-4U,raw);cpu.fault=NULL;cpu.fault_addr=0U;
        cpu.esp=cpu.stack_ceiling-1024U;
        cpu.ecx=(uint32_t)(uintptr_t)active_holder;
        gpush(&cpu,0xfff12008U);sub_00443b10(&cpu); /* valid fixture cleanup, not guard weakening */
    } else CHECK(escaped==0 && cpu.fault==NULL);
    CHECK(cpp_live==0U && unrefs==before_unref+1U);
    lua_rawgeti(L,LUA_REGISTRYINDEX,ref & 65535);CHECK(lua_toboolean(L,-1));lua_pop(L,1);
    active_holder=NULL;
    CHECK(full_collect() && finalized==before_finalized+1U);
    cpu.esp=saved_esp;cpu.ebp=saved_ebp;
    return 0;
}
static int check_no_ref(int ref)
{
    uint32_t saved_esp=cpu.esp,saved_ebp=cpu.ebp,before=unrefs;
    fixture(27U);cpu.ebp=cpu.stack_ceiling-256U;cpu.esp=cpu.stack_ceiling-1024U;
    active_holder=(uint32_t *)(uintptr_t)(cpu.ebp-0x34U);
    memset(active_holder,0,32U);
    active_holder[0]=(uint32_t)(uintptr_t)L;active_holder[1]=(uint32_t)ref;
    gpush(&cpu,(uint32_t)(uintptr_t)source_string);gpush(&cpu,0xfff12100U);
    cpu.ecx=(uint32_t)(uintptr_t)(active_holder+2);sub_000079f0(&cpu);
    CHECK(cpp_live==1U);
    gpush(&cpu,0xfff12104U);cpu.ecx=(uint32_t)(uintptr_t)active_holder;sub_00443b10(&cpu);
    CHECK(unrefs==before+1U && cpp_live==0U && !unexpected);
    active_holder=NULL;cpu.esp=saved_esp;cpu.ebp=saved_ebp;
    return 0;
}
int main(int argc,char **argv)
{
    FILE *f;unsigned char *image;unsigned i,j,mode;
    const unsigned lengths[]={0U,1U,15U,16U,31U,32U,100U,4095U,4096U,8190U};
    uint32_t args[3],*payload;
    char original_error[512]={0};
    CHECK(argc==2);f=fopen(argv[1],"rb");CHECK(f);
    CHECK(!fseek(f,0,SEEK_END));
    {long n=ftell(f);CHECK(n>0 && !fseek(f,0,SEEK_SET));
     image=VirtualAlloc((void *)(uintptr_t)GUEST_IMAGE_BASE,(size_t)n,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
     CHECK(image==(void *)(uintptr_t)GUEST_IMAGE_BASE && fread(image,1,(size_t)n,f)==(size_t)n);}
    fclose(f);
    for(i=0;i<67;++i)st32(VA(0x606150U+i*4U),TOKEN+i);
    st32(VA(0x606504U),VA(0x606504U));st32(VA(0x6064fcU),VA(0x6064fcU));st32(VA(0x6064f8U),VA(0x6064f8U));
    st32(VA(0x606488U),VA(0x606488U)); /* PE: VCRUNTIME140.dll!memcpy */
    st32(VA(0x6065acU),VA(0x6065acU));
    memset(&cpu,0,sizeof cpu);cpu.stack_floor=(uint32_t)(uintptr_t)stack;
    cpu.stack_ceiling=cpu.stack_floor+sizeof stack;cpu.stack_low_water=cpu.stack_ceiling;
    cpu.stack_owner=&cpu;cpu.esp=cpu.stack_ceiling-256U;
#if defined(ISAAC_HEAP_FASTPATH_FIXTURE)
    CHECK(fixture_setup()&&fixture_init(cpu.stack_floor,cpu.stack_ceiling));
#endif
    CHECK(import_call(39U,NULL,0U));L=(lua_State *)(uintptr_t)cpu.eax;CHECK(L);
    args[0]=(uint32_t)(uintptr_t)L;CHECK(import_call(10U,args,1U));
    lua_newtable(L);lua_pushvalue(L,-1);lua_setfield(L,-2,"__const");
    lua_pushliteral(L,"Sprite");lua_setfield(L,-2,"__type");
    lua_pushboolean(L,1);lua_rawsetp(L,-2,(void *)(uintptr_t)VA(0x804140U));
    lua_rawsetp(L,LUA_REGISTRYINDEX,(void *)(uintptr_t)VA(0x80684bU));
    payload=lua_newuserdata(L,8U);payload[0]=0x12345678U;payload[1]=(uint32_t)(uintptr_t)sprite;
    lua_rawgetp(L,LUA_REGISTRYINDEX,(void *)(uintptr_t)VA(0x80684bU));lua_setmetatable(L,-2);lua_setglobal(L,"object");
    payload=lua_newuserdata(L,4U);payload[0]=VA(0x3fa3b0U);
    args[1]=VA(0x445170U);args[2]=1U;CHECK(import_call(61U,args,3U));lua_setglobal(L,"GetAnimation");
    for(mode=0;mode<2;++mode) {
        unsigned before_a=cpp_allocs,before_f=cpp_frees;
        reference_mode=mode;
        for(i=0;i<sizeof lengths/sizeof lengths[0];++i) {
            uint32_t saved_source[6];fixture(lengths[i]);memcpy(saved_source,source_string,sizeof saved_source);
            for(j=0;j<1000U;++j) {
                size_t len;const char *s;
                CHECK(getter_status()==LUA_OK);s=lua_tolstring(L,-1,&len);
                CHECK(s && len==lengths[i] && !memcmp(s,source_bytes,len));
                CHECK(!memcmp(source_string,saved_source,sizeof saved_source));
                lua_pop(L,1);CHECK(cpp_live==0U && !unexpected);
            }
        }
        CHECK(cpp_allocs-before_a==7000U && cpp_frees-before_f==7000U && cpp_peak==1U);
        printf("[digest] mode=%u calls=10000 cpp_alloc/free=7000/7000 live=0 peak=1 alias=unchanged\n",mode);
        {
            unsigned a=cpp_allocs,frees=cpp_frees;
            lua_getglobal(L,"object");lua_setglobal(L,"saved_object");
            lua_pushliteral(L,"wrong type");lua_setglobal(L,"object");
            CHECK(getter_status()==LUA_ERRRUN && strstr(lua_tostring(L,-1),"Sprite expected"));
            if(mode==0U) {
                const char *error=lua_tostring(L,-1);
                CHECK(strlen(error)<sizeof original_error);
                strcpy(original_error,error);
            } else CHECK(strcmp(original_error,lua_tostring(L,-1))==0);
            lua_pop(L,1);
            CHECK(cpp_allocs==a && cpp_frees==frees && cpp_live==0U);
            lua_getglobal(L,"saved_object");lua_setglobal(L,"object");
            CHECK(getter_status()==LUA_OK);lua_pop(L,1); /* bridge still usable after error */
            printf("[digest] mode=%u argument-error before copy; recovery+reuse PASS\n",mode);
        }
    }
    /* Valid official references, not fabricated registry slots/free-list links.
     * Keep the low-ID aliases live; only the high holder reference is reused. */
    for(i=3U;i<0x12345U;++i) {
        lua_pushboolean(L,1);CHECK(luaL_ref(L,LUA_REGISTRYINDEX)==(int)i);
    }
    CHECK(check_holder(sub_00443b10,0x34U,0U,0)==0);
    CHECK(check_holder(sub_005fc38b,0x34U,27U,0)==0);
    CHECK(check_holder(sub_005fc39b,0x80U,4096U,0)==0);
    CHECK(check_holder(sub_005fc3fb,0x38U,27U,0)==0);
    CHECK(check_holder(sub_005fc40b,0x8cU,4096U,0)==0);
    CHECK(check_holder(sub_00443b10,0x34U,4096U,1)==0);
    CHECK(check_no_ref(LUA_REFNIL)==0 && check_no_ref(LUA_NOREF)==0);
    for(i=3U;i<0x12345U;++i)luaL_unref(L,LUA_REGISTRYINDEX,(int)i);
    CHECK(full_collect());
    puts("[digest] LuaRef direct+4 owned EH action entries: tidy BEFORE full-word unref; REF_NIL/NOREF; invalid aligned header stops before unref; valid teardown PASS");
    {
        size_t before=isaac_vita_guest_heap_test_live_count();
        int valid=0;unsigned a=cpp_allocs,frees=cpp_frees;
        unsigned char *p=isaac_vita_guest_realloc(NULL,32U,&valid);
        CHECK(p && valid && isaac_vita_guest_heap_test_live_count()==before+1U);
        memset(p,0x6b,32U);
        CHECK(!isaac_vita_guest_free(p+1U));
        CHECK(!isaac_vita_guest_realloc(p+1U,48U,&valid) && !valid &&
              isaac_vita_guest_heap_owns(p) && p[0]==0x6b && p[31]==0x6b);
        CHECK(!isaac_vita_guest_realloc(p,0U,&valid) && valid &&
              isaac_vita_guest_heap_test_live_count()==before);
        gpush(&cpu,0U);gpush(&cpu,0xfff13000U);guest_call(&cpu,VA(0x6064fcU));cpu.esp+=4U;
        CHECK(!cpu.fault && cpp_allocs==a && cpp_frees==frees && cpp_live==0U);
        puts("[digest] real heap freeNULL/reallocNULL/reallocp0 and rejected interior aliases: ownership/content unchanged PASS");
    }
    args[0]=(uint32_t)(uintptr_t)L;CHECK(import_call(0U,args,1U));
    CHECK(isaac_vita_guest_heap_test_live_count()==0U && cpp_live==0U &&
          cpp_allocs==cpp_frees && !unexpected);
    printf("[digest] final cpp_success_alloc/free=%u/%u live=0 guest_ledger=0 cookie_checks=%u\n",
           cpp_allocs,cpp_frees,cookie_calls);
#if defined(ISAAC_HEAP_FASTPATH_FIXTURE)
    /* No pointer digest: the same binary varies run to run (Lua seed/CRT);
     * pointer equality is the deterministic heap trace oracle's job. */
    CHECK(isaac_vita_guest_heap_test_storage_reset());
#endif
    puts("Original GetAnimation temporary lifetime: PASS; real bridge/heap; native reference and cold Lua close; no runtime fix");
    return 0;
}
