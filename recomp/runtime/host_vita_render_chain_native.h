/* Verdict counters of host_vita_render_chain_native.c (ph120.rn). */
#ifndef HOST_VITA_RENDER_CHAIN_NATIVE_H
#define HOST_VITA_RENDER_CHAIN_NATIVE_H

#include <stdint.h>

typedef struct IsaacVitaRenderChainCounters {
    uint32_t fill_calls;      /* f: sub_0025af60 entries */
    uint32_t fill_declined;   /* fd: guest stack cannot hold the frame -> translated */
    uint32_t getters_native;  /* gn: size getters answered from the pinned leaf */
    uint32_t getters_called;  /* gc: size getters entered through guest_call */
    uint32_t releases;        /* rl: counter non-null -> DecrementReference called */
    uint32_t render_calls;    /* r: sub_0055f610 entries */
    uint32_t render_inlined;  /* ri: [vtable+8] was sub_0055f630, replayed here */
    uint32_t render_declined; /* rd: guest stack cannot hold the frames -> translated */
} IsaacVitaRenderChainCounters;

/* Take-and-zero for the phase profiler (ph120.rn); NULL discards. */
void isaac_vita_render_chain_take(IsaacVitaRenderChainCounters *out);

#endif
