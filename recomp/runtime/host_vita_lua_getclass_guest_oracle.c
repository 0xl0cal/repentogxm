/* Guest-side oracle for the native Userdata::getClass / getExact seam.
 *
 * 32-bit host executable (the frozen Lua ABI is ILP32: raw lua_State and
 * userdata pointers cross the bridge).  It links
 *   - the GENERATED bodies of sub_003f8e30 / sub_003f8c80, extracted verbatim
 *     from the corpus by recomp/test_vita_lua_getclass.py (their IAT slot
 *     reads, string pushes and identity-key push use the PE's absolute guest
 *     addresses, so the oracle maps those pages at the very same addresses),
 *   - the production bridge host_vita_lua.c (ISAAC_VITA_LUA_NATIVE_GETCLASS=1),
 *   - the seam host_vita_lua_getclass.c with its __wrap_ entries,
 *   - pristine Lua 5.3.3,
 * and drives the translated body and the wrap on one bound CPU and one Lua
 * state, comparing eax / esp / callee-saved registers / the guest stack words
 * above esp / the Lua stack (depth, types, identities) / error texts.
 * __real_sub_* is this file's: it either runs the generated body (fallback
 * equivalence) or, for shapes where the translated body itself has undefined
 * behaviour (lua_rawget on a non-table), records that the seam handed over an
 * untouched CPU, an unchanged Lua stack top and unchanged guest stack bytes.
 * A __gc stress proves that a guest finalizer reached from a GC step inside
 * the native path finds the bridge owner.  The VERIFY build checks the seam's
 * own translated-vs-native comparison.
 * Hostile cases (review): LUA_ERRGCMM thrown by a guest __gc out of the GC
 * step inside the seam (longjmp across the native frames, bridge recovery),
 * a guest upvalue pseudo-index argument (vita_lua_index vs lgc_index), the
 * canBeConst byte with garbage upper bytes, the relative-index quirk
 * (index -2 checks one slot and returns another), an index beyond the top,
 * and the seam as the outermost owner while its own GC step runs a guest
 * __gc that re-enters the wrap (getExact, as the game's __gc chain does).
 *
 * Lua-state exactness (the shared gate of the RENDER60 S3.2 Lua levers):
 *   - every luaC_step is seen by this file's luaC_step (the test compiles
 *     lgc.c with -DluaC_step=isaac_oracle_luaC_step_real), which records the
 *     GC point: L->top, the live stack slots, debt, total bytes, collector
 *     state, CallInfo depth, and whether the step moved the Lua stack;
 *   - exact cases (no GC: debt forced negative): the translated body, the
 *     API path and the wrap start from the same string cache, debt and
 *     dead-slot residue and must leave the same string cache, debt, total
 *     bytes and live stack;
 *   - GC transcripts: the same script on fresh Lua states in a reset
 *     deterministic arena (same addresses), once per variant, under forced
 *     collector steps inside the seam, guest and Lua finalizers, a finalizer
 *     that grows the Lua stack (moves it) and one that rehashes the registry
 *     and a class table; the GC-point records, finalizer order and results
 *     must agree, and with ISAAC_VITA_LUA_GETCLASS_DIRECT the API path and
 *     the direct path must leave byte-identical arenas;
 *   - GC-point positions: a collector step leaves the debt negative, so the
 *     transcripts above step only at the FIRST luaC_checkGC of a call.  The
 *     position transcripts number the GC points of each call (1, 2, 3 ...),
 *     hold the positive debt back at every point before position N (the
 *     hook returns without stepping) and step at point N, for N = 1 .. 4;
 *     visits and steps are counted per (probe configuration, position) and
 *     must equal the table the translated body implies, so a GC point that
 *     is dropped, added or moved anywhere in a call changes the transcript;
 *   - ISAAC_VITA_LUA_GETCLASS_DIRECT: the node memo's hit, miss and stale
 *     paths, including a hit on a key whose value was removed. */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>      /* _msize: heap_poison_free */
#include <string.h>

#include <windows.h>

#include "lua.h"
#include "lauxlib.h"
#include "lstate.h"      /* G(L)/luaE_setdebt: the GC-inside-the-seam stress */
#include "lgc.h"         /* luaC_step (the GC-point hook), collector states */
#include "lfunc.h"       /* UpVal: the heap digest */
#include "ltable.h"      /* gkey/gval/gnext: the heap digest */

#include "host_vita_lua.h"
#include "host_vita_lua_getclass.h"
#include "host_vita_startup.h"

/* the generated bodies (lgc_oracle_generated.c) */
void sub_003f8e30(CPU *__restrict c);
void sub_003f8c80(CPU *__restrict c);

#define IMPORT_TOKEN_BASE 0xfff16000U   /* fake IAT slot word = token + Lua local index */
#define IMPORT_RETURN     0xfff15000U
#define LUA_IAT_FIRST_RVA 0x00606150U   /* Lua5.3.3r.dll!lua_close; 67 contiguous slots */
#define LUA_IAT_PAGE_RVA  0x00606000U
#define RDATA_PAGE_RVA    0x0074f000U
#define RET_GETCLASS      0x0040cc27U   /* guest_0122.c: call 0x3f8e30; add esp, 8 */
#define RET_GETEXACT      0x0045d480U   /* guest_0190.c: call 0x3f8c80; add esp, 4 */
#define GUEST_PROBE       0x00421000U
#define GUEST_GC          0x00421010U
#define GUEST_GC_RAISE    0x00421020U   /* __gc that raises through luaL_error */
#define GUEST_GC_EXACT    0x00421030U   /* __gc that calls getExact (nested wrap) */
/* class-table registry keys: light userdata the game takes from its .data */
#define KEY_A     0x9880685cU
#define KEY_B     0x98806860U
#define KEY_C     0x98806864U
#define KEY_X     0x98806868U
#define KEY_P0    0x9880686cU
#define KEY_UNREG 0x98806870U
#define KEY_G     0x98806874U
#define LUA_IMPORT_CLOSE        0U
#define LUA_IMPORT_OPENLIBS     10U
#define LUA_IMPORT_SETGLOBAL    37U
#define LUA_IMPORT_NEWSTATE_AUX 39U
#define LUA_IMPORT_LUAL_ERROR   41U
#define LUA_IMPORT_PCALLK       59U
#define LUA_IMPORT_PUSHCLOSURE  61U
#define STACK_WORDS  8192
#define ESP_HEADROOM 256U

#define VA(rva) ((uint32_t)GUEST_IMAGE_BASE + (uint32_t)(rva))
#define IDENTITY ((const void *)(uintptr_t)VA(ISAAC_LGC_IDENTITY_KEY_RVA))

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "native getclass guest oracle failed at %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

/* --- runtime stubs ---------------------------------------------------------- */

static unsigned char s_coverage[16384];
unsigned char *g_guest_coverage_functions = s_coverage;
unsigned char *g_guest_coverage_imports;
unsigned char *g_guest_coverage_cases;

static uint32_t s_fault_calls;
static uint32_t s_log_lines;

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    ++s_fault_calls;
    if (!c->fault) {
        c->fault_addr = address;
        c->fault = what;
    }
}

int guest_stack_violation(CPU *__restrict c, uint32_t pc, uint32_t kind,
                          uint32_t address, uint32_t size)
{
    (void)kind;
    (void)size;
    guest_fault(c, address, "oracle guest stack violation");
    c->fault_addr = pc ? pc : address;
    return 0;
}

int guest_stack_owner_violation(CPU *__restrict c, uint32_t pc)
{
    guest_fault(c, pc, "oracle guest stack owner violation");
    return 0;
}

/* --- deterministic arena (GC transcripts) -------------------------------------
 *
 * The transcripts run one script on fresh Lua states, once per variant.  While
 * s_arena_on, every allocation of the bridge's luaL_newstate allocator comes
 * from this reservation, which is zeroed and given empty free lists before
 * each run, so two runs that make the same allocation sequence get the same
 * addresses and leave the same bytes.  16-byte size classes below 64 KiB and
 * one first-fit list above; every reallocation that changes the size class
 * moves the block (the most hostile choice for code that keeps stack or node
 * pointers across an allocation). */
#define ARENA_BYTES   (96U << 20)
#define ARENA_HEADER  16U
#define ARENA_CLASSES 4096U
#define ARENA_LIVE    0xa110c8edU

static uint8_t *s_arena;
static int s_arena_on;
static uint32_t s_arena_bump, s_arena_high, s_arena_live, s_arena_failed;
static uint32_t s_arena_free[ARENA_CLASSES + 1U];   /* header offset + 1; 0 = empty */

static uint32_t *arena_header(uint32_t at)
{
    return (uint32_t *)(void *)(s_arena + at);
}

static int arena_owns(const void *pointer)
{
    return s_arena != NULL && (const uint8_t *)pointer >= s_arena + ARENA_HEADER &&
           (const uint8_t *)pointer < s_arena + ARENA_BYTES;
}

static uint32_t arena_class(uint32_t capacity)
{
    return capacity / 16U < ARENA_CLASSES ? capacity / 16U : ARENA_CLASSES;
}

static void *arena_alloc(size_t size)
{
    uint32_t capacity, cls, at;
    if (size == 0U || size > ARENA_BYTES / 2U) {
        ++s_arena_failed;
        return NULL;
    }
    capacity = ((uint32_t)size + 15U) & ~15U;
    cls = arena_class(capacity);
    if (cls < ARENA_CLASSES && s_arena_free[cls] != 0U) {
        at = s_arena_free[cls] - 1U;
        s_arena_free[cls] = arena_header(at)[1];
    } else {
        uint32_t *link = &s_arena_free[ARENA_CLASSES];
        at = UINT32_MAX;
        if (cls == ARENA_CLASSES) {
            while (*link != 0U) {
                uint32_t candidate = *link - 1U;
                if (arena_header(candidate)[0] >= capacity) {
                    *link = arena_header(candidate)[1];
                    at = candidate;
                    break;
                }
                link = &arena_header(candidate)[1];
            }
        }
        if (at == UINT32_MAX) {
            if (ARENA_BYTES - s_arena_bump < ARENA_HEADER + capacity) {
                ++s_arena_failed;
                return NULL;
            }
            at = s_arena_bump;
            s_arena_bump += ARENA_HEADER + capacity;
            if (s_arena_bump > s_arena_high)
                s_arena_high = s_arena_bump;
            arena_header(at)[0] = capacity;
        }
    }
    arena_header(at)[1] = ARENA_LIVE;
    ++s_arena_live;
    return s_arena + at + ARENA_HEADER;
}

/* Freed payloads are poisoned, so a read through a stale pointer (a memo
 * node in a freed node array, a pre-reallocation StkId) returns 0xA5 bytes
 * -- a tag no TValue has -- instead of the old, still-correct value and
 * shows up as a wrong result, a heap mismatch or a fault, not only in a
 * counter (with the memo validation removed, the unpoisoned
 * arena kept every comparison equal).  Deterministic, so runs stay
 * comparable. */
#define ARENA_POISON 0xa5

static void arena_free(void *pointer)
{
    uint32_t at = (uint32_t)((uint8_t *)pointer - s_arena) - ARENA_HEADER;
    uint32_t cls = arena_class(arena_header(at)[0]);
    if (arena_header(at)[1] != ARENA_LIVE) {
        ++s_arena_failed;              /* double free / foreign pointer */
        return;
    }
    memset(s_arena + at + ARENA_HEADER, ARENA_POISON, arena_header(at)[0]);
    arena_header(at)[1] = s_arena_free[cls];
    s_arena_free[cls] = at + 1U;
    --s_arena_live;
}

static void *arena_realloc(void *pointer, size_t size)
{
    uint32_t at, capacity;
    void *moved;
    if (pointer == NULL)
        return arena_alloc(size);
    at = (uint32_t)((uint8_t *)pointer - s_arena) - ARENA_HEADER;
    capacity = arena_header(at)[0];
    if ((((uint32_t)size + 15U) & ~15U) == capacity)
        return pointer;
    moved = arena_alloc(size);
    if (moved == NULL)
        return NULL;
    memcpy(moved, pointer, capacity < size ? capacity : size);
    arena_free(pointer);
    return moved;
}

static int arena_reset(void)
{
    if (s_arena == NULL) {
        s_arena = (uint8_t *)VirtualAlloc(NULL, ARENA_BYTES, MEM_RESERVE | MEM_COMMIT,
                                          PAGE_READWRITE);
        if (s_arena == NULL)
            return 0;
    }
    memset(s_arena, 0, s_arena_high);
    memset(s_arena_free, 0, sizeof s_arena_free);
    s_arena_bump = s_arena_high = s_arena_live = s_arena_failed = 0U;
    return 1;
}

void *isaac_vita_guest_malloc(size_t size)
{
    return s_arena_on ? arena_alloc(size) : malloc(size);
}

/* Outside the arena (exact and memo cases) the C heap gets the same two
 * properties: a freed block is poisoned (its size from the CRT's _msize; the
 * oracle is MSVC-only, it already uses VirtualAlloc) and a reallocation always
 * moves, so a stale pointer never reads the old bytes there either. */
static void heap_poison_free(void *pointer)
{
    if (pointer != NULL)
        memset(pointer, ARENA_POISON, _msize(pointer));
    free(pointer);
}

void *isaac_vita_guest_realloc(void *pointer, size_t size, int *valid_owner)
{
    void *moved;
    size_t old_size;
    if (valid_owner)
        *valid_owner = 1;
    if (arena_owns(pointer) || (pointer == NULL && s_arena_on))
        return arena_realloc(pointer, size);
    if (pointer == NULL || size == 0U)
        return realloc(pointer, size);
    moved = malloc(size);
    if (moved == NULL)
        return NULL;
    old_size = _msize(pointer);
    memcpy(moved, pointer, old_size < size ? old_size : size);
    heap_poison_free(pointer);
    return moved;
}

int isaac_vita_guest_free(void *pointer)
{
    if (arena_owns(pointer))
        arena_free(pointer);
    else
        heap_poison_free(pointer);
    return 1;
}

/* --- Lua-state snapshots ------------------------------------------------------ */

#define FNV64_INIT  0xcbf29ce484222325ULL
#define FNV64_PRIME 0x00000100000001b3ULL

static uint64_t fnv_bytes(uint64_t h, const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    while (size-- != 0U) {
        h ^= *p++;
        h *= FNV64_PRIME;
    }
    return h;
}

static uint64_t fnv_u32(uint64_t h, uint32_t value)
{
    return fnv_bytes(h, &value, sizeof value);
}

/* The bytes of a stack slot Lua writes: value_ and tt_, not the alignment
 * padding after tt_. */
#define SLOT_BYTES (offsetof(TValue, tt_) + sizeof(int))

/* What a slot means: its tag and the value field the tag selects (a nil or
 * boolean slot keeps stale bytes in the rest of value_). */
static uint64_t fnv_slot_meaning(uint64_t h, const TValue *o)
{
    h = fnv_u32(h, (uint32_t)rttype(o));
    if (iscollectable(o))
        return fnv_u32(h, (uint32_t)(uintptr_t)gcvalue(o));
    switch (ttype(o)) {
    case LUA_TNUMINT: {
        lua_Integer i = ivalue(o);
        return fnv_bytes(h, &i, sizeof i);
    }
    case LUA_TNUMFLT: {
        lua_Number n = fltvalue(o);
        return fnv_bytes(h, &n, sizeof n);
    }
    case LUA_TBOOLEAN:
        return fnv_u32(h, (uint32_t)bvalue(o));
    case LUA_TLIGHTUSERDATA:
        return fnv_u32(h, (uint32_t)(uintptr_t)pvalue(o));
    case LUA_TLCF:
        return fnv_u32(h, (uint32_t)(uintptr_t)fvalue(o));
    default:
        return h;
    }
}

/* One collector step as luaC_step (below) sees it on entry. */
typedef struct gc_record {
    uint32_t seq;
    uint32_t in_seam;
    int32_t gcstate;
    int32_t debt;
    uint32_t total;
    int32_t top;            /* L->top - L->stack */
    int32_t stacksize;
    int32_t ci_depth;
    uint32_t moved;         /* the step reallocated the Lua stack */
    uint32_t position;      /* n-th top-level GC point of this seam call (0: outside) */
    uint32_t deferred;      /* position transcript: debt held back, no step here */
    uint64_t live;          /* meaning of [stack, top) */
    uint64_t image;         /* meaning of the whole stack, dead slots included */
} gc_record;

#define GC_KEEP 2048U
#define GC_POSITIONS 8U         /* positions counted per call (the bodies have at most 3) */
#define PROBE_CONFIGS 7U

typedef struct gc_transcript {
    uint32_t points, in_seam, moved, moved_in_seam;
    uint32_t deferred;      /* GC points visited with the debt held back (not in points) */
    /* per (probe configuration, GC-point position - 1), top-level points of a seam call */
    uint32_t visited[PROBE_CONFIGS][GC_POSITIONS];
    uint32_t stepped[PROBE_CONFIGS][GC_POSITIONS];
    uint32_t calls[PROBE_CONFIGS];
    uint32_t beyond;        /* a position past GC_POSITIONS */
    uint64_t meaning;       /* every record except its image */
    uint64_t image;         /* every record's image */
    uint32_t kept;
    gc_record first[GC_KEEP];
} gc_transcript;

/* One GC transcript run (run_transcript). */
typedef struct transcript {
    gc_transcript gc;
    uint64_t fin;           /* finalizer order: (kind, object serial) */
    uint32_t fin_calls;
    uint64_t results;       /* every probe result */
    uint32_t result_count, result_bad;
    uint64_t heap;          /* heap_digest() before lua_close */
    uint32_t heap_objects;
    uint64_t arena;         /* arena bytes [0, high) before lua_close (informational) */
    uint32_t arena_high, arena_live_after_close, arena_failed;
    int32_t debt;
    uint32_t total;
    int32_t gcstate;
    uint32_t handled, fallbacks;
    uint32_t memo[8];       /* ISAAC_LGC_MEMO_COUNT deltas (direct path) */
} transcript;

/* What the Lua heap means, object by object: every collectable object on
 * allgc / finobj / tobefnz / fixedgc and every short string in the string
 * table (address, list link, tag, mark bits and the fields Lua reads), the
 * global state's collector fields, string cache and tag-method names, and the
 * thread's live stack.  Raw arena bytes are not a sound comparison: Lua copies
 * C-stack temporaries into the heap whole (lapi.c lua_rawsetp's `TValue k`
 * reaches a node key through luaH_set), so the half of value_ a light
 * userdata does not use and the padding after tt_ carry whatever the C stack
 * held there -- bytes no Lua operation reads.  Run in the deterministic arena,
 * equal allocation sequences give equal addresses, so pointers are compared
 * as values. */
static uint64_t fnv_ptr(uint64_t h, const void *pointer)
{
    return fnv_u32(h, (uint32_t)(uintptr_t)pointer);
}

static uint64_t digest_object(uint64_t h, const GCObject *o)
{
    int i;
    h = fnv_ptr(h, o);
    h = fnv_ptr(h, o->next);
    h = fnv_u32(h, o->tt);
    h = fnv_u32(h, o->marked);
    switch (o->tt) {
    case LUA_TSHRSTR:
    case LUA_TLNGSTR: {
        const TString *ts = gco2ts(o);
        size_t len = tsslen(ts);
        h = fnv_u32(h, ts->extra);
        h = fnv_u32(h, ts->hash);
        h = fnv_u32(h, (uint32_t)len);
        if (o->tt == LUA_TSHRSTR)
            h = fnv_ptr(h, ts->u.hnext);
        return fnv_bytes(h, getstr(ts), len);
    }
    case LUA_TTABLE: {
        const Table *t = gco2t(o);
        unsigned int k;
        h = fnv_u32(h, t->flags);
        h = fnv_u32(h, t->lsizenode);
        h = fnv_u32(h, t->sizearray);
        h = fnv_ptr(h, t->array);
        h = fnv_ptr(h, t->node);
        h = fnv_ptr(h, t->lastfree);
        h = fnv_ptr(h, t->metatable);
        h = fnv_ptr(h, t->gclist);
        for (k = 0U; k < t->sizearray; ++k)
            h = fnv_slot_meaning(h, &t->array[k]);
        for (i = 0; i < sizenode(t); ++i) {
            const Node *n = &t->node[i];
            h = fnv_slot_meaning(h, gkey(n));
            h = fnv_u32(h, (uint32_t)gnext(n));
            h = fnv_slot_meaning(h, gval(n));
        }
        return h;
    }
    case LUA_TUSERDATA: {
        const Udata *u = gco2u(o);
        TValue user;
        user.value_ = u->user_;
        settt_(&user, u->ttuv_);
        h = fnv_ptr(h, u->metatable);
        h = fnv_u32(h, (uint32_t)u->len);
        h = fnv_slot_meaning(h, &user);
        /* newud's serial; the rest of every block is never written */
        return u->len >= 4U ? fnv_bytes(h, getudatamem(u), 4U) : h;
    }
    case LUA_TLCL: {
        const LClosure *cl = gco2lcl(o);
        h = fnv_u32(h, cl->nupvalues);
        h = fnv_ptr(h, cl->gclist);
        h = fnv_ptr(h, cl->p);
        for (i = 0; i < cl->nupvalues; ++i) {
            const UpVal *uv = cl->upvals[i];
            h = fnv_ptr(h, uv);
            if (uv != NULL) {
                h = fnv_ptr(h, uv->v);
                h = fnv_u32(h, (uint32_t)uv->refcount);
                h = fnv_slot_meaning(h, uv->v);
            }
        }
        return h;
    }
    case LUA_TCCL: {
        const CClosure *cl = gco2ccl(o);
        h = fnv_u32(h, cl->nupvalues);
        h = fnv_ptr(h, cl->gclist);
        h = fnv_u32(h, (uint32_t)(uintptr_t)cl->f);
        for (i = 0; i < cl->nupvalues; ++i)
            h = fnv_slot_meaning(h, &cl->upvalue[i]);
        return h;
    }
    case LUA_TPROTO: {
        const Proto *p = gco2p(o);
        h = fnv_u32(h, p->numparams);
        h = fnv_u32(h, p->is_vararg);
        h = fnv_u32(h, p->maxstacksize);
        h = fnv_u32(h, (uint32_t)p->sizecode);
        h = fnv_bytes(h, p->code, sizeof(Instruction) * (size_t)p->sizecode);
        for (i = 0; i < p->sizek; ++i)
            h = fnv_slot_meaning(h, &p->k[i]);
        for (i = 0; i < p->sizep; ++i)
            h = fnv_ptr(h, p->p[i]);
        h = fnv_ptr(h, p->cache);
        h = fnv_ptr(h, p->source);
        return fnv_ptr(h, p->gclist);
    }
    default:
        return h;
    }
}

static uint64_t heap_digest(lua_State *L, uint32_t *objects)
{
    global_State *g = G(L);
    GCObject *lists[4];
    const TValue *slot;
    uint64_t h = FNV64_INIT;
    int i, j;
    *objects = 0U;
    lists[0] = g->allgc;
    lists[1] = g->finobj;
    lists[2] = g->tobefnz;
    lists[3] = g->fixedgc;
    for (i = 0; i < 4; ++i) {
        const GCObject *o;
        h = fnv_u32(h, 0xa11c0000U + (uint32_t)i);
        for (o = lists[i]; o != NULL; o = o->next) {
            h = digest_object(h, o);
            ++*objects;
        }
    }
    h = fnv_u32(h, (uint32_t)g->strt.size);
    h = fnv_u32(h, (uint32_t)g->strt.nuse);
    h = fnv_ptr(h, g->strt.hash);
    for (i = 0; i < g->strt.size; ++i) {
        const TString *ts;
        for (ts = g->strt.hash[i]; ts != NULL; ts = ts->u.hnext) {
            h = digest_object(h, obj2gco(ts));
            ++*objects;
        }
    }
    h = fnv_u32(h, (uint32_t)g->totalbytes);
    h = fnv_u32(h, (uint32_t)g->GCdebt);
    h = fnv_u32(h, (uint32_t)g->GCmemtrav);
    h = fnv_u32(h, (uint32_t)g->GCestimate);
    h = fnv_slot_meaning(h, &g->l_registry);
    h = fnv_u32(h, g->seed);
    h = fnv_u32(h, ((uint32_t)g->currentwhite << 24) | ((uint32_t)g->gcstate << 16) |
                   ((uint32_t)g->gckind << 8) | g->gcrunning);
    h = fnv_ptr(h, g->allgc);
    h = fnv_ptr(h, g->sweepgc);
    h = fnv_ptr(h, g->finobj);
    h = fnv_ptr(h, g->gray);
    h = fnv_ptr(h, g->grayagain);
    h = fnv_ptr(h, g->weak);
    h = fnv_ptr(h, g->ephemeron);
    h = fnv_ptr(h, g->allweak);
    h = fnv_ptr(h, g->tobefnz);
    h = fnv_ptr(h, g->fixedgc);
    h = fnv_ptr(h, g->twups);
    h = fnv_u32(h, g->gcfinnum);
    h = fnv_u32(h, (uint32_t)g->gcpause);
    h = fnv_u32(h, (uint32_t)g->gcstepmul);
    h = fnv_ptr(h, g->memerrmsg);
    for (i = 0; i < TM_N; ++i)
        h = fnv_ptr(h, g->tmname[i]);
    for (i = 0; i < LUA_NUMTAGS; ++i)
        h = fnv_ptr(h, g->mt[i]);
    for (i = 0; i < STRCACHE_N; ++i)
        for (j = 0; j < STRCACHE_M; ++j)
            h = fnv_ptr(h, g->strcache[i][j]);
    h = fnv_ptr(h, L->stack);
    h = fnv_u32(h, (uint32_t)(L->top - L->stack));
    h = fnv_u32(h, (uint32_t)L->stacksize);
    h = fnv_u32(h, L->nci);
    h = fnv_ptr(h, L->openupval);
    for (slot = L->stack; slot < L->top; ++slot)
        h = fnv_slot_meaning(h, slot);
    return h;
}

static gc_transcript *s_gc_record;       /* recording GC points (NULL: count only) */
static transcript *s_tr;                 /* recording finalizers and results */
static uint32_t s_gc_points_in_seam, s_gc_moves_in_seam;
/* GC-point positions (position transcripts; see luaC_step below) */
static uint32_t s_gc_force_position;     /* 0: step wherever the debt is positive */
static uint32_t s_gc_position;           /* top-level GC points seen in this seam call */
static uint32_t s_gc_step_depth;         /* inside isaac_oracle_luaC_step_real */
static uint32_t s_probe_config;          /* probe_set(k) */

int isaac_vita_startup_map_path(CPU *__restrict c, uint32_t guest_path,
                                char *native_path, uint32_t capacity)
{
    (void)native_path;
    (void)capacity;
    guest_fault(c, guest_path, "oracle has no path mapper");
    return 0;
}

void isaac_vita_log(const char *format, ...)
{
    va_list args;
    char line[768];
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (++s_log_lines <= 48U)
        printf("  log: %s\n", line);
}

/* --- guest dispatch ---------------------------------------------------------- */

static void guest_probe(CPU *__restrict c);
static void guest_gc(CPU *__restrict c);
static void guest_gc_raise(CPU *__restrict c);
static void guest_gc_exact(CPU *__restrict c);

void guest_call(CPU *__restrict c, uint32_t address)
{
    if (address - (uint32_t)IMPORT_TOKEN_BASE < ISAAC_VITA_LUA_IMPORT_COUNT) {
        if (!isaac_vita_lua_import_indexed(
                c, address - (uint32_t)IMPORT_TOKEN_BASE, NULL))
            guest_fault(c, address, "oracle Lua import index rejected");
        return;
    }
    if (address == GUEST_PROBE) { guest_probe(c); return; }
    if (address == GUEST_GC) { guest_gc(c); return; }
    if (address == GUEST_GC_RAISE) { guest_gc_raise(c); return; }
    if (address == GUEST_GC_EXACT) { guest_gc_exact(c); return; }
    guest_fault(c, address, "oracle guest_call target is not registered");
}

/* --- fixture ------------------------------------------------------------------ */

_Alignas(16) static uint32_t s_stack[STACK_WORDS];
static CPU s_cpu;
static lua_State *s_L;
static uint32_t s_state_word;

/* Reservations start on the 64 KiB allocation granularity; commit the page
 * the bodies address inside it. */
static int map_fixed(uint32_t va, uint32_t size)
{
    uint32_t base = va & ~0xffffU;
    void *reserved = VirtualAlloc((void *)(uintptr_t)base, (va - base) + size,
                                  MEM_RESERVE, PAGE_READWRITE);
    void *committed;
    if (reserved != (void *)(uintptr_t)base) {
        fprintf(stderr, "map_fixed(%08x): reserve at %08x got %p error=%lu\n",
                (unsigned)va, (unsigned)base, reserved, GetLastError());
        return 0;
    }
    committed = VirtualAlloc((void *)(uintptr_t)va, size, MEM_COMMIT, PAGE_READWRITE);
    if (committed != (void *)(uintptr_t)va) {
        fprintf(stderr, "map_fixed(%08x): commit got %p error=%lu\n", (unsigned)va,
                committed, GetLastError());
        return 0;
    }
    return 1;
}

static void bind_stack(CPU *c)
{
    c->stack_owner = c;
    c->stack_floor = (uint32_t)(uintptr_t)s_stack;
    c->stack_ceiling = c->stack_floor + sizeof s_stack;
    c->stack_low_water = c->stack_ceiling;
    c->esp = c->stack_ceiling - ESP_HEADROOM;
}

static void reset_cpu(CPU *c)
{
    memset(c, 0, sizeof *c);
    bind_stack(c);
    memset(s_stack, 0xcd, sizeof s_stack);
    c->eax = 0xa5a5a5a5U;
    c->ebx = 0x0b0b0b0bU;
    c->ebp = 0x0e0e0e0eU;
    c->esi = 0x51515151U;
    c->edi = 0xd1d1d1d1U;
}

static int invoke_import(CPU *c, uint32_t index, const uint32_t *arguments,
                         uint32_t count)
{
    uint32_t saved = c->esp;
    uint32_t i;
    for (i = count; i != 0U; --i)
        gpush(c, arguments[i - 1U]);
    gpush(c, IMPORT_RETURN + index);
    if (!isaac_vita_lua_import_indexed(c, index, NULL))
        return 0;
    if (c->fault)
        return 0;
    if (c->esp != saved - count * 4U)
        return 0;
    return guest_stack_adjust(c, count * 4U, index);
}

static int register_closure(CPU *c, uint32_t target, const char *name)
{
    uint32_t closure_arguments[] = { s_state_word, target, 0U };
    uint32_t global_arguments[] = { s_state_word, (uint32_t)(uintptr_t)name };
    if (!invoke_import(c, LUA_IMPORT_PUSHCLOSURE, closure_arguments, 3U))
        return 0;
    return invoke_import(c, LUA_IMPORT_SETGLOBAL, global_arguments, 2U);
}

static int oracle_panic(lua_State *L)
{
    const char *message = lua_tostring(L, -1);
    fprintf(stderr, "oracle Lua panic: %s\n", message ? message : "(none)");
    return 0;
}

/* Runs `code` (0 args) through the bridge's lua_pcallk import so guest
 * closures called from it find the active CPU, exactly like LuaEngine does. */
static int run_chunk(CPU *c, const char *code, int nresults)
{
    uint32_t arguments[] = { s_state_word, 0U, (uint32_t)nresults, 0U, 0U, 0U };
    int root = 0, status;
    /* the load allocates: a collector step here may reach a guest __gc
     * closure, which needs the active CPU (the game loads chunks through a
     * Lua import, i.e. under the bridge owner) */
    if (!isaac_vita_lua_seam_enter(c, &root))
        return 0;
    status = luaL_loadstring(s_L, code);
    isaac_vita_lua_seam_leave(c, root);
    if (status != LUA_OK) {
        fprintf(stderr, "chunk load: %s\n", lua_tostring(s_L, -1));
        return 0;
    }
    if (!invoke_import(c, LUA_IMPORT_PCALLK, arguments, 6U))
        return 0;
    if (c->eax != LUA_OK) {
        fprintf(stderr, "chunk pcall status %u: %s\n", (unsigned)c->eax,
                lua_tostring(s_L, -1));
        return 0;
    }
    return 1;
}

/* Finish every pending guest finalizer under a bridge owner (the game's GC
 * steps run under an import scope).  A raising finalizer ends one full
 * collection with LUA_ERRGCMM, so repeat until three consecutive
 * collections complete cleanly; native calls made outside any owner
 * afterwards cannot meet a guest __gc closure. */
static int native_collect(lua_State *L)
{
    lua_gc(L, LUA_GCCOLLECT, 0);
    return 0;
}

static int drain_finalizers(CPU *c)
{
    int root = 0, clean = 0, i;
    if (!isaac_vita_lua_seam_enter(c, &root))
        return 0;
    for (i = 0; i < 400 && clean < 3; ++i) {
        lua_pushcfunction(s_L, native_collect);
        if (lua_pcall(s_L, 0, 0, 0) == LUA_OK) {
            ++clean;
        } else {
            clean = 0;
            lua_pop(s_L, 1);
        }
    }
    isaac_vita_lua_seam_leave(c, root);
    return clean >= 3;
}

/* Class tables as the frozen Userdata binder builds them: identity boolean,
 * __type, optional __parent, optional __const variant (identity, "const X",
 * __parent = parent's const variant), registered under a light-userdata key. */
static int make_class(lua_State *L, const char *name, int parent_ref,
                      int const_parent_ref, uint32_t key, int want_const,
                      int *const_ref)
{
    lua_newtable(L);
    lua_pushboolean(L, 1);
    lua_rawsetp(L, -2, IDENTITY);
    lua_pushstring(L, name);
    lua_setfield(L, -2, "__type");
    if (parent_ref) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, parent_ref);
        lua_setfield(L, -2, "__parent");
    }
    if (want_const) {
        lua_newtable(L);
        lua_pushboolean(L, 1);
        lua_rawsetp(L, -2, IDENTITY);
        lua_pushfstring(L, "const %s", name);
        lua_setfield(L, -2, "__type");
        if (const_parent_ref) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, const_parent_ref);
            lua_setfield(L, -2, "__parent");
        }
        lua_pushvalue(L, -1);
        *const_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        lua_setfield(L, -2, "__const");
    }
    if (key) {
        lua_pushvalue(L, -1);
        lua_rawsetp(L, LUA_REGISTRYINDEX, (const void *)(uintptr_t)key);
    }
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

static void make_object(lua_State *L, int mt_ref, const char *name)
{
    lua_getglobal(L, "objs");
    lua_newuserdata(L, 16U);
    if (mt_ref) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, mt_ref);
        lua_setmetatable(L, -2);
    }
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
}

static uint32_t s_ud_serial;

/* newud(mt): a 200-byte userdata stamped with a creation serial (the
 * transcripts record finalizer order by it). */
static int native_newud(lua_State *L)
{
    uint32_t *block = (uint32_t *)lua_newuserdata(L, 200U);
    *block = ++s_ud_serial;
    lua_pushvalue(L, 1);
    lua_setmetatable(L, -2);
    return 1;
}

/* Positive GC debt: the next luaC_checkGC anywhere performs a collector step
 * (pristine Lua internals, oracle only). */
static int native_gc_debt(lua_State *L)
{
    luaE_setdebt(G(L), 1);
    return 0;
}

/* --- transcript natives ------------------------------------------------------- */

enum { FN_GETCLASS = 0, FN_GETEXACT = 1 };

static int s_probe_fn, s_probe_variant;
static uint32_t s_probe_key, s_probe_cbc;
static int32_t s_probe_index;

/* probe_set(k): the call the next probe(x) makes (list[k + 1] in the
 * transcript script is the matching object). */
static const struct { int fn; uint32_t key; uint32_t can_be_const; } s_probe_configs[] = {
    { FN_GETCLASS, KEY_A, 0U },   /* objs.A: exact */
    { FN_GETCLASS, KEY_A, 0U },   /* objs.B: one __parent hop */
    { FN_GETCLASS, KEY_A, 0U },   /* objs.C: two hops */
    { FN_GETCLASS, KEY_A, 1U },   /* objs.Ac: const object, class.__const */
    { FN_GETCLASS, KEY_A, 1U },   /* objs.Bc: const + one hop */
    { FN_GETEXACT, KEY_A, 0U },   /* objs.A: exact */
    { FN_GETEXACT, KEY_A, 0U },   /* objs.Ac: the class's const variant */
};

static int native_probe_set(lua_State *L)
{
    lua_Integer k = luaL_checkinteger(L, 1);
    if (k < 0 || k >= (lua_Integer)(sizeof s_probe_configs / sizeof s_probe_configs[0]))
        return luaL_error(L, "probe_set: no configuration %d", (int)k);
    s_probe_fn = s_probe_configs[k].fn;
    s_probe_key = s_probe_configs[k].key;
    s_probe_cbc = s_probe_configs[k].can_be_const;
    s_probe_index = 1;
    s_probe_config = (uint32_t)k;
    return 0;
}

/* mark(result, object): fold the probe result and check it is the object's block. */
static int native_mark(lua_State *L)
{
    void *got = lua_touserdata(L, 1);
    void *want = lua_touserdata(L, 2);
    if (s_tr != NULL) {
        s_tr->results = fnv_u32(s_tr->results, (uint32_t)(uintptr_t)got);
        ++s_tr->result_count;
        if (got != want || got == NULL)
            ++s_tr->result_bad;
    }
    return 0;
}

static void note_finalizer(uint32_t kind, const void *block)
{
    if (s_tr == NULL)
        return;
    s_tr->fin = fnv_u32(s_tr->fin, kind);
    s_tr->fin = fnv_u32(s_tr->fin, block ? *(const uint32_t *)block : 0U);
    ++s_tr->fin_calls;
}

/* gcid(u): a Lua __gc function's record of the object it finalizes. */
static int native_gcid(lua_State *L)
{
    note_finalizer(2U, lua_touserdata(L, 1));
    return 0;
}

/* junk(): one new light-userdata key in the registry and one in class A's
 * table (a __gc calls it, so both tables rehash inside collector steps the
 * seam's own string pushes run). */
#define JUNK_KEYS 0x71000000U
static uint32_t s_junk;

static int native_junk(lua_State *L)
{
    ++s_junk;
    lua_pushboolean(L, 1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, (const void *)(uintptr_t)(JUNK_KEYS + 4U * s_junk));
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, (const void *)(uintptr_t)KEY_A) == LUA_TTABLE) {
        lua_pushboolean(L, 1);
        lua_rawsetp(L, -2, (const void *)(uintptr_t)(JUNK_KEYS + 0x00400000U + 4U * s_junk));
    }
    lua_pop(L, 1);
    return 0;
}

static void push_object(lua_State *L, const char *name)
{
    if (strcmp(name, "table") == 0) {
        lua_newtable(L);
    } else if (strcmp(name, "string") == 0) {
        lua_pushstring(L, "not a userdata");
    } else if (strcmp(name, "light") == 0) {
        lua_pushlightuserdata(L, (void *)(uintptr_t)0x12340U);
    } else {
        lua_getglobal(L, "objs");
        lua_getfield(L, -1, name);
        lua_remove(L, -2);
    }
}

/* --- __real: the oracle's own ------------------------------------------------- */

enum { REAL_GENERATED = 0, REAL_STUB = 1 };
static int s_real_mode;
static uint32_t s_real_calls;
static uint32_t s_real_contract_failures;
static CPU s_entry_cpu;
static int s_entry_top;
static uint8_t s_entry_stack[sizeof s_stack];
static uint32_t s_entry_live;   /* bytes of the live guest stack [esp, ceiling) */
static int s_in_seam;
static int s_ref_G;

static void snapshot_entry(CPU *c)
{
    memcpy(&s_entry_cpu, c, sizeof *c);
    s_entry_top = lua_gettop((lua_State *)(uintptr_t)c->ecx);
    s_entry_live = c->stack_ceiling - c->esp;
    if (s_entry_live > sizeof s_entry_stack)
        s_entry_live = sizeof s_entry_stack;
    memcpy(s_entry_stack, (const void *)(uintptr_t)c->esp, s_entry_live);
}

/* A nested wrap activation (guest __gc -> getExact inside the outer seam)
 * takes its own entry snapshot; the outer one is parked here meanwhile. */
typedef struct entry_snapshot {
    CPU cpu;
    int top;
    uint32_t live;
    uint8_t stack[sizeof s_stack];
} entry_snapshot;
static entry_snapshot s_parked_entry;

static void snapshot_park(void)
{
    memcpy(&s_parked_entry.cpu, &s_entry_cpu, sizeof s_entry_cpu);
    s_parked_entry.top = s_entry_top;
    s_parked_entry.live = s_entry_live;
    memcpy(s_parked_entry.stack, s_entry_stack, s_entry_live);
}

static void snapshot_unpark(void)
{
    memcpy(&s_entry_cpu, &s_parked_entry.cpu, sizeof s_entry_cpu);
    s_entry_top = s_parked_entry.top;
    s_entry_live = s_parked_entry.live;
    memcpy(s_entry_stack, s_parked_entry.stack, s_entry_live);
}

/* The seam promises to hand the translated body an untouched CPU, the entry
 * Lua stack top and unchanged live guest stack words (below esp is dead: a
 * guest __gc closure reached from a GC step inside the seam builds its frames
 * there). */
static void check_real_contract(CPU *c)
{
    lua_State *L = (lua_State *)(uintptr_t)c->ecx;
    int live_same = c->esp == s_entry_cpu.esp &&
                    memcmp(s_entry_stack, (const void *)(uintptr_t)c->esp,
                           s_entry_live) == 0;
    if (c->eax != s_entry_cpu.eax || c->ecx != s_entry_cpu.ecx ||
        c->edx != s_entry_cpu.edx || c->ebx != s_entry_cpu.ebx ||
        c->esp != s_entry_cpu.esp || c->ebp != s_entry_cpu.ebp ||
        c->esi != s_entry_cpu.esi || c->edi != s_entry_cpu.edi ||
        c->fault != NULL || lua_gettop(L) != s_entry_top || !live_same) {
        ++s_real_contract_failures;
        fprintf(stderr, "__real entry contract broken: eax=%08x/%08x esp=%08x/%08x "
                "top=%d/%d live stack=%s\n", (unsigned)c->eax,
                (unsigned)s_entry_cpu.eax, (unsigned)c->esp,
                (unsigned)s_entry_cpu.esp, lua_gettop(L), s_entry_top,
                live_same ? "same" : "CHANGED");
    }
}

/* Stub mode: a raw `ret` (the unbound-stack case must not trip the checked
 * pop) with a sentinel result, nothing else. */
void __real_sub_003f8e30(CPU *__restrict c)
{
    ++s_real_calls;
    check_real_contract(c);
    if (s_real_mode == REAL_STUB) {
        c->eax = 0xdeadbeefU;
        c->esp += 4U;
        return;
    }
    sub_003f8e30(c);
}

void __real_sub_003f8c80(CPU *__restrict c)
{
    ++s_real_calls;
    check_real_contract(c);
    if (s_real_mode == REAL_STUB) {
        c->eax = 0xdeadbeefU;
        c->esp += 4U;
        return;
    }
    sub_003f8c80(c);
}

/* --- the entry variants ----------------------------------------------------------- */

/* TRANSLATED: the generated body.  WRAP: the --wrap seam as built.  API: the
 * seam's Lua C API path (isaac_vita_lua_get*_try, __real on a fallback) --
 * what the wrap runs without ISAAC_VITA_LUA_GETCLASS_DIRECT, so with the knob
 * it is the reference the direct path is compared against. */
enum { VARIANT_TRANSLATED = 0, VARIANT_WRAP = 1, VARIANT_API = 2 };

static void call_api(CPU *c, int fn)
{
#if defined(ISAAC_VITA_LUA_GETCLASS_DIRECT)
    if (fn == FN_GETCLASS) {
        if (!isaac_vita_lua_getclass_try(c))
            __real_sub_003f8e30(c);
    } else {
        if (!isaac_vita_lua_getexact_try(c))
            __real_sub_003f8c80(c);
    }
#else
    if (fn == FN_GETCLASS) __wrap_sub_003f8e30(c); else __wrap_sub_003f8c80(c);
#endif
}

static void call_variant(CPU *c, int fn, int variant)
{
    snapshot_entry(c);
    s_in_seam = 1;
    s_gc_position = 0U;
    if (variant == VARIANT_API) {
        call_api(c, fn);
    } else if (fn == FN_GETCLASS) {
        if (variant == VARIANT_WRAP) __wrap_sub_003f8e30(c); else sub_003f8e30(c);
    } else {
        if (variant == VARIANT_WRAP) __wrap_sub_003f8c80(c); else sub_003f8c80(c);
    }
    s_in_seam = 0;
}

/* Arguments as the translated callers push them: getClass(L=ecx, index=edx,
 * [classKey, canBeConst]) then the return word; getExact(L=ecx, [classKey]). */
static uint32_t push_call(CPU *c, int fn, uint32_t key, uint32_t can_be_const,
                          int32_t index)
{
    c->ecx = s_state_word;
    c->edx = (uint32_t)index;
    if (fn == FN_GETCLASS) {
        gpush(c, can_be_const);
        gpush(c, key);
        gpush(c, RET_GETCLASS);
        return 8U;
    }
    gpush(c, key);
    gpush(c, RET_GETEXACT);
    return 4U;
}

/* --- guest closures ------------------------------------------------------------- */

static uint32_t s_gc_calls, s_gc_in_seam;

static void guest_probe(CPU *__restrict c)
{
    uint32_t L = ld32(c->esp + 4U);
    uint32_t adjust;
    c->ecx = L;
    adjust = push_call(c, s_probe_fn, s_probe_key, s_probe_cbc, s_probe_index);
    if (s_gc_record != NULL && s_probe_config < PROBE_CONFIGS)
        ++s_gc_record->calls[s_probe_config];
    call_variant(c, s_probe_fn, s_probe_variant);
    if (c->fault)
        return;
    (void)guest_stack_adjust(c, adjust, 0U);
    lua_pushlightuserdata((lua_State *)(uintptr_t)L, (void *)(uintptr_t)c->eax);
    c->eax = 1U;
    (void)gpop(c);
}

static void guest_gc(CPU *__restrict c)
{
    ++s_gc_calls;
    if (s_in_seam)
        ++s_gc_in_seam;
    if (s_tr != NULL)
        note_finalizer(1U, lua_touserdata((lua_State *)(uintptr_t)ld32(c->esp + 4U), 1));
    c->eax = 0U;
    (void)gpop(c);
}

/* Hostile: a guest __gc closure that raises through the bridge's luaL_error
 * import (escape to the nested trampoline, native luaL_error).  GCTM catches
 * it and re-throws LUA_ERRGCMM out of the lua_pushstring that stepped the
 * collector: through the seam's native frames (wrap) or the import
 * handler's (translated). */
static uint32_t s_gc_raise_calls, s_gc_raise_in_seam;
static const char s_gc_raise_format[] = "%s";
static const char s_gc_raise_text[] = "boom";

static void guest_gc_raise(CPU *__restrict c)
{
    uint32_t L = ld32(c->esp + 4U);
    ++s_gc_raise_calls;
    if (s_in_seam)
        ++s_gc_raise_in_seam;
    /* luaL_error(L, "%s", "boom") exactly as a translated body pushes it */
    gpush(c, (uint32_t)(uintptr_t)s_gc_raise_text);
    gpush(c, (uint32_t)(uintptr_t)s_gc_raise_format);
    gpush(c, L);
    gpush(c, IMPORT_RETURN + LUA_IMPORT_LUAL_ERROR);
    (void)isaac_vita_lua_import_indexed(c, LUA_IMPORT_LUAL_ERROR, NULL);
    guest_fault(c, 0U, "luaL_error import returned to the __gc closure");
}

/* Hostile: the game's __gc handler chain calls getExact(L, classKey) on the
 * object being finalized -- a nested wrap activation inside the GC step that
 * the outer getClass seam's own lua_pushstring started (same CPU, same Lua
 * state, owner slot already held by the outer activation). */
static uint32_t s_gc_exact_calls, s_gc_exact_native, s_gc_exact_bad;

static void guest_gc_exact(CPU *__restrict c)
{
    lua_State *L = (lua_State *)(uintptr_t)ld32(c->esp + 4U);
    const isaac_vita_lua_getclass_stats *st = isaac_vita_lua_getclass_stats_get();
    uint32_t handled_before = st->handled[FN_GETEXACT];
    void *expect = lua_touserdata(L, 1);
    ++s_gc_exact_calls;
    snapshot_park();
    c->ecx = (uint32_t)(uintptr_t)L;
    gpush(c, KEY_G);
    gpush(c, RET_GETEXACT);
    snapshot_entry(c);
    __wrap_sub_003f8c80(c);
    snapshot_unpark();
    if (c->fault)
        return;
    (void)guest_stack_adjust(c, 4U, 0U);
    if (c->eax != (uint32_t)(uintptr_t)expect)
        ++s_gc_exact_bad;
    if (st->handled[FN_GETEXACT] == handled_before + 1U)
        ++s_gc_exact_native;
    c->eax = 0U;
    (void)gpop(c);
}

/* --- GC points --------------------------------------------------------------------- */

/* recomp/test_vita_lua_getclass.py compiles lgc.c with
 * -DluaC_step=isaac_oracle_luaC_step_real, so every other reference to
 * luaC_step -- luaC_checkGC in lapi.c, in the bridge and in the seam's direct
 * path -- lands here (the eboot wraps luaC_step the same way under
 * ISAAC_VITA_LUA_COST_PROFILE).  The record is the collector's input at the
 * GC point: the live stack, L->top, debt, total bytes, state. */
void isaac_oracle_luaC_step_real(lua_State *L);

void luaC_step(lua_State *L)
{
    gc_transcript *t = s_gc_record;
    const TValue *before = L->stack;
    gc_record r;
    uint32_t moved, position = 0U, deferred = 0U;
    memset(&r, 0, sizeof r);
    /* A GC point of the seam call itself (not one a finalizer reaches while a
     * collector step is already running: lgc.c GCTM stops the collector, the
     * nested luaC_step only resets the debt). */
    if (s_in_seam && s_gc_step_depth == 0U) {
        position = ++s_gc_position;
        /* position transcript: hold the positive debt back until point N.
         * Returning without a step leaves GCdebt > 0, so the call's next
         * luaC_checkGC lands here again -- or, when the call has fewer than
         * N points, the first one after it, outside the seam. */
        deferred = t != NULL && s_gc_force_position != 0U &&
                   position < s_gc_force_position;
    }
    if (t != NULL) {
        const CallInfo *ci;
        const TValue *o;
        r.seq = t->points + t->deferred;
        r.position = position;
        r.deferred = deferred;
        r.in_seam = (uint32_t)s_in_seam;
        r.gcstate = G(L)->gcstate;
        r.debt = (int32_t)G(L)->GCdebt;
        r.total = (uint32_t)gettotalbytes(G(L));
        r.top = (int32_t)(L->top - L->stack);
        r.stacksize = L->stacksize;
        for (ci = L->ci; ci != NULL; ci = ci->previous)
            ++r.ci_depth;
        r.live = FNV64_INIT;
        for (o = L->stack; o < L->top; ++o)
            r.live = fnv_slot_meaning(r.live, o);
        /* dead slots too, by meaning: their raw bytes carry residue
         * (ldo.c luaD_reallocstack sets only the tag of new slots) */
        r.image = FNV64_INIT;
        for (o = L->stack; o < L->stack + L->stacksize; ++o)
            r.image = fnv_slot_meaning(r.image, o);
    }
    if (!deferred) {
        ++s_gc_step_depth;
        isaac_oracle_luaC_step_real(L);   /* a raising __gc leaves by longjmp:
                                           * run_transcript resets the depth */
        --s_gc_step_depth;
    }
    moved = L->stack != before;
    if (s_in_seam && !deferred) {
        ++s_gc_points_in_seam;
        s_gc_moves_in_seam += moved;
    }
    if (t != NULL) {
        r.moved = moved;
        if (position != 0U && s_probe_config < PROBE_CONFIGS) {
            if (position <= GC_POSITIONS) {
                ++t->visited[s_probe_config][position - 1U];
                t->stepped[s_probe_config][position - 1U] += !deferred;
            } else {
                ++t->beyond;
            }
        }
        if (deferred) {
            ++t->deferred;
        } else {
            ++t->points;
            t->in_seam += r.in_seam != 0U;
            t->moved += moved;
            t->moved_in_seam += moved && r.in_seam;
        }
        t->meaning = fnv_bytes(t->meaning, &r, offsetof(gc_record, image));
        t->image = fnv_bytes(t->image, &r.image, sizeof r.image);
        if (t->kept < GC_KEEP)
            t->first[t->kept++] = r;
    }
}

/* --- direct comparison ------------------------------------------------------------ */

typedef struct outcome {
    uint32_t eax, esp, ecx, edx, ebx, ebp, esi, edi;
    const char *fault;
    int top;
    int types[8];
    const void *pointers[8];
    uint8_t above[ESP_HEADROOM + 16U];
} outcome;

static void collect_outcome(const CPU *c, outcome *out);

static void run_direct(int fn, int variant, const char *object,
                       const char *object2, uint32_t key,
                       uint32_t can_be_const, int32_t index, outcome *out)
{
    CPU *c = &s_cpu;
    lua_settop(s_L, 0);
    push_object(s_L, object);
    if (object2)
        push_object(s_L, object2);
    reset_cpu(c);
    (void)push_call(c, fn, key, can_be_const, index);
    call_variant(c, fn, variant);
    collect_outcome(c, out);
}

static void collect_outcome(const CPU *c, outcome *out)
{
    int i;
    memset(out, 0, sizeof *out);
    out->eax = c->eax; out->esp = c->esp; out->ecx = c->ecx; out->edx = c->edx;
    out->ebx = c->ebx; out->ebp = c->ebp; out->esi = c->esi; out->edi = c->edi;
    out->fault = c->fault;
    out->top = lua_gettop(s_L);
    for (i = 1; i <= out->top && i <= 8; ++i) {
        out->types[i - 1] = lua_type(s_L, i);
        out->pointers[i - 1] = lua_topointer(s_L, i);
    }
    if (c->stack_ceiling - c->esp <= sizeof out->above)
        memcpy(out->above, (const void *)(uintptr_t)c->esp,
               c->stack_ceiling - c->esp);
}

static int same_outcome(const outcome *a, const outcome *b)
{
    return a->eax == b->eax && a->esp == b->esp && a->ecx == b->ecx &&
           a->edx == b->edx && a->ebx == b->ebx && a->ebp == b->ebp &&
           a->esi == b->esi && a->edi == b->edi && a->fault == b->fault &&
           a->top == b->top &&
           memcmp(a->types, b->types, sizeof a->types) == 0 &&
           memcmp(a->pointers, b->pointers, sizeof a->pointers) == 0 &&
           memcmp(a->above, b->above, sizeof a->above) == 0;
}

typedef struct direct_case {
    const char *name;
    int fn;
    const char *object;
    uint32_t key;
    uint32_t can_be_const;
    int32_t index;
    int native;             /* 1 = the wrap must serve it natively */
    uint32_t reason;        /* fallback reason when native == 0 */
    uint32_t hops;
    uint32_t imports;
} direct_case;

static const direct_case s_direct_cases[] = {
    { "getClass exact A",              FN_GETCLASS, "A",   KEY_A,  0U, 1, 1, 0U, 0U, 14U },
    { "getClass derived B:A (1 hop)",  FN_GETCLASS, "B",   KEY_A,  0U, 1, 1, 0U, 1U, 21U },
    { "getClass derived C:B:A (2)",    FN_GETCLASS, "C",   KEY_A,  0U, 1, 1, 0U, 2U, 28U },
    { "getClass const A canBeConst=1", FN_GETCLASS, "Ac",  KEY_A,  1U, 1, 1, 0U, 0U, 19U },
    { "getClass const B:A cbc=1",      FN_GETCLASS, "Bc",  KEY_A,  1U, 1, 1, 0U, 1U, 26U },
    { "getClass mutable A cbc=1",      FN_GETCLASS, "A",   KEY_A,  1U, 1, 1, 0U, 0U, 14U },
    /* hostile: canBeConst is `cmp byte ptr [ebp+0xc], 0`; upper bytes are garbage */
    { "getClass const A cbc=0xffffff01", FN_GETCLASS, "Ac",  KEY_A,  0xffffff01U, 1, 1, 0U, 0U, 19U },
    { "getClass exact B",              FN_GETCLASS, "B",   KEY_B,  0U, 1, 1, 0U, 0U, 14U },
    { "getClass 20-deep chain (hops)", FN_GETCLASS, "P20", KEY_P0, 0U, 1, 0, ISAAC_LGC_REASON_HOPS, 0U, 0U },
    { "getExact exact A",              FN_GETEXACT, "A",   KEY_A,  0U, 1, 1, 0U, 0U, 10U },
    { "getExact const A",              FN_GETEXACT, "Ac",  KEY_A,  0U, 1, 1, 0U, 0U, 14U },
    { "getExact exact B",              FN_GETEXACT, "B",   KEY_B,  0U, 1, 1, 0U, 0U, 10U },
};

typedef struct pcall_case {
    const char *name;
    int fn;
    const char *object;     /* Lua expression */
    uint32_t key;
    uint32_t can_be_const;
    int32_t index;
    uint32_t reason;
    const char *expect;     /* substring of the error message */
} pcall_case;

static const pcall_case s_pcall_cases[] = {
    { "getClass table",             FN_GETCLASS, "{}",             KEY_A, 0U, 1,  ISAAC_LGC_REASON_USERDATA, "(A expected, got table)" },
    { "getClass string",            FN_GETCLASS, "'s'",            KEY_A, 0U, 1,  ISAAC_LGC_REASON_USERDATA, "(A expected, got string)" },
    { "getClass wrong class A as B", FN_GETCLASS, "objs.A",        KEY_B, 0U, 1,  ISAAC_LGC_REASON_PARENT,   "(B expected, got A)" },
    { "getClass const violation",   FN_GETCLASS, "objs.Ac",        KEY_A, 0U, 1,  ISAAC_LGC_REASON_CONST,    "(cannot be const)" },
    { "getClass const wrong class", FN_GETCLASS, "objs.Ac",        KEY_B, 1U, 1,  ISAAC_LGC_REASON_PARENT,   "expected, got const A)" },
    { "getClass foreign metatable", FN_GETCLASS, "objs.foreign",   KEY_A, 0U, 1,  ISAAC_LGC_REASON_IDENTITY, "(A expected, got userdata)" },
    { "getClass identity not bool", FN_GETCLASS, "objs.wrongid",   KEY_A, 0U, 1,  ISAAC_LGC_REASON_IDENTITY, "(A expected, got userdata)" },
    { "getClass negative index",    FN_GETCLASS, "objs.A",         KEY_A, 0U, -1, ISAAC_LGC_REASON_USERDATA, "expected, got" },
    /* hostile: the byte read of canBeConst (0x100 -> byte 0 -> const violation) */
    { "getClass const cbc=0x100",   FN_GETCLASS, "objs.Ac",        KEY_A, 0x100U, 1, ISAAC_LGC_REASON_CONST, "(cannot be const)" },
    { "getClass const B:A cbc=0x4300", FN_GETCLASS, "objs.Bc",       KEY_A, 0x4300U, 1, ISAAC_LGC_REASON_CONST, "(cannot be const)" },
    /* hostile: an index beyond the top is LUA_TNONE ("no value") */
    { "getClass index beyond top",  FN_GETCLASS, "objs.A",         KEY_A, 0U, 5,  ISAAC_LGC_REASON_USERDATA, "(A expected, got no value)" },
    { "getExact derived B as A",    FN_GETEXACT, "objs.B",         KEY_A, 0U, 1,  ISAAC_LGC_REASON_PARENT,   "(A expected, got B)" },
    { "getExact table",             FN_GETEXACT, "{}",             KEY_A, 0U, 1,  ISAAC_LGC_REASON_USERDATA, "(A expected, got table)" },
    { "getExact foreign",           FN_GETEXACT, "objs.foreign",   KEY_A, 0U, 1,  ISAAC_LGC_REASON_IDENTITY, "(A expected, got userdata)" },
};

typedef struct stub_case {
    const char *name;
    int fn;
    const char *object;
    uint32_t key;
    uint32_t can_be_const;
    uint32_t reason;
} stub_case;

/* Shapes where the translated body would run lua_rawget on a non-table: the
 * seam must hand over before any side effect; __real is the recording stub. */
static const stub_case s_stub_cases[] = {
    { "getClass userdata without metatable", FN_GETCLASS, "nomt",      KEY_A,     0U, ISAAC_LGC_REASON_METATABLE },
    { "getClass light userdata",             FN_GETCLASS, "light",     KEY_A,     0U, ISAAC_LGC_REASON_METATABLE },
    { "getClass unregistered class key",     FN_GETCLASS, "A",         KEY_UNREG, 0U, ISAAC_LGC_REASON_CLASS },
    { "getClass const object, class has no __const", FN_GETCLASS, "X", KEY_X,     1U, ISAAC_LGC_REASON_CONST_CLASS },
    { "getClass __parent is a number",       FN_GETCLASS, "badparent", KEY_A,     0U, ISAAC_LGC_REASON_PARENT_TYPE },
    { "getExact without metatable",          FN_GETEXACT, "nomt",      KEY_A,     0U, ISAAC_LGC_REASON_METATABLE },
    { "getExact unregistered class key",     FN_GETEXACT, "A",         KEY_UNREG, 0U, ISAAC_LGC_REASON_CLASS },
};

static int run_stub_case(const stub_case *k)
{
    CPU *c = &s_cpu;
    const isaac_vita_lua_getclass_stats *st = isaac_vita_lua_getclass_stats_get();
    uint32_t real_before = s_real_calls;
    uint32_t fallbacks_before = st->fallbacks[k->fn];
    uint32_t handled_before = st->handled[k->fn];
    uint32_t entry_esp;
    lua_settop(s_L, 0);
    push_object(s_L, k->object);
    reset_cpu(c);
    (void)push_call(c, k->fn, k->key, k->can_be_const, 1);
    entry_esp = c->esp;
    s_real_mode = REAL_STUB;
    call_variant(c, k->fn, VARIANT_WRAP);
    s_real_mode = REAL_GENERATED;
    CHECK(s_real_calls == real_before + 1U);
    CHECK(s_real_contract_failures == 0U);
    CHECK(c->eax == 0xdeadbeefU && c->esp == entry_esp + 4U && !c->fault);
    CHECK(st->fallbacks[k->fn] == fallbacks_before + 1U);
    CHECK(st->handled[k->fn] == handled_before);
    CHECK(st->last_reason == k->reason);
    CHECK(lua_gettop(s_L) == 1);
    printf("  stub  : %-44s reason=%u OK\n", k->name, (unsigned)k->reason);
    return 0;
}

static int run_direct_case(const direct_case *k)
{
    outcome translated, wrapped;
    const isaac_vita_lua_getclass_stats *st = isaac_vita_lua_getclass_stats_get();
    uint32_t handled_before = st->handled[k->fn];
    uint32_t fallbacks_before = st->fallbacks[k->fn];
    uint32_t hops_before = st->hops;
    uint32_t imports_before = st->imports_elided;
    uint32_t real_before = s_real_calls;
    uint32_t entry_esp;

    run_direct(k->fn, VARIANT_TRANSLATED, k->object, NULL, k->key,
               k->can_be_const, k->index, &translated);
    CHECK(translated.fault == NULL && s_fault_calls == 0U);
    run_direct(k->fn, VARIANT_WRAP, k->object, NULL, k->key, k->can_be_const,
               k->index, &wrapped);
    CHECK(wrapped.fault == NULL && s_fault_calls == 0U);
    CHECK(same_outcome(&translated, &wrapped));
    entry_esp = s_cpu.stack_ceiling - ESP_HEADROOM -
                (k->fn == FN_GETCLASS ? 12U : 8U);
    CHECK(wrapped.esp == entry_esp + 4U);              /* plain ret */
    CHECK(wrapped.eax == (uint32_t)(uintptr_t)lua_touserdata(s_L, 1));
    CHECK(wrapped.top == 1);                            /* stack-neutral */
    CHECK(wrapped.ebx == 0x0b0b0b0bU && wrapped.ebp == 0x0e0e0e0eU &&
          wrapped.esi == 0x51515151U && wrapped.edi == 0xd1d1d1d1U);
    CHECK(wrapped.ecx == s_state_word && wrapped.edx == (uint32_t)k->index);
    if (k->native) {
        CHECK(st->handled[k->fn] == handled_before + 1U);
        CHECK(st->fallbacks[k->fn] == fallbacks_before);
        CHECK(st->last_reason == ISAAC_LGC_HANDLED);
        CHECK(st->hops == hops_before + (k->fn == FN_GETCLASS ? k->hops : 0U));
        CHECK(st->imports_elided == imports_before + k->imports);
        CHECK(s_coverage[k->fn == FN_GETCLASS ? ISAAC_LGC_GETCLASS_COVERAGE
                                              : ISAAC_LGC_GETEXACT_COVERAGE] == 1U);
#if defined(ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY)
        CHECK(s_real_calls == real_before + 1U);        /* the verifier ran __real */
#else
        CHECK(s_real_calls == real_before);
#endif
    } else {
        CHECK(st->handled[k->fn] == handled_before);
        CHECK(st->fallbacks[k->fn] == fallbacks_before + 1U);
        CHECK(st->last_reason == k->reason);
        CHECK(s_real_calls == real_before + 1U);
        CHECK(s_real_contract_failures == 0U);
    }
    printf("  direct: %-36s eax=%08x esp=%08x top=%d %s\n", k->name,
           (unsigned)wrapped.eax, (unsigned)wrapped.esp, wrapped.top,
           k->native ? "NATIVE" : "fallback");
    return 0;
}

static int run_pcall_variant(const pcall_case *k, int variant, char *message,
                             size_t capacity, int *ok)
{
    char code[512];
    const char *text;
    lua_settop(s_L, 0);
    s_probe_fn = k->fn;
    s_probe_variant = variant;
    s_probe_key = k->key;
    s_probe_cbc = k->can_be_const;
    s_probe_index = k->index;
    s_in_seam = 0;
    snprintf(code, sizeof code,
             "local ok, m = pcall(probe, %s) return ok, tostring(m)", k->object);
    if (!run_chunk(&s_cpu, code, 2))
        return 1;
    *ok = lua_toboolean(s_L, -2);
    text = lua_tostring(s_L, -1);
    snprintf(message, capacity, "%s", text ? text : "(nil)");
    lua_settop(s_L, 0);
    return 0;
}

static int run_pcall_case(const pcall_case *k)
{
    const isaac_vita_lua_getclass_stats *st = isaac_vita_lua_getclass_stats_get();
    char translated[512], wrapped[512];
    int ok_translated, ok_wrapped;
    uint32_t fallbacks_before, handled_before, real_before;

    CHECK(run_pcall_variant(k, VARIANT_TRANSLATED, translated, sizeof translated,
                            &ok_translated) == 0);
    CHECK(!ok_translated);
#if defined(ISAAC_VITA_LUA_GETCLASS_DIRECT)
    {
        /* the API path (the wrap without the knob) raises the same error */
        char api[512];
        int ok_api;
        CHECK(run_pcall_variant(k, VARIANT_API, api, sizeof api, &ok_api) == 0);
        CHECK(!ok_api);
        if (strcmp(translated, api) != 0)
            printf("          api: \"%s\"\n", api);
        CHECK(strcmp(translated, api) == 0);
        CHECK(st->last_reason == k->reason);
    }
#endif
    fallbacks_before = st->fallbacks[k->fn];
    handled_before = st->handled[k->fn];
    real_before = s_real_calls;
    CHECK(run_pcall_variant(k, VARIANT_WRAP, wrapped, sizeof wrapped,
                            &ok_wrapped) == 0);
    CHECK(!ok_wrapped);
    printf("  pcall : %-30s reason=%-2u \"%s\"\n", k->name, (unsigned)k->reason, wrapped);
    if (strcmp(translated, wrapped) != 0)
        printf("          translated: \"%s\"\n", translated);
    CHECK(strcmp(translated, wrapped) == 0);
    CHECK(strstr(wrapped, k->expect) != NULL);
    CHECK(st->fallbacks[k->fn] == fallbacks_before + 1U);
    CHECK(st->handled[k->fn] == handled_before);
    CHECK(st->last_reason == k->reason);
    CHECK(s_real_calls == real_before + 1U);
    CHECK(s_real_contract_failures == 0U);
    CHECK(s_fault_calls == 0U);
    return 0;
}

/* A finalizer implemented as a guest closure runs from GC steps that the
 * seam's own lua_pushstring triggers; it needs the bridge owner. */
static int run_gc_stress(void)
{
    const isaac_vita_lua_getclass_stats *st = isaac_vita_lua_getclass_stats_get();
    uint32_t handled_before = st->handled[FN_GETCLASS];
    uint32_t fallbacks_before = st->fallbacks[FN_GETCLASS];
    lua_settop(s_L, 0);
    s_probe_fn = FN_GETCLASS;
    s_probe_variant = VARIANT_WRAP;
    s_probe_key = KEY_A;
    s_probe_cbc = 0U;
    s_probe_index = 1;
    s_gc_calls = 0U;
    s_gc_in_seam = 0U;
    /* Every iteration leaves finalizable garbage and a positive GC debt with
     * no GC check between it and the getClass call (a growing OP_SETTABLE
     * does that in the game; gc_debt() makes it happen on every iteration),
     * so the seam's own lua_pushstring is the checkpoint that steps the
     * collector and runs the guest __gc closure. */
    CHECK(run_chunk(&s_cpu,
        "local mt = { __gc = gc_probe }\n"
        "local a = objs.A\n"
        "local expect = tostring(probe(a))\n"
        "local t = {}\n"
        "for i = 1, 40000 do\n"
        "  local u = newud(mt)\n"
        "  t[i] = i\n"
        "  gc_debt()\n"
        "  if tostring(probe(a)) ~= expect then error('pointer drift at ' .. i) end\n"
        "end\n"
        "collectgarbage()\n"
        "return true", 1));
    CHECK(lua_toboolean(s_L, -1));
    lua_settop(s_L, 0);
    CHECK(st->handled[FN_GETCLASS] == handled_before + 40001U);
    CHECK(st->fallbacks[FN_GETCLASS] == fallbacks_before);
    CHECK(s_gc_calls >= 40000U);
    CHECK(s_gc_in_seam > 0U);
    CHECK(s_fault_calls == 0U);
    printf("  gc    : 40001 native getClass under allocation pressure; guest __gc "
           "calls=%u, of which inside the seam=%u\n",
           (unsigned)s_gc_calls, (unsigned)s_gc_in_seam);
    return 0;
}

static int run_stack_and_owner_cases(void)
{
    CPU *c = &s_cpu;
    CPU other;
    const isaac_vita_lua_getclass_stats *st = isaac_vita_lua_getclass_stats_get();
    uint32_t real_before = s_real_calls;
    uint32_t entry_esp;
    int root = 0;

    /* esp outside the bound stack: no Lua call, straight to the body */
    lua_settop(s_L, 0);
    push_object(s_L, "A");
    reset_cpu(c);
    (void)push_call(c, FN_GETCLASS, KEY_A, 0U, 1);
    c->stack_owner = NULL;
    entry_esp = c->esp;
    s_real_mode = REAL_STUB;
    call_variant(c, FN_GETCLASS, VARIANT_WRAP);
    s_real_mode = REAL_GENERATED;
    CHECK(s_real_calls == real_before + 1U && st->last_reason == ISAAC_LGC_REASON_STACK);
    CHECK(c->eax == 0xdeadbeefU && c->esp == entry_esp + 4U);
    c->stack_owner = c;
    printf("  stub  : %-44s reason=%u OK\n", "unbound guest stack", (unsigned)ISAAC_LGC_REASON_STACK);

    /* another CPU owns the Lua bridge */
    memset(&other, 0, sizeof other);
    CHECK(isaac_vita_lua_seam_enter(&other, &root) && root == 1);
    lua_settop(s_L, 0);
    push_object(s_L, "A");
    reset_cpu(c);
    (void)push_call(c, FN_GETCLASS, KEY_A, 0U, 1);
    entry_esp = c->esp;
    s_real_mode = REAL_STUB;
    call_variant(c, FN_GETCLASS, VARIANT_WRAP);
    s_real_mode = REAL_GENERATED;
    CHECK(s_real_calls == real_before + 2U && st->last_reason == ISAAC_LGC_REASON_OWNER);
    CHECK(c->eax == 0xdeadbeefU && c->esp == entry_esp + 4U);
    isaac_vita_lua_seam_leave(&other, root);
    CHECK(s_real_contract_failures == 0U && s_fault_calls == 0U);
    printf("  stub  : %-44s reason=%u OK\n", "bridge owned by another CPU", (unsigned)ISAAC_LGC_REASON_OWNER);

    /* the owner is released: a native call succeeds again */
    lua_settop(s_L, 0);
    push_object(s_L, "A");
    reset_cpu(c);
    (void)push_call(c, FN_GETCLASS, KEY_A, 0U, 1);
    call_variant(c, FN_GETCLASS, VARIANT_WRAP);
    CHECK(st->last_reason == ISAAC_LGC_HANDLED && c->eax == (uint32_t)(uintptr_t)lua_touserdata(s_L, 1));
    return 0;
}

/* --- hostile cases (review) -------------------------------------------------------- */

/* The relative-index quirk: with index -2 the bodies test the userdata at
 * depth entry+1 (the class table is already pushed) but return
 * lua_touserdata(index) at the entry depth -- a different slot.  The seam
 * must replicate that, not correct it. */
static int run_negative_index_quirk(void)
{
    outcome translated, wrapped;
    const isaac_vita_lua_getclass_stats *st = isaac_vita_lua_getclass_stats_get();
    uint32_t handled_before = st->handled[FN_GETCLASS];
    run_direct(FN_GETCLASS, VARIANT_TRANSLATED, "A", "B", KEY_B, 0U, -2, &translated);
    CHECK(translated.fault == NULL);
    run_direct(FN_GETCLASS, VARIANT_WRAP, "A", "B", KEY_B, 0U, -2, &wrapped);
    CHECK(wrapped.fault == NULL && s_fault_calls == 0U);
    CHECK(same_outcome(&translated, &wrapped));
    CHECK(wrapped.top == 2);
    CHECK(wrapped.eax == (uint32_t)(uintptr_t)lua_touserdata(s_L, 1));   /* A's block */
    CHECK(wrapped.eax != (uint32_t)(uintptr_t)lua_touserdata(s_L, 2));   /* not B's */
    CHECK(st->handled[FN_GETCLASS] == handled_before + 1U);
    CHECK(st->last_reason == ISAAC_LGC_HANDLED);
    printf("  quirk : %-36s eax=%08x (slot 1; class B checked on slot 2) NATIVE\n",
           "getClass index -2 of [A, B] as B", (unsigned)wrapped.eax);
    return 0;
}

/* A guest upvalue pseudo-index argument: getClass(L, lua_upvalueindex(1), ..)
 * from a guest closure whose guest upvalue #1 is the userdata.  Every index
 * the translated bodies pass crosses vita_lua_index (native upvalue #1 is
 * the bridge's target word); the seam's lgc_index must agree. */
static int run_upvalue_case(void)
{
    CPU *c = &s_cpu;
    const isaac_vita_lua_getclass_stats *st = isaac_vita_lua_getclass_stats_get();
    uint32_t closure_arguments[] = { s_state_word, GUEST_PROBE, 1U };
    uint32_t global_arguments[] = { s_state_word, (uint32_t)(uintptr_t)"probe_uv" };
    char translated[512], wrapped[512];
    int variant;

    lua_settop(s_L, 0);
    push_object(s_L, "A");                       /* guest upvalue #1 */
    reset_cpu(c);
    CHECK(invoke_import(c, LUA_IMPORT_PUSHCLOSURE, closure_arguments, 3U));
    CHECK(invoke_import(c, LUA_IMPORT_SETGLOBAL, global_arguments, 2U));
    CHECK(lua_gettop(s_L) == 0);
    for (variant = VARIANT_TRANSLATED; variant <= VARIANT_WRAP; ++variant) {
        char *message = variant == VARIANT_WRAP ? wrapped : translated;
        uint32_t handled_before = st->handled[FN_GETCLASS];
        uint32_t real_before = s_real_calls;
        const char *text;
        s_probe_fn = FN_GETCLASS;
        s_probe_variant = variant;
        s_probe_key = KEY_A;
        s_probe_cbc = 0U;
        s_probe_index = LUA_REGISTRYINDEX - 1;     /* guest lua_upvalueindex(1) */
        CHECK(run_chunk(c, "return tostring(probe_uv())", 1));
        text = lua_tostring(s_L, -1);
        snprintf(message, 512, "%s", text ? text : "(nil)");
        lua_settop(s_L, 0);
        if (variant == VARIANT_WRAP) {
            CHECK(st->handled[FN_GETCLASS] == handled_before + 1U);
            CHECK(st->last_reason == ISAAC_LGC_HANDLED);
#if !defined(ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY)
            CHECK(s_real_calls == real_before);
#else
            (void)real_before;
#endif
        }
    }
    CHECK(strcmp(translated, wrapped) == 0);
    push_object(s_L, "A");
    lua_pushlightuserdata(s_L, lua_touserdata(s_L, -1));
    CHECK(strcmp(luaL_tolstring(s_L, -1, NULL), wrapped) == 0);   /* objs.A's block */
    lua_settop(s_L, 0);
    printf("  upval : %-36s %s NATIVE\n", "getClass via guest lua_upvalueindex(1)", wrapped);
    /* the wrong class through the same pseudo-index: identical argerror text */
    for (variant = VARIANT_TRANSLATED; variant <= VARIANT_WRAP; ++variant) {
        char *message = variant == VARIANT_WRAP ? wrapped : translated;
        const char *text;
        s_probe_variant = variant;
        s_probe_key = KEY_B;
        CHECK(run_chunk(c, "local ok, m = pcall(probe_uv) return ok, tostring(m)", 2));
        CHECK(!lua_toboolean(s_L, -2));
        text = lua_tostring(s_L, -1);
        snprintf(message, 512, "%s", text ? text : "(nil)");
        lua_settop(s_L, 0);
    }
    CHECK(strcmp(translated, wrapped) == 0);
    CHECK(strstr(wrapped, "(B expected, got A)") != NULL);
    CHECK(st->last_reason == ISAAC_LGC_REASON_PARENT);
    printf("  upval : %-36s \"%s\"\n", "wrong class via upvalue", wrapped);
    CHECK(s_real_contract_failures == 0U && s_fault_calls == 0U);
    return 0;
}

/* The seam as the OUTERMOST bridge owner (a direct call with no import scope
 * active) while the collector step inside its own lua_pushstring finalizes
 * an object of class G whose guest __gc calls getExact: the nested wrap
 * must find the owner (same CPU, root=0), be served natively, and the outer
 * activation must still release the slot and return the right pointer. */
static int run_nested_gc_exact_case(void)
{
    CPU *c = &s_cpu;
    CPU other;
    const isaac_vita_lua_getclass_stats *st = isaac_vita_lua_getclass_stats_get();
    uint32_t i;
    int root = 0;
    s_gc_exact_calls = s_gc_exact_native = s_gc_exact_bad = 0U;
    for (i = 0U; i < 2000U && s_gc_exact_calls == 0U; ++i) {
        uint32_t handled_before;
        lua_settop(s_L, 0);
        push_object(s_L, "A");
        /* finalizable garbage of class G (__gc = gc_exact) */
        lua_newuserdata(s_L, 16U);
        lua_rawgeti(s_L, LUA_REGISTRYINDEX, s_ref_G);
        lua_setmetatable(s_L, -2);
        lua_pop(s_L, 1);
        luaE_setdebt(G(s_L), 1);
        reset_cpu(c);
        (void)push_call(c, FN_GETCLASS, KEY_A, 0U, 1);
        handled_before = st->handled[FN_GETCLASS];
        call_variant(c, FN_GETCLASS, VARIANT_WRAP);
        CHECK(!c->fault);
        CHECK(st->handled[FN_GETCLASS] == handled_before + 1U);
        CHECK(c->eax == (uint32_t)(uintptr_t)lua_touserdata(s_L, 1));
        CHECK(lua_gettop(s_L) == 1);
        /* the outermost activation released the owner slot */
        memset(&other, 0, sizeof other);
        CHECK(isaac_vita_lua_seam_enter(&other, &root) && root == 1);
        isaac_vita_lua_seam_leave(&other, root);
    }
    CHECK(s_gc_exact_calls > 0U);
    CHECK(drain_finalizers(c));            /* the loop's remaining G objects */
    CHECK(s_gc_exact_native == s_gc_exact_calls);
    CHECK(s_gc_exact_bad == 0U);
    CHECK(s_real_contract_failures == 0U && s_fault_calls == 0U);
    printf("  nested: %-36s __gc getExact calls=%u native=%u (outer root=1, %u iterations)\n",
           "guest __gc -> getExact inside the seam", (unsigned)s_gc_exact_calls,
           (unsigned)s_gc_exact_native, (unsigned)i);
    return 0;
}

/* LUA_ERRGCMM raised by a guest __gc from the collector step that the body's
 * own lua_pushstring performs: the longjmp crosses the seam's native frames
 * (wrap) or the import handler's (translated).  Lua's pcall must see the same
 * status and text, the bridge must recover the abandoned callback frame, the
 * owner slot must be free afterwards, and the next call must work. */
static int run_gc_error_variant(int variant, char *message, size_t capacity,
                                int *ok, int *raised_inside)
{
    const char *text;
    lua_settop(s_L, 0);
    s_probe_fn = FN_GETCLASS;
    s_probe_variant = variant;
    s_probe_key = KEY_A;
    s_probe_cbc = 0U;
    s_probe_index = 1;
    s_gc_raise_calls = 0U;
    s_gc_raise_in_seam = 0U;
    s_in_seam = 0;
    /* objs.B is derived: the body pushes "__const" and "__parent" (two GC
     * checkpoints); each iteration leaves one finalizable object behind and
     * a positive debt right before the call. */
    if (!run_chunk(&s_cpu,
        "local mt = { __gc = gc_raise }\n"
        "local b = objs.B\n"
        "for i = 1, 20000 do\n"
        "  local u = newud(mt)\n"
        "  gc_debt()\n"
        "  local ok, m = pcall(probe, b)\n"
        "  if not ok then return ok, tostring(m), i end\n"
        "end\n"
        "return true, 'no finalizer raised inside getClass in 20000 iterations', 0", 3))
        return 1;
    *ok = lua_toboolean(s_L, -3);
    text = lua_tostring(s_L, -2);
    snprintf(message, capacity, "%s", text ? text : "(nil)");
    *raised_inside = s_gc_raise_in_seam > 0U;
    s_in_seam = 0;                 /* the longjmp skipped call_variant's reset */
    lua_settop(s_L, 0);
    /* drain the remaining raising finalizers (each collection propagates at
     * most one LUA_ERRGCMM) so later native calls cannot meet one */
    if (!drain_finalizers(&s_cpu))
        return 1;
    return 0;
}

static int run_gc_error_case(void)
{
    const isaac_vita_lua_getclass_stats *st = isaac_vita_lua_getclass_stats_get();
    CPU other;
    char translated[512], wrapped[512];
    int ok_translated, ok_wrapped, inside_translated, inside_wrapped, root = 0;
    uint32_t calls_before, done_before, abandoned;

    CHECK(run_gc_error_variant(VARIANT_TRANSLATED, translated, sizeof translated,
                               &ok_translated, &inside_translated) == 0);
    CHECK(!ok_translated && inside_translated);
    calls_before = st->calls[FN_GETCLASS];
    done_before = st->handled[FN_GETCLASS] + st->fallbacks[FN_GETCLASS];
    CHECK(run_gc_error_variant(VARIANT_WRAP, wrapped, sizeof wrapped,
                               &ok_wrapped, &inside_wrapped) == 0);
    CHECK(!ok_wrapped && inside_wrapped);
    printf("  gcerr : %-36s \"%s\"\n", "guest __gc raises inside getClass", wrapped);
    if (strcmp(translated, wrapped) != 0)
        printf("          translated: \"%s\"\n", translated);
    CHECK(strcmp(translated, wrapped) == 0);
    CHECK(strstr(wrapped, "error in __gc metamethod (boom)") != NULL);
    /* the activation the longjmp abandoned is counted as a call but neither
     * handled nor fallen back (0 when VERIFY's re-run took the throw) */
    abandoned = (st->calls[FN_GETCLASS] - calls_before) -
                (st->handled[FN_GETCLASS] + st->fallbacks[FN_GETCLASS] - done_before);
    CHECK(abandoned <= 1U);
    CHECK(s_fault_calls == 0U && s_real_contract_failures == 0U);
    /* the owner slot is free and a plain native call works again */
    memset(&other, 0, sizeof other);
    CHECK(isaac_vita_lua_seam_enter(&other, &root) && root == 1);
    isaac_vita_lua_seam_leave(&other, root);
    lua_settop(s_L, 0);
    push_object(s_L, "A");
    reset_cpu(&s_cpu);
    (void)push_call(&s_cpu, FN_GETCLASS, KEY_A, 0U, 1);
    call_variant(&s_cpu, FN_GETCLASS, VARIANT_WRAP);
    CHECK(!s_cpu.fault && st->last_reason == ISAAC_LGC_HANDLED &&
          s_cpu.eax == (uint32_t)(uintptr_t)lua_touserdata(s_L, 1));
    lua_settop(s_L, 0);
    printf("  gcerr : abandoned wrap activations=%u; bridge recovered, owner free, next call native\n",
           (unsigned)abandoned);
    return 0;
}

static int setup_lua(void)
{
    CPU *c = &s_cpu;
    uint32_t openlibs_arguments[1];
    int ref_A, ref_Ac = 0, ref_B, ref_Bc = 0, ref_C, ref_Cc = 0, ref_X, ref_dummy;
    int ref_foreign, ref_wrongid, ref_badparent, previous, i;

    reset_cpu(c);
    CHECK(invoke_import(c, LUA_IMPORT_NEWSTATE_AUX, NULL, 0U));
    s_state_word = c->eax;
    s_L = (lua_State *)(uintptr_t)s_state_word;
    CHECK(s_L != NULL);
    (void)lua_atpanic(s_L, oracle_panic);
    openlibs_arguments[0] = s_state_word;
    CHECK(invoke_import(c, LUA_IMPORT_OPENLIBS, openlibs_arguments, 1U));
    CHECK(register_closure(c, GUEST_PROBE, "probe"));
    CHECK(register_closure(c, GUEST_GC, "gc_probe"));
    CHECK(register_closure(c, GUEST_GC_RAISE, "gc_raise"));
    CHECK(register_closure(c, GUEST_GC_EXACT, "gc_exact"));
    lua_pushcfunction(s_L, native_newud);
    lua_setglobal(s_L, "newud");
    lua_pushcfunction(s_L, native_gc_debt);
    lua_setglobal(s_L, "gc_debt");
    lua_pushcfunction(s_L, native_probe_set);
    lua_setglobal(s_L, "probe_set");
    lua_pushcfunction(s_L, native_mark);
    lua_setglobal(s_L, "mark");
    lua_pushcfunction(s_L, native_gcid);
    lua_setglobal(s_L, "gcid");
    lua_pushcfunction(s_L, native_junk);
    lua_setglobal(s_L, "junk");
    lua_newtable(s_L);
    lua_setglobal(s_L, "objs");

    ref_A = make_class(s_L, "A", 0, 0, KEY_A, 1, &ref_Ac);
    ref_B = make_class(s_L, "B", ref_A, ref_Ac, KEY_B, 1, &ref_Bc);
    ref_C = make_class(s_L, "C", ref_B, ref_Bc, KEY_C, 1, &ref_Cc);
    ref_X = make_class(s_L, "X", 0, 0, KEY_X, 0, &ref_dummy);
    /* G: a class whose __gc is the guest closure calling getExact */
    s_ref_G = make_class(s_L, "G", 0, 0, KEY_G, 0, &ref_dummy);
    lua_rawgeti(s_L, LUA_REGISTRYINDEX, s_ref_G);
    lua_getglobal(s_L, "gc_exact");
    lua_setfield(s_L, -2, "__gc");
    lua_pop(s_L, 1);
    /* foreign: a metatable without the identity key */
    lua_newtable(s_L);
    lua_pushstring(s_L, "F");
    lua_setfield(s_L, -2, "__type");
    ref_foreign = luaL_ref(s_L, LUA_REGISTRYINDEX);
    /* wrongid: identity present but not a boolean */
    lua_newtable(s_L);
    lua_pushinteger(s_L, 1);
    lua_rawsetp(s_L, -2, IDENTITY);
    lua_pushstring(s_L, "W");
    lua_setfield(s_L, -2, "__type");
    ref_wrongid = luaL_ref(s_L, LUA_REGISTRYINDEX);
    /* badparent: mutable (has __const) with a non-table __parent */
    lua_newtable(s_L);
    lua_pushboolean(s_L, 1);
    lua_rawsetp(s_L, -2, IDENTITY);
    lua_pushstring(s_L, "BP");
    lua_setfield(s_L, -2, "__type");
    lua_newtable(s_L);
    lua_setfield(s_L, -2, "__const");
    lua_pushinteger(s_L, 5);
    lua_setfield(s_L, -2, "__parent");
    ref_badparent = luaL_ref(s_L, LUA_REGISTRYINDEX);
    /* P0 <- P1 <- ... <- P20: deeper than the native hop bound */
    previous = 0;
    for (i = 0; i <= 20; ++i) {
        char name[8];
        int unused_const = 0;
        snprintf(name, sizeof name, "P%d", i);
        previous = make_class(s_L, name, previous, 0, i == 0 ? (uint32_t)KEY_P0 : 0U, 1,
                              &unused_const);
    }
    make_object(s_L, ref_A, "A");
    make_object(s_L, ref_Ac, "Ac");
    make_object(s_L, ref_B, "B");
    make_object(s_L, ref_Bc, "Bc");
    make_object(s_L, ref_C, "C");
    make_object(s_L, ref_X, "X");
    make_object(s_L, ref_foreign, "foreign");
    make_object(s_L, ref_wrongid, "wrongid");
    make_object(s_L, ref_badparent, "badparent");
    make_object(s_L, 0, "nomt");
    make_object(s_L, previous, "P20");
    (void)ref_C;
    (void)ref_Cc;
    CHECK(lua_gettop(s_L) == 0);
    return 0;
}

/* --- Lua-state exactness ----------------------------------------------------------- */

#if !defined(ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY)
/* (VERIFY re-runs the translated body after a native hit, so its pushes and
 * GC points happen twice; the state comparisons are for the shipped seams.) */

#define DEAD_SLOTS 24

typedef struct lua_image {
    l_mem debt;
    lu_mem total;
    int gcstate;
    ptrdiff_t top;
    int stacksize;
    const TValue *stack;
    TString *strcache[STRCACHE_N][STRCACHE_M];
    uint64_t live;          /* meaning of [stack, top) */
    uint64_t dead;          /* value_+tt_ bytes of [top, top + DEAD_SLOTS) */
} lua_image;

static void image_take(lua_State *L, lua_image *im)
{
    const TValue *o;
    memset(im, 0, sizeof *im);
    im->debt = G(L)->GCdebt;
    im->total = gettotalbytes(G(L));
    im->gcstate = G(L)->gcstate;
    im->top = L->top - L->stack;
    im->stacksize = L->stacksize;
    im->stack = L->stack;
    memcpy(im->strcache, G(L)->strcache, sizeof im->strcache);
    im->live = FNV64_INIT;
    for (o = L->stack; o < L->top; ++o)
        im->live = fnv_slot_meaning(im->live, o);
    im->dead = FNV64_INIT;
    for (o = L->top; o < L->top + DEAD_SLOTS; ++o)
        im->dead = fnv_bytes(im->dead, o, SLOT_BYTES);
}

/* One direct case three times -- translated body, API path, wrap -- each from
 * the same string cache, debt and dead-slot residue, the collector held off
 * by a negative debt.  Round 1 first evicts the two guest .rdata strings from
 * the string cache, so luaS_new takes its miss path (strcmp both entries,
 * shift, luaS_newlstr) instead of its hit path. */
static int run_exact_case(const direct_case *k, int round, uint32_t *dead_differs)
{
    static const int variants[3] = { VARIANT_TRANSLATED, VARIANT_API, VARIANT_WRAP };
    lua_State *L = s_L;
    global_State *g = G(L);
    TString *cache[STRCACHE_N][STRCACHE_M];
    TValue dead[DEAD_SLOTS];
    lua_image image[3];
    outcome result[3];
    const l_mem debt0 = -(l_mem)(32 << 20);
    lu_mem total0 = 0U;
    int v;

    for (v = 0; v < 3; ++v) {
        CPU *c = &s_cpu;
        lua_settop(L, 0);
        push_object(L, k->object);
        if (v == 0) {
            int i;
            if (round == 1) {
                const char *strings[2];
                strings[0] = (const char *)(uintptr_t)VA(ISAAC_LGC_STR_CONST_RVA);
                strings[1] = (const char *)(uintptr_t)VA(ISAAC_LGC_STR_PARENT_RVA);
                for (i = 0; i < 2; ++i) {
                    /* lstring.c luaS_new's bucket; memerrmsg is luaS_init's empty entry */
                    unsigned bucket = point2uint(strings[i]) % STRCACHE_N;
                    int j;
                    for (j = 0; j < STRCACHE_M; ++j)
                        g->strcache[bucket][j] = g->memerrmsg;
                }
            }
            memcpy(cache, g->strcache, sizeof cache);
            CHECK(L->stack_last - L->top >= DEAD_SLOTS);
            for (i = 0; i < DEAD_SLOTS; ++i) {
                memset(&dead[i], 0x60 + i, sizeof dead[i]);
                settt_(&dead[i], LUA_TNIL);
            }
            luaE_setdebt(g, debt0);
            total0 = gettotalbytes(g);
        } else {
            memcpy(g->strcache, cache, sizeof cache);
            luaE_setdebt(g, debt0);
            CHECK(gettotalbytes(g) == total0);
        }
        memcpy(L->top, dead, sizeof dead);
        reset_cpu(c);
        (void)push_call(c, k->fn, k->key, k->can_be_const, k->index);
        call_variant(c, k->fn, variants[v]);
        CHECK(!c->fault && s_fault_calls == 0U);
        collect_outcome(c, &result[v]);
        image_take(L, &image[v]);
    }
    CHECK(same_outcome(&result[0], &result[1]));
    CHECK(same_outcome(&result[1], &result[2]));
    for (v = 1; v < 3; ++v) {
        CHECK(image[v].debt == image[0].debt && image[v].total == image[0].total);
        CHECK(image[v].gcstate == image[0].gcstate);
        CHECK(image[v].top == image[0].top && image[v].stack == image[0].stack &&
              image[v].stacksize == image[0].stacksize);
        CHECK(memcmp(image[v].strcache, image[0].strcache, sizeof image[0].strcache) == 0);
        CHECK(image[v].live == image[0].live);
    }
    /* the wrap leaves exactly the API path's dead slots */
    CHECK(image[2].dead == image[1].dead);
    if (image[1].dead != image[0].dead)
        ++*dead_differs;
    printf("  exact : %-36s round=%d alloc=%d strcache/debt/live same, dead slots %s\n",
           k->name, round, (int)(image[0].total - total0),
           image[1].dead == image[0].dead ? "same" : "differ from translated (API == wrap)");
    lua_settop(L, 0);
    return 0;
}
#endif /* !ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY */

#if defined(ISAAC_VITA_LUA_GETCLASS_DIRECT)
#define KEY_TMP 0x98806878U

/* One getClass(object, key) through `variant`; `want` (or NULL) is the
 * expected memo counter delta, `got` receives it. */
static int memo_call(const char *name, int variant, const char *object, uint32_t key,
                     int stub, uint32_t reason, const uint32_t *want, uint32_t *got)
{
    CPU *c = &s_cpu;
    const isaac_vita_lua_getclass_stats *st = isaac_vita_lua_getclass_stats_get();
    uint32_t before[ISAAC_LGC_MEMO_COUNT], after[ISAAC_LGC_MEMO_COUNT];
    unsigned i;
    lua_settop(s_L, 0);
    push_object(s_L, object);
    reset_cpu(c);
    (void)push_call(c, FN_GETCLASS, key, 0U, 1);
    isaac_vita_lua_getclass_direct_memo(before);
    if (stub)
        s_real_mode = REAL_STUB;
    call_variant(c, FN_GETCLASS, variant);
    s_real_mode = REAL_GENERATED;
    isaac_vita_lua_getclass_direct_memo(after);
    CHECK(!c->fault && s_fault_calls == 0U && s_real_contract_failures == 0U);
    CHECK(st->last_reason == reason);
    if (reason == ISAAC_LGC_HANDLED)
        CHECK(c->eax == (uint32_t)(uintptr_t)lua_touserdata(s_L, 1));
    else
        CHECK(c->eax == 0xdeadbeefU);              /* the recording stub ran */
    for (i = 0U; i < ISAAC_LGC_MEMO_COUNT; ++i) {
        got[i] = after[i] - before[i];
        if (want != NULL && got[i] != want[i]) {
            printf("  memo  : %s: counter %u moved by %u, expected %u\n", name ? name : "?",
                   i, (unsigned)got[i], (unsigned)want[i]);
            return 1;
        }
    }
    if (name != NULL)
        printf("  memo  : %-46s class %u/%u/%u identity %u/%u/%u reason=%u\n", name,
               (unsigned)got[0], (unsigned)got[1], (unsigned)got[2], (unsigned)got[3],
               (unsigned)got[4], (unsigned)got[5], (unsigned)reason);
    lua_settop(s_L, 0);
    return 0;
}

static int run_memo_cases(void)
{
    static const uint32_t cold[ISAAC_LGC_MEMO_COUNT] = { 0U, 1U, 0U, 0U, 1U, 0U };
    static const uint32_t warm[ISAAC_LGC_MEMO_COUNT] = { 1U, 0U, 0U, 1U, 0U, 0U };
    static const uint32_t new_key[ISAAC_LGC_MEMO_COUNT] = { 0U, 1U, 0U, 1U, 0U, 0U };
    static const uint32_t class_hit[ISAAC_LGC_MEMO_COUNT] = { 1U, 0U, 0U, 0U, 0U, 0U };
    static const uint32_t none[ISAAC_LGC_MEMO_COUNT] = { 0U, 0U, 0U, 0U, 0U, 0U };
    uint32_t got[ISAAC_LGC_MEMO_COUNT], sum[ISAAC_LGC_MEMO_COUNT];
    uint32_t i, j;

    isaac_vita_lua_getclass_direct_memo_flush();
    CHECK(memo_call("cold: both lookups miss", VARIANT_WRAP, "A", KEY_A, 0,
                    ISAAC_LGC_HANDLED, cold, got) == 0);
    CHECK(memo_call("warm: both lookups hit", VARIANT_WRAP, "A", KEY_A, 0,
                    ISAAC_LGC_HANDLED, warm, got) == 0);
    /* The registry grows one key at a time with a lookup after each insert:
     * every rehash moves its node array (the new array is allocated while the
     * old one is still live, so they cannot overlap) and the next lookup must
     * find the memo node stale, then refill. */
    memset(sum, 0, sizeof sum);
    for (i = 0U; i < 600U; ++i) {
        lua_pushboolean(s_L, 1);
        lua_rawsetp(s_L, LUA_REGISTRYINDEX, (const void *)(uintptr_t)(JUNK_KEYS + 0x00800000U + 4U * i));
        CHECK(memo_call(NULL, VARIANT_WRAP, "A", KEY_A, 0, ISAAC_LGC_HANDLED, NULL, got) == 0);
        for (j = 0U; j < ISAAC_LGC_MEMO_COUNT; ++j)
            sum[j] += got[j];
    }
    CHECK(sum[ISAAC_LGC_MEMO_CLASS_STALE] > 0U && sum[ISAAC_LGC_MEMO_CLASS_MISS] == 0U &&
          sum[ISAAC_LGC_MEMO_CLASS_HIT] + sum[ISAAC_LGC_MEMO_CLASS_STALE] == 600U);
    CHECK(sum[ISAAC_LGC_MEMO_IDENTITY_HIT] == 600U);
    printf("  memo  : %-46s class %u/%u/%u identity %u/%u/%u\n",
           "600 registry inserts, a lookup after each", (unsigned)sum[0], (unsigned)sum[1],
           (unsigned)sum[2], (unsigned)sum[3], (unsigned)sum[4], (unsigned)sum[5]);
    /* the same for the identity lookup: class A's table (objs.A's metatable) */
    memset(sum, 0, sizeof sum);
    for (i = 0U; i < 64U; ++i) {
        CHECK(lua_rawgetp(s_L, LUA_REGISTRYINDEX, (const void *)(uintptr_t)KEY_A) == LUA_TTABLE);
        lua_pushboolean(s_L, 1);
        lua_rawsetp(s_L, -2, (const void *)(uintptr_t)(JUNK_KEYS + 0x00c00000U + 4U * i));
        lua_pop(s_L, 1);
        CHECK(memo_call(NULL, VARIANT_WRAP, "A", KEY_A, 0, ISAAC_LGC_HANDLED, NULL, got) == 0);
        for (j = 0U; j < ISAAC_LGC_MEMO_COUNT; ++j)
            sum[j] += got[j];
    }
    CHECK(sum[ISAAC_LGC_MEMO_IDENTITY_STALE] > 0U && sum[ISAAC_LGC_MEMO_IDENTITY_MISS] == 0U &&
          sum[ISAAC_LGC_MEMO_IDENTITY_HIT] + sum[ISAAC_LGC_MEMO_IDENTITY_STALE] == 64U);
    CHECK(sum[ISAAC_LGC_MEMO_CLASS_HIT] == 64U);
    printf("  memo  : %-46s class %u/%u/%u identity %u/%u/%u\n",
           "64 inserts into class A's table, a lookup after each", (unsigned)sum[0],
           (unsigned)sum[1], (unsigned)sum[2], (unsigned)sum[3], (unsigned)sum[4],
           (unsigned)sum[5]);
    /* A removed value: lua_rawsetp(nil) keeps the key in its node (ltable.c
     * luaH_set finds it), so the memo node is still valid and its value is
     * nil -- exactly what luaH_get returns: CLASS fallback on both paths. */
    CHECK(lua_rawgetp(s_L, LUA_REGISTRYINDEX, (const void *)(uintptr_t)KEY_A) == LUA_TTABLE);
    lua_rawsetp(s_L, LUA_REGISTRYINDEX, (const void *)(uintptr_t)KEY_TMP);
    CHECK(memo_call("registry[tmp] = class A: class miss", VARIANT_WRAP, "A", KEY_TMP, 0,
                    ISAAC_LGC_HANDLED, new_key, got) == 0);
    CHECK(memo_call("registry[tmp] warm", VARIANT_WRAP, "A", KEY_TMP, 0,
                    ISAAC_LGC_HANDLED, warm, got) == 0);
    lua_pushnil(s_L);
    lua_rawsetp(s_L, LUA_REGISTRYINDEX, (const void *)(uintptr_t)KEY_TMP);
    CHECK(memo_call("registry[tmp] = nil: memo hit reads nil", VARIANT_WRAP, "A", KEY_TMP, 1,
                    ISAAC_LGC_REASON_CLASS, class_hit, got) == 0);
    CHECK(memo_call("registry[tmp] = nil: API path agrees", VARIANT_API, "A", KEY_TMP, 1,
                    ISAAC_LGC_REASON_CLASS, none, got) == 0);
    return 0;
}
#endif /* ISAAC_VITA_LUA_GETCLASS_DIRECT */

#if !defined(ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY)
#define TRANSCRIPT_ITERATIONS 12000

/* Collector steps forced inside the seam (gc_debt right before every probe);
 * guest and Lua finalizers; a finalizer that recurses 150 Lua frames, so the
 * Lua stack grows (moves) inside a collector step the seam's own string push
 * runs, and later shrinks (moves) in an atomic phase; a finalizer that adds a
 * key to the registry and to class A's table (rehash: memo nodes go stale). */
static const char s_transcript_chunk[] =
    "local mt = { __gc = gc_probe }\n"
    "local deep\n"
    "deep = function(n) if n == 0 then return 0 end return 1 + deep(n - 1) end\n"
    "local mt_move = { __gc = function(u) gcid(u) deep(150) end }\n"
    "local mt_rehash = { __gc = function(u) gcid(u) junk() end }\n"
    "local list = { objs.A, objs.B, objs.C, objs.Ac, objs.Bc, objs.A, objs.Ac }\n"
    "for i = 1, 12000 do\n"
    "  newud(mt)\n"
    "  if i % 3 == 0 then newud(mt_move) end\n"
    "  if i % 4 == 0 then newud(mt_rehash) end\n"
    "  local k = i % 7\n"
    "  probe_set(k)\n"
    "  gc_debt()\n"
    "  local x = list[k + 1]\n"
    "  mark(probe(x), x)\n"
    "end\n"
    "return true";

static transcript s_transcripts[4];
static transcript s_position_transcripts[3];

/* GC points of one handled call per probe configuration (s_probe_configs,
 * list[k + 1] of the chunk), READ off the seam's direct path
 * (host_vita_lua_getclass.c isaac_vita_lua_getclass_direct_try /
 * _getexact_direct_try) and off the translated bodies' lua_pushstring sites:
 *   0 getClass A  as A        "__const" probe                              1
 *   1 getClass B  as A        "__const" probe, "__parent" hop              2
 *   2 getClass C  as A        "__const" probe, two "__parent" hops         3
 *   3 getClass Ac as A const  "__const" probe, class "__const"             2
 *   4 getClass Bc as A const  "__const" probe, class "__const", one hop    3
 *   5 getExact A  as A        the metatable is the class: no push          0
 *   6 getExact Ac as A        class "__const"                              1 */
static const uint32_t s_probe_gc_points[PROBE_CONFIGS] = { 1U, 2U, 3U, 2U, 3U, 0U, 1U };
#define GC_POSITION_MAX 3U      /* the largest entry above */

/* The script on a fresh state in the reset arena, every probe through
 * `variant`.  Called from one place, so the C stack address lstate.c's
 * makeseed mixes into the string-hash seed is the same in every run (the test
 * compiles Lua with a constant luai_makeseed). */
static int run_transcript(int variant, uint32_t force_position, transcript *out)
{
    const isaac_vita_lua_getclass_stats *st = isaac_vita_lua_getclass_stats_get();
    lua_State *saved_L = s_L;
    uint32_t saved_word = s_state_word;
    int saved_ref_G = s_ref_G;
    uint32_t handled_before = st->handled[0] + st->handled[1];
    uint32_t fallbacks_before = st->fallbacks[0] + st->fallbacks[1];
    uint32_t close_arguments[1];
#if defined(ISAAC_VITA_LUA_GETCLASS_DIRECT)
    uint32_t memo_before[ISAAC_LGC_MEMO_COUNT], memo_after[ISAAC_LGC_MEMO_COUNT];
    unsigned i;
#endif

    memset(out, 0, sizeof *out);
    out->gc.meaning = out->gc.image = out->fin = out->results = FNV64_INIT;
    CHECK(arena_reset());
#if defined(ISAAC_VITA_LUA_GETCLASS_DIRECT)
    isaac_vita_lua_getclass_direct_memo_flush();
    isaac_vita_lua_getclass_direct_memo(memo_before);
#endif
    s_ud_serial = 0U;
    s_junk = 0U;
    s_arena_on = 1;
    CHECK(setup_lua() == 0);
    CHECK(arena_owns(s_L));
    s_probe_variant = variant;
    s_tr = out;
    s_gc_step_depth = 0U;
    s_gc_force_position = force_position;
    s_gc_record = &out->gc;
    CHECK(run_chunk(&s_cpu, s_transcript_chunk, 1));
    CHECK(lua_toboolean(s_L, -1));
    lua_settop(s_L, 0);
    s_gc_record = NULL;
    s_gc_force_position = 0U;
    s_tr = NULL;
    out->debt = (int32_t)G(s_L)->GCdebt;
    out->total = (uint32_t)gettotalbytes(G(s_L));
    out->gcstate = G(s_L)->gcstate;
    out->arena_high = s_arena_high;
    out->heap = heap_digest(s_L, &out->heap_objects);
    out->arena = fnv_bytes(FNV64_INIT, s_arena, s_arena_high);
    close_arguments[0] = s_state_word;
    reset_cpu(&s_cpu);
    CHECK(invoke_import(&s_cpu, LUA_IMPORT_CLOSE, close_arguments, 1U));
    s_arena_on = 0;
    out->arena_live_after_close = s_arena_live;
    out->arena_failed = s_arena_failed;
    s_L = saved_L;
    s_state_word = saved_word;
    s_ref_G = saved_ref_G;
    out->handled = st->handled[0] + st->handled[1] - handled_before;
    out->fallbacks = st->fallbacks[0] + st->fallbacks[1] - fallbacks_before;
#if defined(ISAAC_VITA_LUA_GETCLASS_DIRECT)
    isaac_vita_lua_getclass_direct_memo(memo_after);
    for (i = 0U; i < ISAAC_LGC_MEMO_COUNT; ++i)
        out->memo[i] = memo_after[i] - memo_before[i];
#endif
    return 0;
}

static int same_meaning(const transcript *a, const transcript *b)
{
    return a->gc.points == b->gc.points && a->gc.in_seam == b->gc.in_seam &&
           a->gc.moved == b->gc.moved && a->gc.moved_in_seam == b->gc.moved_in_seam &&
           a->gc.deferred == b->gc.deferred && a->gc.beyond == b->gc.beyond &&
           memcmp(a->gc.visited, b->gc.visited, sizeof a->gc.visited) == 0 &&
           memcmp(a->gc.stepped, b->gc.stepped, sizeof a->gc.stepped) == 0 &&
           memcmp(a->gc.calls, b->gc.calls, sizeof a->gc.calls) == 0 &&
           a->gc.meaning == b->gc.meaning && a->fin == b->fin &&
           a->fin_calls == b->fin_calls && a->results == b->results &&
           a->result_count == b->result_count && a->result_bad == b->result_bad &&
           a->debt == b->debt && a->total == b->total && a->gcstate == b->gcstate;
}

/* + every GC point's whole stack by meaning (dead slots too), the heap digest and
 * the arena high-water mark (the same allocation sequence) */
static int same_state(const transcript *a, const transcript *b)
{
    return same_meaning(a, b) && a->gc.image == b->gc.image && a->heap == b->heap &&
           a->heap_objects == b->heap_objects && a->arena_high == b->arena_high;
}

static void print_gc_record(const char *tag, const gc_record *r)
{
    printf("      %-10s seq=%u seam=%u pos=%u deferred=%u state=%d debt=%d total=%u top=%d "
           "size=%d ci=%d moved=%u live=%016llx image=%016llx\n", tag, (unsigned)r->seq,
           (unsigned)r->in_seam, (unsigned)r->position, (unsigned)r->deferred,
           (int)r->gcstate, (int)r->debt, (unsigned)r->total,
           (int)r->top, (int)r->stacksize, (int)r->ci_depth, (unsigned)r->moved,
           (unsigned long long)r->live, (unsigned long long)r->image);
}

static void first_difference(const transcript *a, const char *an,
                             const transcript *b, const char *bn)
{
    uint32_t i, n = a->gc.kept < b->gc.kept ? a->gc.kept : b->gc.kept;
    for (i = 0U; i < n; ++i) {
        if (memcmp(&a->gc.first[i], &b->gc.first[i], sizeof a->gc.first[i]) != 0) {
            printf("    first GC point that differs: #%u\n", (unsigned)i);
            print_gc_record(an, &a->gc.first[i]);
            print_gc_record(bn, &b->gc.first[i]);
            return;
        }
    }
    for (i = 0U; i < PROBE_CONFIGS; ++i) {
        uint32_t p;
        for (p = 0U; p < GC_POSITIONS; ++p) {
            if (a->gc.visited[i][p] != b->gc.visited[i][p] ||
                a->gc.stepped[i][p] != b->gc.stepped[i][p])
                printf("    probe configuration %u, GC-point position %u: visited %u/%u "
                       "stepped %u/%u\n", (unsigned)i, (unsigned)(p + 1U),
                       (unsigned)a->gc.visited[i][p], (unsigned)b->gc.visited[i][p],
                       (unsigned)a->gc.stepped[i][p], (unsigned)b->gc.stepped[i][p]);
        }
    }
    printf("    the first %u GC points agree; points %u/%u fin %u/%u results %u/%u "
           "debt %d/%d total %u/%u\n", (unsigned)n, (unsigned)a->gc.points,
           (unsigned)b->gc.points, (unsigned)a->fin_calls, (unsigned)b->fin_calls,
           (unsigned)a->result_count, (unsigned)b->result_count, (int)a->debt, (int)b->debt,
           (unsigned)a->total, (unsigned)b->total);
}

static int run_transcripts(void)
{
#if defined(ISAAC_VITA_LUA_GETCLASS_DIRECT)
    static const char *const names[4] = { "translated", "api", "direct", "api-again" };
#else
    static const char *const names[4] = { "translated", "wrap", "wrap-again", "wrap-third" };
#endif
    static const int order[4] = { VARIANT_TRANSLATED, VARIANT_API, VARIANT_WRAP, VARIANT_API };
    const transcript *t = s_transcripts;
    int i;

    for (i = 0; i < 4; ++i) {
        const transcript *r = &s_transcripts[i];
        CHECK(run_transcript(order[i], 0U, &s_transcripts[i]) == 0);
        printf("  trans : %-10s points=%u in_seam=%u moved=%u moved_in_seam=%u fin=%u "
               "results=%u bad=%u handled=%u fallbacks=%u arena=%u KiB\n", names[i],
               (unsigned)r->gc.points, (unsigned)r->gc.in_seam, (unsigned)r->gc.moved,
               (unsigned)r->gc.moved_in_seam, (unsigned)r->fin_calls,
               (unsigned)r->result_count, (unsigned)r->result_bad, (unsigned)r->handled,
               (unsigned)r->fallbacks, (unsigned)(r->arena_high >> 10));
        printf("          meaning=%016llx image=%016llx heap=%016llx (%u objects) fin=%016llx "
               "results=%016llx arena bytes=%016llx\n", (unsigned long long)r->gc.meaning,
               (unsigned long long)r->gc.image, (unsigned long long)r->heap,
               (unsigned)r->heap_objects, (unsigned long long)r->fin,
               (unsigned long long)r->results, (unsigned long long)r->arena);
#if defined(ISAAC_VITA_LUA_GETCLASS_DIRECT)
        if (order[i] == VARIANT_WRAP)
            printf("          memo class %u/%u/%u identity %u/%u/%u\n",
                   (unsigned)r->memo[0], (unsigned)r->memo[1], (unsigned)r->memo[2],
                   (unsigned)r->memo[3], (unsigned)r->memo[4], (unsigned)r->memo[5]);
#endif
        CHECK(r->result_count == TRANSCRIPT_ITERATIONS && r->result_bad == 0U);
        CHECK(r->arena_live_after_close == 0U && r->arena_failed == 0U);
        CHECK(r->gc.in_seam > 0U && r->fin_calls > 0U);
        CHECK(s_fault_calls == 0U && s_real_contract_failures == 0U);
    }
    /* the harness itself is deterministic: the same path twice, same state */
    if (!same_state(&t[1], &t[3]))
        first_difference(&t[1], names[1], &t[3], names[3]);
    CHECK(same_state(&t[1], &t[3]));
    /* the shipped wrap and the API path: identical GC points (live stack,
     * whole stack by meaning, debt, total, state), finalizer order, results, heap */
    if (!same_state(&t[1], &t[2]))
        first_difference(&t[1], names[1], &t[2], names[2]);
    CHECK(same_state(&t[1], &t[2]));
    /* the translated body: same GC points by meaning, finalizers, results */
    if (!same_meaning(&t[0], &t[1]))
        first_difference(&t[0], names[0], &t[1], names[1]);
    CHECK(same_meaning(&t[0], &t[1]));
    /* ... and the same heap: every live object and the live stack by meaning
     * (dead slots are not compared: the translated body rotates where the API
     * path copies, see the exact cases) */
    CHECK(t[0].heap == t[1].heap && t[0].heap_objects == t[1].heap_objects);
    CHECK(t[2].gc.moved_in_seam > 0U && t[2].handled == TRANSCRIPT_ITERATIONS &&
          t[2].fallbacks == 0U);
#if defined(ISAAC_VITA_LUA_GETCLASS_DIRECT)
    for (i = 0; i < ISAAC_LGC_MEMO_COUNT; ++i)
        CHECK(t[2].memo[i] > 0U);
    printf("  trans : direct == api: every GC point (live stack, whole stack incl. dead slots, debt, total, "
           "state), finalizer order, results and the heap digest identical; arena bytes %s\n",
           t[1].arena == t[2].arena ? "same" : "differ (C-stack residue in copied temporaries)");
#else
    printf("  trans : wrap == wrap: every GC point, finalizer order, results and the heap digest "
           "identical (harness deterministic); arena bytes %s\n",
           t[1].arena == t[3].arena ? "same" : "differ (C-stack residue in copied temporaries)");
#endif
    printf("  trans : translated == %s by meaning (GC points, finalizers, results, debt); "
           "whole stack %s, heap digest %s\n", names[1],
           t[0].gc.image == t[1].gc.image ? "same" : "differ (dead slots)",
           "same");
    return 0;
}

/* The table every position transcript must show.  With the debt held back
 * until point N, a call of configuration k (n = s_probe_gc_points[k] points)
 * visits positions 1 .. min(n, N), each exactly once per call, and steps at
 * position N only (every visit there is a step; none before, and none after:
 * the step at N leaves the debt negative for the rest of the call).  N = 0 is
 * the classic transcript: gc_debt() arms the first point, so it must look
 * like N = 1. */
static int check_gc_positions(const char *name, const gc_transcript *g, uint32_t force)
{
    uint32_t n_force = force == 0U ? 1U : force;
    uint32_t k, p;
    CHECK(g->beyond == 0U);
    CHECK(force != 0U || g->deferred == 0U);
    for (k = 0U; k < PROBE_CONFIGS; ++k) {
        uint32_t points = s_probe_gc_points[k];
        uint32_t reach = points < n_force ? points : n_force;
        CHECK(g->calls[k] >= TRANSCRIPT_ITERATIONS / PROBE_CONFIGS);
        for (p = 0U; p < GC_POSITIONS; ++p) {
            uint32_t want_visits = p < reach ? g->calls[k] : 0U;
            uint32_t want_steps = p + 1U == n_force ? want_visits : 0U;
            if (g->visited[k][p] != want_visits || g->stepped[k][p] != want_steps)
                printf("  pos   : %s N=%u configuration %u position %u: visited %u "
                       "(want %u) stepped %u (want %u)\n", name, (unsigned)force,
                       (unsigned)k, (unsigned)(p + 1U), (unsigned)g->visited[k][p],
                       (unsigned)want_visits, (unsigned)g->stepped[k][p],
                       (unsigned)want_steps);
            CHECK(g->visited[k][p] == want_visits);
            CHECK(g->stepped[k][p] == want_steps);
        }
    }
    return 0;
}

/* A collector step at every GC-point position of a call in turn. */
static int run_position_transcripts(void)
{
#if defined(ISAAC_VITA_LUA_GETCLASS_DIRECT)
    static const char *const names[3] = { "translated", "api", "direct" };
#else
    static const char *const names[3] = { "translated", "wrap", "wrap-again" };
#endif
    static const int order[3] = { VARIANT_TRANSLATED, VARIANT_API, VARIANT_WRAP };
    const transcript *t = s_position_transcripts;
    uint32_t force, k;
    int i;

    for (i = 0; i < 4; ++i)
        CHECK(check_gc_positions("classic", &s_transcripts[i].gc, 0U) == 0);
    /* N = GC_POSITION_MAX + 1 proves there is no later point: nothing steps
     * inside the seam, every call leaves with the debt still positive. */
    for (force = 1U; force <= GC_POSITION_MAX + 1U; ++force) {
        uint32_t steps_here = 0U;
        for (i = 0; i < 3; ++i) {
            const transcript *r = &s_position_transcripts[i];
            CHECK(run_transcript(order[i], force, &s_position_transcripts[i]) == 0);
            printf("  pos   : N=%u %-10s points=%u in_seam=%u deferred=%u moved_in_seam=%u "
                   "fin=%u results=%u bad=%u handled=%u fallbacks=%u meaning=%016llx "
                   "image=%016llx heap=%016llx\n", (unsigned)force, names[i],
                   (unsigned)r->gc.points, (unsigned)r->gc.in_seam, (unsigned)r->gc.deferred,
                   (unsigned)r->gc.moved_in_seam, (unsigned)r->fin_calls,
                   (unsigned)r->result_count, (unsigned)r->result_bad, (unsigned)r->handled,
                   (unsigned)r->fallbacks, (unsigned long long)r->gc.meaning,
                   (unsigned long long)r->gc.image, (unsigned long long)r->heap);
            CHECK(r->result_count == TRANSCRIPT_ITERATIONS && r->result_bad == 0U);
            CHECK(r->arena_live_after_close == 0U && r->arena_failed == 0U);
            CHECK(r->fin_calls > 0U);
            CHECK(s_fault_calls == 0U && s_real_contract_failures == 0U);
            CHECK(check_gc_positions(names[i], &r->gc, force) == 0);
        }
        if (!same_state(&t[1], &t[2]))
            first_difference(&t[1], names[1], &t[2], names[2]);
        CHECK(same_state(&t[1], &t[2]));
        if (!same_meaning(&t[0], &t[1]))
            first_difference(&t[0], names[0], &t[1], names[1]);
        CHECK(same_meaning(&t[0], &t[1]));
        CHECK(t[0].heap == t[1].heap && t[0].heap_objects == t[1].heap_objects);
        CHECK(t[2].handled == TRANSCRIPT_ITERATIONS && t[2].fallbacks == 0U);
        for (k = 0U; k < PROBE_CONFIGS; ++k)
            steps_here += t[2].gc.stepped[k][force - 1U];
        if (force <= GC_POSITION_MAX) {
            CHECK(steps_here > 0U && t[2].gc.in_seam >= steps_here);
            CHECK(t[2].gc.moved_in_seam > 0U);
        } else {
            CHECK(steps_here == 0U && t[2].gc.in_seam == 0U);
        }
        printf("  pos   : N=%u %s == %s (every GC point incl. held-back ones, whole stack, heap) "
               "and translated == %s by meaning; steps at position %u inside the seam=%u "
               "[configurations 0..6: %u/%u/%u/%u/%u/%u/%u]\n", (unsigned)force, names[2],
               names[1], names[1], (unsigned)force, (unsigned)steps_here,
               (unsigned)t[2].gc.stepped[0][force - 1U], (unsigned)t[2].gc.stepped[1][force - 1U],
               (unsigned)t[2].gc.stepped[2][force - 1U], (unsigned)t[2].gc.stepped[3][force - 1U],
               (unsigned)t[2].gc.stepped[4][force - 1U], (unsigned)t[2].gc.stepped[5][force - 1U],
               (unsigned)t[2].gc.stepped[6][force - 1U]);
    }
    printf("  pos   : GC-point positions 1..%u each stepped in turn, position %u never "
           "reached: PASS\n", (unsigned)GC_POSITION_MAX, (unsigned)(GC_POSITION_MAX + 1U));
    return 0;
}
#endif /* !ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY */

int main(void)
{
    const isaac_vita_lua_getclass_stats *st;
    uint32_t i, expected_verify_runs, handled;
    CPU *c = &s_cpu;
    uint32_t close_arguments[1];

    setvbuf(stdout, NULL, _IONBF, 0);   /* a crash must not swallow the transcript */
    /* the PE pages the generated bodies address absolutely */
    CHECK(map_fixed(VA(LUA_IAT_PAGE_RVA), 0x1000U));
    CHECK(map_fixed(VA(RDATA_PAGE_RVA), 0x1000U));
    for (i = 0U; i < ISAAC_VITA_LUA_IMPORT_COUNT; ++i)
        st32(VA(LUA_IAT_FIRST_RVA) + 4U * i, (uint32_t)IMPORT_TOKEN_BASE + i);
    strcpy((char *)(uintptr_t)VA(ISAAC_LGC_STR_TYPE_RVA), "__type");
    strcpy((char *)(uintptr_t)VA(ISAAC_LGC_STR_FORMAT_RVA), "%s expected, got %s");
    strcpy((char *)(uintptr_t)VA(ISAAC_LGC_STR_CONST_RVA), "__const");
    strcpy((char *)(uintptr_t)VA(ISAAC_LGC_STR_CANNOT_CONST_RVA), "cannot be const");
    strcpy((char *)(uintptr_t)VA(ISAAC_LGC_STR_PARENT_RVA), "__parent");

    CHECK(setup_lua() == 0);
    st = isaac_vita_lua_getclass_stats_get();

    for (i = 0U; i < sizeof s_direct_cases / sizeof s_direct_cases[0]; ++i)
        CHECK(run_direct_case(&s_direct_cases[i]) == 0);
    for (i = 0U; i < sizeof s_pcall_cases / sizeof s_pcall_cases[0]; ++i)
        CHECK(run_pcall_case(&s_pcall_cases[i]) == 0);
    for (i = 0U; i < sizeof s_stub_cases / sizeof s_stub_cases[0]; ++i)
        CHECK(run_stub_case(&s_stub_cases[i]) == 0);
    CHECK(run_stack_and_owner_cases() == 0);
    CHECK(run_gc_stress() == 0);
    CHECK(run_negative_index_quirk() == 0);
    CHECK(run_upvalue_case() == 0);
    CHECK(run_nested_gc_exact_case() == 0);
    CHECK(run_gc_error_case() == 0);
#if !defined(ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY)
    {
        uint32_t dead_differs = 0U, cases = 0U;
        int round;
        for (round = 0; round < 2; ++round) {
            for (i = 0U; i < sizeof s_direct_cases / sizeof s_direct_cases[0]; ++i, ++cases)
                CHECK(run_exact_case(&s_direct_cases[i], round, &dead_differs) == 0);
        }
        printf("  exact : %u cases x 3 paths: string cache, debt, total bytes, collector state, "
               "live stack identical; dead slots API == wrap in all, translated differs in %u\n",
               (unsigned)cases, (unsigned)dead_differs);
    }
#endif
#if defined(ISAAC_VITA_LUA_GETCLASS_DIRECT)
    CHECK(run_memo_cases() == 0);
#endif
#if !defined(ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY)
    CHECK(run_transcripts() == 0);
    CHECK(run_position_transcripts() == 0);
#endif
    printf("gc points: inside a getClass/getExact call=%u, of which moved the Lua stack=%u\n",
           (unsigned)s_gc_points_in_seam, (unsigned)s_gc_moves_in_seam);
#if defined(ISAAC_VITA_LUA_GETCLASS_DIRECT)
    {
        uint32_t memo[ISAAC_LGC_MEMO_COUNT];
        isaac_vita_lua_getclass_direct_memo(memo);
        printf("memo coverage: class hit/miss/stale=%u/%u/%u identity hit/miss/stale=%u/%u/%u\n",
               (unsigned)memo[0], (unsigned)memo[1], (unsigned)memo[2], (unsigned)memo[3],
               (unsigned)memo[4], (unsigned)memo[5]);
        for (i = 0U; i < ISAAC_LGC_MEMO_COUNT; ++i)
            CHECK(memo[i] > 0U);
    }
#endif

    handled = st->handled[0] + st->handled[1];
#if defined(ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY)
    /* first 64 hits, then every 4096th (hit counter multiples in (64, handled]) */
    expected_verify_runs = handled < 64U ? handled : 64U + handled / 4096U;
    /* a verify re-run that took the __gc throw counted its hit but not a run */
    CHECK(st->verify_runs == expected_verify_runs ||
          st->verify_runs + 1U == expected_verify_runs);
    expected_verify_runs = st->verify_runs;
    CHECK(st->verify_mismatches == 0U);
#else
    expected_verify_runs = 0U;
    CHECK(st->verify_runs == 0U && st->verify_mismatches == 0U);
#endif
    CHECK(s_real_contract_failures == 0U);
    CHECK(s_fault_calls == 0U);
    isaac_vita_lua_getclass_stats_log();

    close_arguments[0] = s_state_word;
    reset_cpu(c);
    CHECK(invoke_import(c, LUA_IMPORT_CLOSE, close_arguments, 1U));
    printf("native getclass guest oracle: PASS; direct=%u pcall=%u stub=%u hostile=4 "
           "native=%u fallbacks=%u gc_in_seam=%u verify=%u/%u\n",
           (unsigned)(sizeof s_direct_cases / sizeof s_direct_cases[0]),
           (unsigned)(sizeof s_pcall_cases / sizeof s_pcall_cases[0]),
           (unsigned)(sizeof s_stub_cases / sizeof s_stub_cases[0] + 2U),
           (unsigned)handled, (unsigned)(st->fallbacks[0] + st->fallbacks[1]),
           (unsigned)s_gc_in_seam, (unsigned)st->verify_mismatches,
           (unsigned)expected_verify_runs);
    return 0;
}
