#ifndef ISAAC_THEORA_CADENCE_PROFILE_H
#define ISAAC_THEORA_CADENCE_PROFILE_H
#if defined(ISAAC_VITA_THEORA_CADENCE_PROFILE) && ISAAC_VITA_THEORA_CADENCE_PROFILE
#include "host_vita_native_theora_snapshot.h"
#include "kage_vita_fullspeed_scheduler.h"

/* Included only by the existing pump TU (and the executable host oracle).
 * One interval starts immediately before a service and closes at the next
 * service entry: its Render decisions therefore belong to its paid decode.
 * Three complete records share an id; no logger call fragments a line.
 * u64 times are hex microseconds, split into words: Vita printf %llu is unsafe.
 * Limit is per process, not per clip. The first refused interval logs overflow.
 */
#define VITA_TC_MAX_ROWS 2048U
#define VITA_TC_U64(v) (unsigned)((uint64_t)(v) >> 32), (unsigned)(uint32_t)(v)

typedef struct VitaTheoraCadenceSample {
    uint64_t now_us;
    VitaTheoraNativeSnapshot native;
    KageVitaFullspeedSnapshot scheduler;
    uint32_t clips;
    uint32_t scheduler_enabled;
} VitaTheoraCadenceSample;

typedef struct VitaTheoraCadenceProfile {
    VitaTheoraCadenceSample begin, end;
    uint64_t pump_us;
    uint64_t fetch_us;
    uint64_t subupload_us;
    uint32_t subuploads;
    uint32_t site, pending, rows, overflow, flags;
    uint32_t fetches, upload_pops, discards, no_upload, fetch_faults, nested;
} VitaTheoraCadenceProfile;

void isaac_vita_log(const char *format, ...);

/* flags: 1 counter retreat/reset, 2 saturated counter, 4 clock/time retreat,
 * 8 guest fault, 16 nested service, 32 invalid manager vector. Never infer an
 * exact delta from a row with flags != 0. */
static uint32_t vita_tc_delta32(uint32_t a, uint32_t b, uint32_t *flags)
{
    if (a == UINT32_MAX || b == UINT32_MAX) *flags |= 2U;
    if (b < a) { *flags |= 1U; return 0U; }
    return b - a;
}

static uint64_t vita_tc_delta64(uint64_t a, uint64_t b, uint32_t *flags)
{
    if (b < a) { *flags |= 4U; return 0U; }
    return b - a;
}

static void vita_tc_add(uint32_t *value, uint32_t n, uint32_t *flags)
{
    if (n >= UINT32_MAX - *value) { *value = UINT32_MAX; *flags |= 2U; }
    else *value += n;
}

static void vita_tc_fetch(VitaTheoraCadenceProfile *p, uint32_t dropped_before,
                          uint32_t dropped_after, uint32_t popped_before,
                          uint32_t popped_after, uint32_t fault)
{
    uint32_t pops;
    if (!p->pending || p->overflow) return;
    vita_tc_add(&p->fetches, 1U, &p->flags);
    if (fault) {
        vita_tc_add(&p->fetch_faults, 1U, &p->flags);
        p->flags |= 8U;
        return;
    }
    pops = vita_tc_delta32(popped_before, popped_after, &p->flags);
    vita_tc_add(&p->upload_pops, pops, &p->flags);
    vita_tc_add(&p->discards,
        vita_tc_delta32(dropped_before, dropped_after, &p->flags), &p->flags);
    if (!pops) vita_tc_add(&p->no_upload, 1U, &p->flags);
}

static void vita_tc_fetch_time(VitaTheoraCadenceProfile *p,
                               uint64_t begin, uint64_t end)
{
    uint64_t elapsed;
    if (!p->pending || p->overflow) return;
    elapsed = vita_tc_delta64(begin, end, &p->flags);
    if (elapsed > UINT64_MAX - p->fetch_us) {
        p->fetch_us = UINT64_MAX;
        p->flags |= 2U;
    } else p->fetch_us += elapsed;
}

static void vita_tc_subupload_time(VitaTheoraCadenceProfile *p,
                                   uint64_t begin, uint64_t end)
{
    uint64_t elapsed;
    if (!p->pending || p->overflow) return;
    vita_tc_add(&p->subuploads, 1U, &p->flags);
    elapsed = vita_tc_delta64(begin, end, &p->flags);
    if (elapsed > UINT64_MAX - p->subupload_us) {
        p->subupload_us = UINT64_MAX;
        p->flags |= 2U;
    } else p->subupload_us += elapsed;
}

static void vita_tc_close(VitaTheoraCadenceProfile *p,
                           const VitaTheoraCadenceSample *next, uint32_t next_site)
{
    uint32_t flags = p->flags;
    uint32_t packets, rgb, vorbis, dup, errors, sp, sr, sv;
    uint32_t full, nonfull, late, ns, presents, game, ticks, disables;
    uint64_t packet_us, rgb_us, vorbis_us, interval_us, service_us;
    if (!p->pending || p->overflow) return;
    if (p->begin.clips == UINT32_MAX || p->end.clips == UINT32_MAX ||
            next->clips == UINT32_MAX) flags |= 32U;
    /* Keep zero-work rows while any clip exists; include the final interval
     * whose clip was destroyed between services. No guest object is retained. */
    if (!p->begin.clips && !p->end.clips && !next->clips && !p->fetches &&
            p->begin.native.packets == next->native.packets &&
            p->begin.native.rgb_decodes == next->native.rgb_decodes) return;
    if (p->rows >= VITA_TC_MAX_ROWS) {
        p->overflow = 1U;
        isaac_vita_log("tc.overflow rows=%u next_site=%08x tick=%u further_intervals_omitted=1",
            (unsigned)p->rows, (unsigned)next_site,
            (unsigned)next->scheduler.wrapper_ticks);
        return;
    }
#define TC_N(field) vita_tc_delta32(p->begin.native.field, next->native.field, &flags)
#define TC_S(field) vita_tc_delta32(p->begin.scheduler.field, next->scheduler.field, &flags)
    packets = TC_N(packets); rgb = TC_N(rgb_decodes); vorbis = TC_N(vorbis_syntheses);
    dup = TC_N(dup_frames); errors = TC_N(packet_errors);
    sp = vita_tc_delta32(p->begin.native.packets, p->end.native.packets, &flags);
    sr = vita_tc_delta32(p->begin.native.rgb_decodes, p->end.native.rgb_decodes, &flags);
    sv = vita_tc_delta32(p->begin.native.vorbis_syntheses, p->end.native.vorbis_syntheses, &flags);
    full = TC_S(full_phases); nonfull = TC_S(nonfull_phases);
    late = TC_S(late_full_skips); ns = TC_S(nonfull_skips);
    presents = TC_S(presents); game = TC_S(game_updates);
    ticks = TC_S(wrapper_ticks); disables = TC_S(runtime_disables);
#undef TC_N
#undef TC_S
    packet_us = vita_tc_delta64(p->begin.native.packetin_us_sum, next->native.packetin_us_sum, &flags);
    rgb_us = vita_tc_delta64(p->begin.native.rgb_us_sum, next->native.rgb_us_sum, &flags);
    vorbis_us = vita_tc_delta64(p->begin.native.vorbis_us_sum, next->native.vorbis_us_sum, &flags);
    interval_us = vita_tc_delta64(p->begin.now_us, next->now_us, &flags);
    service_us = vita_tc_delta64(p->begin.now_us, p->end.now_us, &flags);
    ++p->rows;
    isaac_vita_log("tc.s id=%u site=%08x next=%08x tick=%u..%u clips=%u/%u/%u at=%08x%08x interval_us=%08x%08x pump_us=%08x%08x service_us=%08x%08x flags=%x nested=%u enabled=%u/%u/%u",
        (unsigned)p->rows, (unsigned)p->site, (unsigned)next_site,
        (unsigned)p->begin.scheduler.wrapper_ticks, (unsigned)next->scheduler.wrapper_ticks,
        (unsigned)p->begin.clips, (unsigned)p->end.clips, (unsigned)next->clips,
        VITA_TC_U64(p->begin.now_us), VITA_TC_U64(interval_us),
        VITA_TC_U64(p->pump_us), VITA_TC_U64(service_us),
        (unsigned)flags, (unsigned)p->nested,
        (unsigned)p->begin.scheduler_enabled, (unsigned)p->end.scheduler_enabled,
        (unsigned)next->scheduler_enabled);
    isaac_vita_log("tc.n id=%u packets=%u rgb=%u vorbis=%u dup=%u errors=%u service_p/r/v=%u/%u/%u packet_us=%08x%08x rgb_us=%08x%08x vorbis_us=%08x%08x",
        (unsigned)p->rows, (unsigned)packets, (unsigned)rgb, (unsigned)vorbis,
        (unsigned)dup, (unsigned)errors, (unsigned)sp, (unsigned)sr, (unsigned)sv,
        VITA_TC_U64(packet_us), VITA_TC_U64(rgb_us), VITA_TC_U64(vorbis_us));
    isaac_vita_log("tc.r id=%u ticks=%u full=%u nonfull=%u late_full_skip=%u nonfull_skip=%u presents=%u game=%u disables=%u fetch=%u upload_pop=%u discard=%u no_upload=%u fetch_fault=%u fetch_us=%08x%08x subuploads=%u subupload_us=%08x%08x",
        (unsigned)p->rows, (unsigned)ticks, (unsigned)full, (unsigned)nonfull,
        (unsigned)late, (unsigned)ns, (unsigned)presents, (unsigned)game,
        (unsigned)disables, (unsigned)p->fetches, (unsigned)p->upload_pops,
        (unsigned)p->discards, (unsigned)p->no_upload, (unsigned)p->fetch_faults,
        VITA_TC_U64(p->fetch_us), (unsigned)p->subuploads,
        VITA_TC_U64(p->subupload_us));
}

static void vita_tc_begin(VitaTheoraCadenceProfile *p,
                           const VitaTheoraCadenceSample *sample, uint32_t site)
{
    p->begin = *sample;
    p->end = *sample;
    p->site = site;
    p->pending = 1U;
    p->pump_us = 0U;
    p->fetch_us = 0U;
    p->subupload_us = 0U;
    p->subuploads = 0U;
    p->flags = p->fetches = p->upload_pops = p->discards = 0U;
    p->no_upload = p->fetch_faults = p->nested = 0U;
}

static void vita_tc_end(VitaTheoraCadenceProfile *p,
                         const VitaTheoraCadenceSample *sample,
                         uint64_t pump_start, uint64_t pump_end, uint32_t fault)
{
    p->end = *sample;
    p->pump_us = vita_tc_delta64(pump_start, pump_end, &p->flags);
    if (fault) p->flags |= 8U;
}
#endif
#endif
