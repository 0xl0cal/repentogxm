#ifndef ISAAC_HOST_VITA_PNG_TEXEL_INIT_H
#define ISAAC_HOST_VITA_PNG_TEXEL_INIT_H

#include "guest.h"

#ifndef ISAAC_VITA_PNG_TEXEL_INIT_ELISION
#define ISAAC_VITA_PNG_TEXEL_INIT_ELISION 0
#endif

/* Frozen return word after ImagePng's one texel-buffer memset. */
#define ISAAC_VITA_PNG_TEXEL_INIT_RETURN_RVA 0x005a12e7U

/* Owner-thread cumulative admission evidence, not a timing estimate. No
 * clocks, locks, allocation or logging. Counters saturate rather than wrap. */
#define ISAAC_VITA_PNG_TEXEL_INIT_ABI 1u
typedef struct {
    uint32_t attempts, elided, faults, saturated;
    uint64_t bytes;
} IsaacVitaPngTexelInitSnapshot;
void isaac_vita_png_texel_init_snapshot(IsaacVitaPngTexelInitSnapshot *out);

/* Called after ordinary memset destination validation. One means the exact
 * PNG scratch clear is redundant; zero leaves the original memset intact;
 * minus one is a terminal lease-release fault (do not return/pop).
 * No guest CPU or memory mutation except that explicit terminal fault. */
int isaac_vita_png_texel_init_try(CPU *__restrict c, uint32_t destination,
                                int value, uint32_t count);

#endif
