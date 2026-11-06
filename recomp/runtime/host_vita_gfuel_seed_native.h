/* Verdict counters of host_vita_gfuel_seed_native.c (ph120.gf). */
#ifndef HOST_VITA_GFUEL_SEED_NATIVE_H
#define HOST_VITA_GFUEL_SEED_NATIVE_H

#include <stdint.h>

/* Decline reasons, in the order of the ph120.gf d(...) legend. */
enum {
    ISAAC_VITA_GFUEL_DECLINE_STACK = 0,  /* s: guest stack cannot hold the frames */
    ISAAC_VITA_GFUEL_DECLINE_NULL,       /* n: null Game, head, root, child or player */
    ISAAC_VITA_GFUEL_DECLINE_WALK,       /* w: set walk longer than the bound */
    ISAAC_VITA_GFUEL_DECLINE_BYTE,       /* b: sub_0039cb80 with a G FUEL byte above 7 */
    ISAAC_VITA_GFUEL_DECLINE_REASONS
};

/* Profiling builds (ISAAC_VITA_GFUEL_SEED_NATIVE_PROFILE): sampled wall-clock
 * timers, ph120.gu us(q,m,o,h,z).  Every 64th entry of a wrapper is timed
 * from entry to exit on sceKernelGetProcessTimeWide (1 us steps; the mean of
 * the quantised spans is unbiased); every 64th at residue 32 times an empty
 * bracket instead, so z is the clock's own share of each span. */
enum {
    ISAAC_VITA_GFUEL_TIMER_QUERY = 0,  /* q: __wrap_sub_0039cb10, whole wrapper */
    ISAAC_VITA_GFUEL_TIMER_MODE,       /* m: __wrap_sub_0039cb80, whole wrapper */
    ISAAC_VITA_GFUEL_TIMER_LOOKUP,     /* o: translated sub_000206b0, other callers */
    ISAAC_VITA_GFUEL_TIMER_HAS,        /* h: HasCollectible sub_0033af30, inclusive,
                                          entries not inside another HasCollectible */
    ISAAC_VITA_GFUEL_TIMER_EMPTY,      /* z: two back-to-back clock reads */
/* The ph120.gn/gb members exist only where they are filled (the profiling
 * seams) or printed (the phase profiler, which every build compiling it
 * defines); a GFUEL_SEED_NATIVE build without the profiler keeps the
 * pre-gn layout, so its object stays identical. */
#if defined(ISAAC_VITA_GFUEL_SEED_NATIVE_PROFILE) || defined(ISAAC_VITA_PHASE_PROFILE)
    ISAAC_VITA_GFUEL_TIMER_GTM,        /* ph120.gn us(t): GetTrinketMultiplier
                                          sub_0038d4c0 outside any HasCollectible,
                                          inclusive */
#endif
    ISAAC_VITA_GFUEL_TIMERS
};
#define ISAAC_VITA_GFUEL_TOP_SITES 10

typedef struct IsaacVitaGfuelTimer {
    uint32_t n;      /* timed spans */
    uint32_t us;     /* their sum, outliers left out */
    uint32_t over;   /* spans of 100 us or more (preemption), counted in n only */
} IsaacVitaGfuelTimer;

typedef struct IsaacVitaGfuelSite {
    uint32_t ret;    /* return word the call site pushed */
    uint32_t n;      /* entries; 0 = unused */
} IsaacVitaGfuelSite;

typedef struct IsaacVitaGfuelSites {
    uint32_t distinct;   /* return words tabled this window */
    uint32_t overflow;   /* entries not tabled (probe limit reached) */
    IsaacVitaGfuelSite top[ISAAC_VITA_GFUEL_TOP_SITES];   /* by n, descending */
} IsaacVitaGfuelSites;

typedef struct IsaacVitaGfuelSeedCounters {
    uint32_t query_calls;    /* q(n): __wrap_sub_0039cb10 entries */
    uint32_t query_handled;  /* q(h): answered natively */
    uint32_t mode_calls;     /* m(n): __wrap_sub_0039cb80 entries */
    uint32_t mode_handled;   /* m(h): answered natively */
    uint32_t walked;         /* w: handled queries whose seed set was not empty */
    uint32_t present;        /* p: handled queries that found SEED_G_FUEL */
    uint32_t declined[ISAAC_VITA_GFUEL_DECLINE_REASONS];
    /* l(k,t,o): k = 1 when the profiling seams are linked
     * (--wrap=sub_000206b0 and --wrap=sub_0033af30, profiling builds); t =
     * entries into the translated sub_000206b0 whose return word is 0x39cb3c
     * (the translated sub_0039cb10); o = its other entries from other units
     * (the one same-unit call site is not seen). */
    uint32_t lookup_linked;
    uint32_t lookup_translated;
    uint32_t lookup_other;
    /* ph120.gu h(n,i,g): HasCollectible entries from other units and the
     * dispatch table (its 120 call sites inside guest_0092.c are not seen),
     * of which ignoreModifiers set / collectible id negative. */
    uint32_t has_calls;
    uint32_t has_ignore;
    uint32_t has_negative;
    IsaacVitaGfuelTimer timer[ISAAC_VITA_GFUEL_TIMERS];
    IsaacVitaGfuelSites lookup_sites;   /* ph120.gl: the o entries by call site */
    IsaacVitaGfuelSites has_sites;      /* ph120.gh: the h(n) entries by call site */
#if defined(ISAAC_VITA_GFUEL_SEED_NATIVE_PROFILE) || defined(ISAAC_VITA_PHASE_PROFILE)
    /* ph120.gn x = h(n) entries inside another HasCollectible (not timed;
     * h - x are the top-level entries the h timer samples); b =
     * HasCollectible bodies, whatever entered them (each body's first call,
     * 0033af4e call 0x2fe5e0); t(n,h,p) = GetTrinketMultiplier entries from
     * other units / from HasCollectible (0x33b006, 0x33b0cb) / outside any
     * HasCollectible (the ones its timer samples). */
    uint32_t has_nested;
    uint32_t has_bodies;
    uint32_t gtm_calls;
    uint32_t gtm_from_has;
    uint32_t gtm_top;
    IsaacVitaGfuelSites body_sites;     /* ph120.gb: the b bodies by HasCollectible's return word */
#endif
} IsaacVitaGfuelSeedCounters;

/* Take-and-zero for the phase profiler (ph120.gf); NULL discards. */
void isaac_vita_gfuel_seed_take(IsaacVitaGfuelSeedCounters *out);

/* The body behind the profiling HasCollectible wrapper: with
 * ISAAC_VITA_HASCOLLECTIBLE_NATIVE the native HasCollectible (its TU then
 * leaves __wrap_sub_0033af30 to this one, ISAAC_VITA_HASCOLL_NATIVE_CHAINED),
 * else the translated body.  Defined last so that no line above moves. */
#if defined(ISAAC_VITA_HASCOLLECTIBLE_NATIVE)
# include "host_vita_hascoll_native.h"
# define ISAAC_VITA_GFUEL_HAS_BODY(c) isaac_vita_hascoll_native_call(c)
#else
# define ISAAC_VITA_GFUEL_HAS_BODY(c) __real_sub_0033af30(c)
#endif

#endif
