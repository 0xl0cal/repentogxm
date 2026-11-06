#ifndef ISAAC_HOST_VITA_HEAP_STAGE_H
#define ISAAC_HOST_VITA_HEAP_STAGE_H

/* ISAAC_VITA_HEAP_STAGE_PROFILE (profiling builds only): per-window counts of
 * the guest malloc/free import routes and 1-in-64 wall-clock spans of each
 * stage, printed as ph120.hc / ph120.hf / ph120.hm.  Per call of a stage:
 * us / (n - over) - (z.us / z.n), z = the empty clock bracket. */
#include <stdint.h>

enum {
    ISAAC_VITA_HEAP_STAGE_F_ALL,    /* whole free import */
    ISAAC_VITA_HEAP_STAGE_F_PRE,    /* terminal + OGG + ANM2 + texel checks */
    ISAAC_VITA_HEAP_STAGE_F_SLAB,   /* room-slab free route */
    ISAAC_VITA_HEAP_STAGE_F_OBS,    /* native Vorbis mirror observer */
    ISAAC_VITA_HEAP_STAGE_F_LOCK,   /* allocator spinlock acquire */
    ISAAC_VITA_HEAP_STAGE_F_LEDGER, /* ready check, ledger lookup, lease, remove */
    ISAAC_VITA_HEAP_STAGE_F_NATIVE, /* newlib free (its own LwMutex) */
    ISAAC_VITA_HEAP_STAGE_F_POST,   /* unlock + telemetry */
    ISAAC_VITA_HEAP_STAGE_M_ALL,    /* whole malloc import */
    ISAAC_VITA_HEAP_STAGE_M_PRE,    /* terminal, ANM2, texel owner + texel, slab size check */
    ISAAC_VITA_HEAP_STAGE_M_LOCK,
    ISAAC_VITA_HEAP_STAGE_M_RESERVE,/* ready check + ledger reserve */
    ISAAC_VITA_HEAP_STAGE_M_NATIVE, /* newlib malloc + overflow-domain check */
    ISAAC_VITA_HEAP_STAGE_M_LEDGER, /* ledger insert + floor accounting */
    ISAAC_VITA_HEAP_STAGE_M_POST,   /* unlock + telemetry */
    ISAAC_VITA_HEAP_STAGE_EMPTY,    /* empty bracket: the clock's own share */
    ISAAC_VITA_HEAP_STAGE_TIMERS
};

enum {
    ISAAC_VITA_HEAP_COUNT_FREE,
    ISAAC_VITA_HEAP_COUNT_FREE_NULL,
    ISAAC_VITA_HEAP_COUNT_FREE_OGG,
    ISAAC_VITA_HEAP_COUNT_FREE_ANM2,
    ISAAC_VITA_HEAP_COUNT_FREE_TEXEL,
    ISAAC_VITA_HEAP_COUNT_FREE_SLAB,
    ISAAC_VITA_HEAP_COUNT_FREE_SLAB_FAST, /* slab route skipped (fast reject) */
    ISAAC_VITA_HEAP_COUNT_FREE_POOL,      /* generic free of an overflow block */
    ISAAC_VITA_HEAP_COUNT_MALLOC,
    ISAAC_VITA_HEAP_COUNT_MALLOC_ANM2,
    ISAAC_VITA_HEAP_COUNT_MALLOC_TEXEL,
    ISAAC_VITA_HEAP_COUNT_MALLOC_SLAB,
    ISAAC_VITA_HEAP_COUNT_MALLOC_POOL,    /* native failed, overflow served */
    ISAAC_VITA_HEAP_COUNT_MALLOC_NULL,
    ISAAC_VITA_HEAP_COUNT_CALLOC,
    ISAAC_VITA_HEAP_COUNT_REALLOC,
    ISAAC_VITA_HEAP_COUNTS
};

typedef struct IsaacVitaHeapStageTimer {
    uint32_t n, us, over;
} IsaacVitaHeapStageTimer;

typedef struct IsaacVitaHeapStageCounters {
    uint32_t count[ISAAC_VITA_HEAP_COUNTS];
    IsaacVitaHeapStageTimer timer[ISAAC_VITA_HEAP_STAGE_TIMERS];
} IsaacVitaHeapStageCounters;

/* Copy and zero (out may be NULL: discard). */
void isaac_vita_heap_stage_take(IsaacVitaHeapStageCounters *out);

#if defined(ISAAC_VITA_HEAP_FREE_COST_PROFILE)
/* ISAAC_VITA_HEAP_FREE_COST_PROFILE (profiling builds only, on top of the
 * stage profile).  ph120.hg: the generic free's ledger span g split into
 * three 1-in-64 sub-spans taken on calls whose a and g spans are not timed,
 * and exact work counts of every generic free.  ph120.hv: the free
 * prefilter's decisions and the Vorbis free observer's live-slot census
 * (zeros without ISAAC_VITA_NATIVE_VORBIS_FREE_WATCH_MASK).  Per call of
 * a sub-span: us / (n - over) - z (z from ph120.hm). */
enum {
    ISAAC_VITA_HEAP_COST_K,     /* ledger lookup (find_slot) */
    ISAAC_VITA_HEAP_COST_E,     /* lease scan + overflow-domain check */
    ISAAC_VITA_HEAP_COST_R,     /* remove: preflight scan + backshift */
    ISAAC_VITA_HEAP_COST_TIMERS
};
/* nv[]: observer calls, sum and max of live-mask bits per call, then calls
 * with 0, 1, 2 and 3+ live bits.  Zeros without the Vorbis live mask. */
#define ISAAC_VITA_HEAP_NV_CENSUS 7

typedef struct IsaacVitaHeapFreeCost {
    IsaacVitaHeapStageTimer timer[ISAAC_VITA_HEAP_COST_TIMERS];
    uint32_t lookups;           /* generic frees that found their entry */
    uint32_t lookup_probes;     /* find_slot probes over those lookups */
    uint32_t lookup_probe_max;
    uint32_t removes;           /* entries removed by a generic free */
    uint32_t remove_scan;       /* preflight entries scanned to the hole */
    uint32_t remove_moves;      /* entries shifted back by the commit */
    uint32_t remove_scan_max;
    uint32_t lease_scans;       /* lease checks with a non-empty lease set */
    uint32_t ledger_live;       /* ledger entries at the take */
    uint32_t ledger_capacity;
    uint32_t prefilter_skip;    /* frees outside every fixed block */
    uint32_t prefilter_maybe;   /* frees handed to the block routes */
    uint32_t nv[ISAAC_VITA_HEAP_NV_CENSUS];
} IsaacVitaHeapFreeCost;

/* Copy and zero (out may be NULL: discard). */
void isaac_vita_heap_free_cost_take(IsaacVitaHeapFreeCost *out);
#endif

#endif
