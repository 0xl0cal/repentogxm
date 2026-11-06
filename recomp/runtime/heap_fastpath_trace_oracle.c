/* Deterministic trace + genuine simultaneous public-heap callers. No shared
 * translated CPU/Lua state and no timing assertion masquerading as Vita FPS. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <windows.h>
#include "host_vita_heap.h"
#include "host_vita_import_id.h"
#if defined(ISAAC_VITA_HEAP_STAGE_PROFILE)
#include "host_vita_heap_stage.h"
#endif
int fixture_setup(void);
int fixture_init(uint32_t,uint32_t);
unsigned fixture_observers(void);
void fixture_fail_next(void);
static _Alignas(16) uint32_t frame[8];
static uint64_t digest=UINT64_C(1469598103934665603);
static HANDLE start_event;
static atomic_uint worker_errors;
#define CHECK(x) do{if(!(x)){fprintf(stderr,"heap trace line %d: %s\n",__LINE__,#x);return 1;}}while(0)
void isaac_vita_log(const char *f,...){(void)f;}
void guest_fault(CPU *__restrict c,uint32_t at,const char *why){c->fault=why;c->fault_addr=at;}
int guest_stack_violation(CPU *__restrict c,uint32_t pc,uint32_t kind,uint32_t a,uint32_t n)
{(void)kind;(void)a;(void)n;guest_fault(c,pc,"stack");return 0;}
int guest_stack_owner_violation(CPU *__restrict c,uint32_t p){guest_fault(c,p,"owner");return 0;}
static void fold(uint64_t value){digest=(digest^value)*UINT64_C(1099511628211);}
static uint32_t imp(unsigned kind,uint32_t p,uint32_t owner,int *fault)
{
    CPU c;memset(&c,0,sizeof c);memset(frame,0,sizeof frame);
    frame[0]=kind==3U?ISAAC_VITA_HEAP_WRAPPER_MALLOC_RETURN_RVA:0x3e3806U;
    frame[1]=p;frame[3]=owner;
    c.esp=(uint32_t)(uintptr_t)frame;c.stack_floor=c.esp;
    c.stack_ceiling=c.esp+sizeof frame;c.stack_low_water=c.esp;c.stack_owner=&c;
    if(!isaac_vita_heap_import_indexed(&c,kind,NULL))abort();
    *fault=c.fault!=NULL;return c.eax;
}
static DWORD WINAPI worker(void *arg)
{
    unsigned i,id=(unsigned)(uintptr_t)arg;
    WaitForSingleObject(start_event,INFINITE);
    for(i=0;i<12000U;++i){
        int valid=0;size_t n=16U+((i+id)%16U)*16U;
        unsigned char *p=isaac_vita_guest_malloc(n),*q;
        if(!p){atomic_fetch_add(&worker_errors,1);break;}
        memset(p,(int)id,n);
        q=isaac_vita_guest_realloc(p,n+16U,&valid);
        if(!q||!valid||q[0]!=(unsigned char)id||q[n-1]!=(unsigned char)id||
                !isaac_vita_guest_free(q)){atomic_fetch_add(&worker_errors,1);break;}
    }
    return 0;
}
int main(void)
{
    isaac_vita_heap_overflow_forbidden_ranges ranges;
    isaac_vita_guest_heap_telemetry_snapshot ov;
    isaac_vita_guest_heap_test_probe_stats before,after;
    isaac_vita_room_entry_slab_snapshot slab;
    HANDLE threads[6];unsigned i;int fault,valid=0;
    unsigned char *p,*q;uintptr_t boundary[4];
    LARGE_INTEGER t0,t1,hz;
    CHECK((uintptr_t)frame<=UINT32_MAX-sizeof frame && fixture_setup());
    CHECK(fixture_init((uint32_t)(uintptr_t)frame,(uint32_t)(uintptr_t)frame+sizeof frame));
    CHECK(isaac_vita_guest_heap_test_forbidden_ranges(0x82ad1560U,0x05100000U,
          (uint32_t)(uintptr_t)frame,(uint32_t)(uintptr_t)frame+sizeof frame,&ranges));
    boundary[0]=ranges.retained_newlib.begin;boundary[1]=ranges.retained_newlib.end-1U;
    boundary[2]=ranges.retained_newlib.end;boundary[3]=ranges.retained_newlib.begin-1U;
    for(i=0;i<4U;++i){CHECK(!isaac_vita_guest_free((void *)boundary[i]));fold(boundary[i]);}
    CHECK(isaac_vita_guest_free(NULL));
    for(i=0;i<3000U;++i){
        size_t n=16U+(i%12U)*16U;
        p=isaac_vita_guest_malloc(n);CHECK(p);fold((uintptr_t)p);memset(p,0x71,n);
        CHECK(!isaac_vita_guest_free(p+1U));
        CHECK(!isaac_vita_guest_realloc(p+1U,n+32U,&valid)&&!valid);
        q=isaac_vita_guest_realloc(p,n+16U,&valid);CHECK(q&&valid&&q[0]==0x71);fold((uintptr_t)q);
        CHECK(isaac_vita_guest_free(q));
    }
    fixture_fail_next();p=isaac_vita_guest_malloc(32U);CHECK(p);fold((uintptr_t)p);
    CHECK(isaac_vita_guest_free(p)); /* generic overflow, never the native range */
    p=(unsigned char *)(uintptr_t)imp(3U,ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES,
                                     ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA,&fault);
    CHECK(p&&!fault);fold((uintptr_t)p);CHECK(!isaac_vita_guest_free(p+1U));
    (void)imp(1U,(uint32_t)(uintptr_t)p,0U,&fault);CHECK(!fault);
    CHECK(!isaac_vita_guest_free(p));
    CHECK(isaac_vita_guest_heap_test_room_snapshot(&slab));
    CHECK(slab.frees==1U && slab.live_slots==0U);
    CHECK(isaac_vita_guest_heap_telemetry_snapshot_get(&ov));fold(ov.native_failures);fold(ov.pool_allocations);fold(ov.pool_frees);
    printf("[trace] digest=%016llx ledger=%zu observers=%u slab=%u/%u nf/pf=%u/%u\n",
           (unsigned long long)digest,isaac_vita_guest_heap_test_live_count(),fixture_observers(),
           slab.allocations,slab.frees,ov.native_failures,ov.pool_failures);
    {
        /* Floor totals with one live block: 1 normally, 0 when the lean
         * import skips the measurement-only floor sidecars. */
        isaac_vita_guest_heap_floor_lifetime_snapshot fl;
        p=isaac_vita_guest_malloc(48U);CHECK(p);
        memset(&fl,0,sizeof fl);(void)isaac_vita_guest_heap_floor_lifetime_snapshot_get(&fl);
        printf("[floor] live_total=%u\n",(unsigned)fl.total_count);
        CHECK(isaac_vita_guest_free(p)&&isaac_vita_guest_heap_test_live_count()==0U);
    }
#if defined(ISAAC_VITA_HEAP_STAGE_PROFILE)
    {
        /* 6400 import pairs = exactly 100 timed spans per stage whatever the
         * ordinal phase; counts are exact; z gets one empty bracket per 64
         * of each whole-import stage. */
        IsaacVitaHeapStageCounters s;
        unsigned k;
        isaac_vita_heap_stage_take(NULL);
        for(i=0;i<6400U;++i){
            p=(unsigned char *)(uintptr_t)imp(3U,32U,0x123456U,&fault);CHECK(p&&!fault);
            (void)imp(1U,(uint32_t)(uintptr_t)p,0U,&fault);CHECK(!fault);
        }
        isaac_vita_heap_stage_take(&s);
        CHECK(s.count[ISAAC_VITA_HEAP_COUNT_FREE]==6400U&&s.count[ISAAC_VITA_HEAP_COUNT_MALLOC]==6400U);
        CHECK(s.count[ISAAC_VITA_HEAP_COUNT_FREE_SLAB_FAST]==6400U&&s.count[ISAAC_VITA_HEAP_COUNT_FREE_NULL]==0U);
        CHECK(s.count[ISAAC_VITA_HEAP_COUNT_FREE_SLAB]==0U&&s.count[ISAAC_VITA_HEAP_COUNT_MALLOC_NULL]==0U);
        /* The route call is bracketed even when its fast reject answers. */
        for(k=0;k<ISAAC_VITA_HEAP_STAGE_EMPTY;++k)CHECK(s.timer[k].n==100U);
        CHECK(s.timer[ISAAC_VITA_HEAP_STAGE_EMPTY].n==200U);
        printf("[stages] free a=%u/%u slab=%u/%u obs=%u/%u lock=%u/%u ledger=%u/%u native=%u/%u post=%u/%u "
               "malloc a=%u/%u native=%u/%u z=%u/%u\n",
               s.timer[0].n,s.timer[0].us,s.timer[2].n,s.timer[2].us,s.timer[3].n,s.timer[3].us,
               s.timer[4].n,s.timer[4].us,s.timer[5].n,s.timer[5].us,s.timer[6].n,s.timer[6].us,
               s.timer[7].n,s.timer[7].us,s.timer[8].n,s.timer[8].us,s.timer[12].n,s.timer[12].us,
               s.timer[15].n,s.timer[15].us);
    }
#if defined(ISAAC_VITA_HEAP_FREE_COST_PROFILE)
    {
        /* ph120.hg/hv.  The cost ordinal advances with the ledger span's
         * (both in vita_heap_guest_free_impl), so its phase 32 never meets a
         * timed g; it meets a timed whole span a (F_ALL phase 16) for all of
         * a lockstep block or for none of it.  One slab-handled free between
         * two blocks advances F_ALL alone, so at most one block of 6400
         * pairs is skipped and the other times exactly 6400/64 = 100 calls.
         * This heap build links no fixed-block owner: the prefilter answers
         * "skip" for every import free. */
        IsaacVitaHeapFreeCost cost[2];
        unsigned b,timed=0U;
        for(b=0U;b<2U;++b){
            isaac_vita_heap_free_cost_take(NULL);
            for(i=0;i<6400U;++i){
                p=(unsigned char *)(uintptr_t)imp(3U,32U,0x123456U,&fault);CHECK(p&&!fault);
                (void)imp(1U,(uint32_t)(uintptr_t)p,0U,&fault);CHECK(!fault);
            }
            isaac_vita_heap_free_cost_take(&cost[b]);
            CHECK(cost[b].lookups==6400U&&cost[b].removes==6400U);
            CHECK(cost[b].lookup_probes>=6400U&&cost[b].lookup_probe_max>=1U);
            CHECK(cost[b].timer[ISAAC_VITA_HEAP_COST_K].n==cost[b].timer[ISAAC_VITA_HEAP_COST_E].n&&
                  cost[b].timer[ISAAC_VITA_HEAP_COST_E].n==cost[b].timer[ISAAC_VITA_HEAP_COST_R].n);
            CHECK(cost[b].timer[ISAAC_VITA_HEAP_COST_K].n==0U||cost[b].timer[ISAAC_VITA_HEAP_COST_K].n==100U);
            timed+=cost[b].timer[ISAAC_VITA_HEAP_COST_K].n;
#if defined(ISAAC_VITA_HEAP_FREE_PREFILTER)
            CHECK(cost[b].prefilter_skip==6400U&&cost[b].prefilter_maybe==0U);
#else
            CHECK(!cost[b].prefilter_skip&&!cost[b].prefilter_maybe);
#endif
            CHECK(!cost[b].nv[0]); /* no Vorbis observer linked: weak census absent */
            CHECK(cost[b].ledger_live==0U&&cost[b].ledger_capacity>0U);
            if(!b){
                p=(unsigned char *)(uintptr_t)imp(3U,ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES,
                                                 ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA,&fault);
                CHECK(p&&!fault);
                (void)imp(1U,(uint32_t)(uintptr_t)p,0U,&fault);CHECK(!fault);
            }
        }
        CHECK(timed==100U||timed==200U);
        printf("[cost] blocks k=%u/%u lookups=%u probes=%u/%u removes=%u scan=%u/%u moves=%u "
               "lease=%u prefilter skip=%u maybe=%u\n",
               cost[0].timer[ISAAC_VITA_HEAP_COST_K].n,cost[1].timer[ISAAC_VITA_HEAP_COST_K].n,
               cost[1].lookups,cost[1].lookup_probes,cost[1].lookup_probe_max,cost[1].removes,
               cost[1].remove_scan,cost[1].remove_scan_max,cost[1].remove_moves,cost[1].lease_scans,
               cost[1].prefilter_skip,cost[1].prefilter_maybe);
    }
#endif
#endif
    isaac_vita_guest_heap_test_probe_stats_reset();
    QueryPerformanceFrequency(&hz);QueryPerformanceCounter(&t0);
    for(i=0;i<100000U;++i){p=isaac_vita_guest_malloc(32U);CHECK(p);CHECK(isaac_vita_guest_free(p));}
    QueryPerformanceCounter(&t1);
    CHECK(isaac_vita_guest_heap_test_probe_stats_snapshot(&after));
    printf("[bench] malloc/free=100000 time_us=%llu locks=%zu spins=%zu\n",
           (unsigned long long)((t1.QuadPart-t0.QuadPart)*1000000/hz.QuadPart),after.lock_acquisitions,after.lock_spins);
    CHECK(isaac_vita_guest_heap_test_probe_stats_snapshot(&before));
    start_event=CreateEvent(NULL,TRUE,FALSE,NULL);CHECK(start_event);
    for(i=0;i<6U;++i){threads[i]=CreateThread(NULL,0,worker,(void *)(uintptr_t)(i+1U),0,NULL);CHECK(threads[i]);}
    SetEvent(start_event);WaitForMultipleObjects(6U,threads,TRUE,INFINITE);
    for(i=0;i<6U;++i)CloseHandle(threads[i]);CloseHandle(start_event);
    CHECK(!atomic_load(&worker_errors)&&isaac_vita_guest_heap_test_live_count()==0U);
    CHECK(isaac_vita_guest_heap_test_probe_stats_snapshot(&after));
    printf("[threads] six callers x12000 malloc/realloc/free PASS locks=%zu spins=%zu\n",
           after.lock_acquisitions-before.lock_acquisitions,after.lock_spins-before.lock_spins);
    CHECK(isaac_vita_guest_heap_telemetry_snapshot_get(&ov)&&ov.accounting_valid&&!ov.terminal);
    CHECK(isaac_vita_guest_heap_test_storage_reset());
    puts("Heap slab reject trace/NULL/alias/boundary/domain/contention: PASS");return 0;
}
