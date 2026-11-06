/* Verdict counters of host_vita_hascoll_native.c (ph120.hn). */
#ifndef HOST_VITA_HASCOLL_NATIVE_H
#define HOST_VITA_HASCOLL_NATIVE_H

#include <stdint.h>

struct CPU;

/* Decline reasons, in the order of the ph120.hn d(...) legend.  Every
 * decline hands the untouched CPU to the translated body (__real_). */
enum {
    ISAAC_VITA_HASCOLL_DECLINE_STACK = 0, /* s: guest stack cannot hold the frames */
    ISAAC_VITA_HASCOLL_DECLINE_COOP,      /* c: co-op baby ([player+0x2c] == 1) */
    ISAAC_VITA_HASCOLL_DECLINE_ID,        /* i: negative id or 0x147/0x148/0x139/9/0x3b */
    ISAAC_VITA_HASCOLL_DECLINE_GFUEL,     /* g: G FUEL byte above 7, null link or long walk */
    ISAAC_VITA_HASCOLL_DECLINE_TWIN,      /* t: the [player+0x1d84] HasCollectible call */
    ISAAC_VITA_HASCOLL_DECLINE_NULL,      /* n: a null pointer the body would dereference */
    ISAAC_VITA_HASCOLL_DECLINE_WALK,      /* w: map walk or effect list over the bound */
    ISAAC_VITA_HASCOLL_DECLINE_TABLE,     /* x: item predicate jump target not in the frozen pair */
    ISAAC_VITA_HASCOLL_DECLINE_REASONS
};

typedef struct IsaacVitaHasCollCounters {
    uint32_t calls;     /* n: entries (the --wrap seam, or the G FUEL profile wrapper) */
    uint32_t handled;   /* h: answered natively */
    uint32_t bodies;    /* b: native bodies: the handled calls plus their HasCollectible(0xbf) */
    uint32_t yes;       /* y: handled calls that returned al = 1 */
    uint32_t declined[ISAAC_VITA_HASCOLL_DECLINE_REASONS];
} IsaacVitaHasCollCounters;

/* The native HasCollectible (thiscall, ret 8): either answers natively or
 * runs the translated body (__real_sub_0033af30) on the untouched CPU. */
void isaac_vita_hascoll_native_call(struct CPU *__restrict c);

/* Take-and-zero for the phase profiler (ph120.hn); NULL discards. */
void isaac_vita_hascoll_native_take(IsaacVitaHasCollCounters *out);

#endif
