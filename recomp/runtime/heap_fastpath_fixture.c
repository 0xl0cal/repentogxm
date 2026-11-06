/* Test-only deterministic native/kernel backends. Same addresses and allocation
 * policy for OFF/ON; not a replacement allocator or a Vita RAM measurement. */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <windows.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include "host_vita_heap.h"

#define BANK_BASE ((uintptr_t)0x84000000U)
#define BANK_BYTES 0x00800000U
#define CELLS 2048U
typedef struct cell { unsigned char *p; size_t cap, used; atomic_int live; } cell;
static cell cells[CELLS];
static unsigned char *bank;
static size_t bump;
static unsigned count;
static atomic_uint observers;
static unsigned char *blocks[16];
static size_t block_sizes[16];
static unsigned block_live[16];
static unsigned native_calls, native_frees, native_fail;
char _end;
unsigned int _newlib_heap_size_user;
int fixture_setup(void)
{
    bank=VirtualAlloc((void *)BANK_BASE,BANK_BYTES,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    return bank==(void *)BANK_BASE;
}
void *fixture_malloc(size_t n)
{
    unsigned i;size_t span;
    ++native_calls;
    if(native_fail){--native_fail;return NULL;}
    n=n?n:1U;
    for(i=0;i<count;++i)if(!atomic_load(&cells[i].live) && cells[i].cap>=n)break;
    if(i==count){
        if(n>SIZE_MAX-31U || count==CELLS)return NULL;
        span=(n+31U)&~(size_t)15U;
        if(span>BANK_BYTES-bump)return NULL;
        cells[i].p=bank+bump+16U;cells[i].cap=span-16U;bump+=span;++count;
        memcpy(cells[i].p-16U,&i,sizeof i);
    }
    cells[i].used=n;atomic_store(&cells[i].live,1);
    return cells[i].p;
}
static cell *find_cell(void *p)
{
    unsigned i;
    if(!p || (uintptr_t)p<BANK_BASE+16U || (uintptr_t)p>=BANK_BASE+BANK_BYTES)return NULL;
    memcpy(&i,(unsigned char *)p-16U,sizeof i);
    return i<count && cells[i].p==p?&cells[i]:NULL;
}
void fixture_free(void *p)
{
    cell *c;if(!p)return;
    c=find_cell(p);
    if(!c || !atomic_load(&c->live))abort();
    atomic_store(&c->live,0);++native_frees;
}
void *fixture_calloc(size_t n,size_t sz)
{
    void *p;if(n && sz>SIZE_MAX/n)return NULL;
    p=fixture_malloc(n*sz);if(p)memset(p,0,n*sz);return p;
}
void *fixture_realloc(void *p,size_t n)
{
    cell *c;void *q;if(!p)return fixture_malloc(n);
    c=find_cell(p);if(!c || !atomic_load(&c->live))abort();
    if(!n){fixture_free(p);return NULL;}
    if(c->cap>=n){c->used=n;return p;}
    q=fixture_malloc(n);if(!q)return NULL;
    memcpy(q,p,c->used<n?c->used:n);fixture_free(p);return q;
}
void isaac_nv_guest_buffer_freed(void *p)
{
    if(p)atomic_fetch_add(&observers,1U); /* includes rejected non-NULL pointers */
}
unsigned fixture_observers(void){return atomic_load(&observers);}
unsigned fixture_native_calls(void){return native_calls;}
unsigned fixture_native_frees(void){return native_frees;}
void fixture_fail_next(void){native_fail=1U;}
int fixture_init(uint32_t floor,uint32_t ceiling)
{return isaac_vita_guest_heap_test_init(0x82ad1560U,0x05100000U,floor,ceiling);}

typedef struct space { unsigned char *base; size_t bytes, bump; cell entries[512]; unsigned count; } space;
static space pool;
SceUID sceKernelAllocMemBlock(const char *name,SceKernelMemBlockType kind,SceSize n,SceKernelAllocMemBlockOpt *opt)
{
    unsigned i;uintptr_t at;(void)name;(void)kind;(void)opt;
    for(i=0;i<16U;++i)if(!block_live[i])break;
    if(i==16U)return -1;
    at=(uintptr_t)0x90000000U+(uintptr_t)i*0x02000000U;
    blocks[i]=VirtualAlloc((void *)at,n,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(blocks[i]!=(void *)at)return -1;
    block_sizes[i]=n;block_live[i]=1U;return (SceUID)(100+i);
}
int sceKernelGetMemBlockBase(SceUID id,void **out)
{
    unsigned i=(unsigned)(id-100);if(i>=16U || !block_live[i])return -1;
    *out=blocks[i];return 0;
}
int sceKernelFreeMemBlock(SceUID id)
{
    unsigned i=(unsigned)(id-100);if(i>=16U || !block_live[i])return -1;
    if(!VirtualFree(blocks[i],0,MEM_RELEASE))return -1;
    blocks[i]=NULL;block_sizes[i]=0;block_live[i]=0;return 0;
}
SceClibMspace sceClibMspaceCreate(void *p,SceSize n)
{ memset(&pool,0,sizeof pool);pool.base=p;pool.bytes=n;return &pool; }
void sceClibMspaceDestroy(SceClibMspace s){(void)s;}
void *sceClibMspaceMalloc(SceClibMspace s,SceSize n)
{
    space *a=s;unsigned i;size_t span;n=n?n:1U;
    for(i=0;i<a->count;++i)if(!atomic_load(&a->entries[i].live) && a->entries[i].cap>=n)break;
    if(i==a->count){
        if(i==512U)return NULL;span=((size_t)n+7U)&~(size_t)7U;
        if(span>a->bytes-a->bump)return NULL;
        a->entries[i].p=a->base+a->bump;a->entries[i].cap=span;a->bump+=span;++a->count;
    }
    a->entries[i].used=n;atomic_store(&a->entries[i].live,1);return a->entries[i].p;
}
void sceClibMspaceFree(SceClibMspace s,void *p)
{
    space *a=s;unsigned i;for(i=0;i<a->count;++i)if(a->entries[i].p==p && atomic_load(&a->entries[i].live))break;
    if(i==a->count)abort();atomic_store(&a->entries[i].live,0);
}
void *sceClibMspaceCalloc(SceClibMspace s,SceSize n,SceSize z)
{void *p;if(n && z>UINT32_MAX/n)return NULL;p=sceClibMspaceMalloc(s,n*z);if(p)memset(p,0,n*z);return p;}
void *sceClibMspaceRealloc(SceClibMspace s,void *p,SceSize n)
{
    space *a=s;unsigned i;void *q;if(!p)return sceClibMspaceMalloc(s,n);
    for(i=0;i<a->count;++i)if(a->entries[i].p==p && atomic_load(&a->entries[i].live))break;
    if(i==a->count)abort();if(!n){sceClibMspaceFree(s,p);return NULL;}
    if(a->entries[i].cap>=n){a->entries[i].used=n;return p;}
    q=sceClibMspaceMalloc(s,n);if(!q)return NULL;
    memcpy(q,p,a->entries[i].used<n?a->entries[i].used:n);sceClibMspaceFree(s,p);return q;
}
