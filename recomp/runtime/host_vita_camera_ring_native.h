/* Verdict counters of host_vita_camera_ring_native.c (ph120.cr). */
#ifndef HOST_VITA_CAMERA_RING_NATIVE_H
#define HOST_VITA_CAMERA_RING_NATIVE_H

#include <stdint.h>

/* Decline reasons, in the order of the ph120.cr d(...) legend. */
enum {
    ISAAC_VITA_CAMERA_RING_DECLINE_FRAME = 0,  /* f: guest stack cannot hold the frame */
    ISAAC_VITA_CAMERA_RING_DECLINE_POINTER,    /* o: null, wrapping or overlapping out/ring/data */
    ISAAC_VITA_CAMERA_RING_DECLINE_RING,       /* r: cap <= 0, start < 0 or start + n - 1 overflows */
    ISAAC_VITA_CAMERA_RING_DECLINE_TRUNC,      /* k: n * frac NaN or outside int32 */
    ISAAC_VITA_CAMERA_RING_DECLINE_LOGGER,     /* l: a step would reach the InverseLerp logger */
    ISAAC_VITA_CAMERA_RING_DECLINE_NAN,        /* x: NaN fraction, constant or element */
    ISAAC_VITA_CAMERA_RING_DECLINE_REASONS
};

typedef struct IsaacVitaCameraRingCounters {
    uint32_t calls;    /* n: isaac_vita_camera_ring_try entries */
    uint32_t handled;  /* h: replayed natively */
    uint32_t steps;    /* st: loop steps of the handled calls (sum of count) */
    uint32_t declined[ISAAC_VITA_CAMERA_RING_DECLINE_REASONS];
} IsaacVitaCameraRingCounters;

/* Take-and-zero for the phase profiler (ph120.cr); NULL discards. */
void isaac_vita_camera_ring_take(IsaacVitaCameraRingCounters *out);

#endif
