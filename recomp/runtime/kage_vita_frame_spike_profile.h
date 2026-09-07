#ifndef KAGE_VITA_FRAME_SPIKE_PROFILE_H
#define KAGE_VITA_FRAME_SPIKE_PROFILE_H

#include <stdint.h>
#include <string.h>

/* No clocks or allocations here. Values come from existing phase seams.
 * Phase order is SERVICE, UPDATE, RENDER, PRESENT, LIMITER. PRESENT is nested
 * inside RENDER, so those fields must never be summed as disjoint costs.
 * These are completed outer-loop samples, not scanout or Present-gap samples.
 */
#define KAGE_VITA_SPIKE_PHASES 5u
#define KAGE_VITA_SPIKE_WINNERS 3u /* whole, update, render */
typedef struct {
    uint32_t loop, at_ms, whole_us, phase_us[KAGE_VITA_SPIKE_PHASES];
    uint32_t phase_mask, draws, clears, fbos, uploads, bad, clamped;
} KageVitaFrameSpike;
typedef struct {
    KageVitaFrameSpike current, worst[KAGE_VITA_SPIKE_WINNERS];
    uint32_t samples;
} KageVitaFrameSpikeWindow;

static inline void kage_vita_spike_record(KageVitaFrameSpikeWindow *s,
                                         uint32_t phase, uint32_t us)
{
    uint32_t *p;
    if (phase >= KAGE_VITA_SPIKE_PHASES)
        return;
    p = &s->current.phase_us[phase];
    if (*p > UINT32_MAX - us) {
        *p = UINT32_MAX;
        s->current.clamped = 1u;
    } else {
        *p += us;
    }
    s->current.phase_mask |= 1u << phase;
}

static inline void kage_vita_spike_finish(KageVitaFrameSpikeWindow *s,
                                         uint32_t loop, uint64_t at_us,
                                         uint64_t whole_us)
{
    KageVitaFrameSpike *p = &s->current;
    p->loop = loop;
    if (at_us / 1000u > UINT32_MAX || whole_us > UINT32_MAX)
        p->clamped = 1u;
    p->at_ms = at_us / 1000u > UINT32_MAX ? UINT32_MAX : (uint32_t)(at_us / 1000u);
    p->whole_us = whole_us > UINT32_MAX ? UINT32_MAX : (uint32_t)whole_us;
    if (!s->samples || p->whole_us > s->worst[0].whole_us)
        s->worst[0] = *p;
    if (!s->samples || p->phase_us[1] > s->worst[1].phase_us[1])
        s->worst[1] = *p;
    if (!s->samples || p->phase_us[2] > s->worst[2].phase_us[2])
        s->worst[2] = *p;
    ++s->samples;
    memset(p, 0, sizeof *p);
}

#endif
