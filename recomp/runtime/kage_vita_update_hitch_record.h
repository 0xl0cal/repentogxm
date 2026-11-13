/* ISAAC_VITA_UPDATE_HITCH_PROBE: the ph120.uh / ph120.uhs records.
 *
 * Included by kage_vita_phase_profile.c only, and only when the knob is ON
 * (same arrangement as kage_vita_room_profile.h): it uses that unit's
 * KVPP_PRINTF, build id, loop counters and the seam headers it already has.
 * Contract and field legend: kage_vita_update_hitch.h and
 * recomp/vita/README.md "Update hitch probe". */
#ifndef KAGE_VITA_UPDATE_HITCH_RECORD_H
#define KAGE_VITA_UPDATE_HITCH_RECORD_H

#include "kage_vita_update_hitch.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(ISAAC_VITA_STATIC_SFX_PROFILE)
# include "host_vita_static_sfx_profile.h"
#endif
#if defined(ISAAC_VITA_PNG_WINDOW_PROFILE)
# include "host_vita_png_window_profile.h"
#endif
#if defined(ISAAC_VITA_PNG_TEXEL_INIT_ELISION)
# include "host_vita_png_texel_init.h"
#endif
#if defined(ISAAC_VITA_CRT_FILE_LOOKUP_HINT)
# include "host_vita_crt.h"
#endif
#if defined(ISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE)
int sceKernelGetThreadId(void);
#else
# include <psp2/kernel/threadmgr.h>
#endif

/* Groups, in record order.  A group is "off" when its seam is not compiled
 * into this build, "na" when one of the two snapshots could not be read. */
enum {
    KVUH_G_GL, KVUH_G_FH, KVUH_G_PNGI, KVUH_G_PNG, KVUH_G_SFX, KVUH_G_LUA,
    KVUH_G_SMP, KVUH_G_COUNT
};
#define KVUH_MAX_FIELDS 10u

typedef struct KvuhGroup {
    const char *name;   /* printed verbatim, with its own field legend */
    uint32_t fields;
    uint32_t compiled;
} KvuhGroup;

static const KvuhGroup s_kvuh_groups[KVUH_G_COUNT] = {
    { "gl(tex,draw,clear,fbo)", 4u, 1u },
    { "fh(hit,miss)", 2u,
#if defined(ISAAC_VITA_CRT_FILE_LOOKUP_HINT)
      1u },
#else
      0u },
#endif
    { "pngi(attempt,elided,bytes)", 3u,
#if defined(ISAAC_VITA_PNG_TEXEL_INIT_ELISION)
      1u },
#else
      0u },
#endif
    { "png(images,native,decode_us,io_us)", 4u,
#if defined(ISAAC_VITA_PNG_WINDOW_PROFILE)
      1u },
#else
      0u },
#endif
    { "sfx(wav_c,wav_us,ogg_c,ogg_us,riff_c,riff_us,up_c,up_us,play_c,play_us)",
      10u,
#if defined(ISAAC_VITA_STATIC_SFX_PROFILE)
      1u },
#else
      0u },
#endif
    { "lua(pc_n,pc_us,cb_n,cb_us,gc_n,gc_us)", 6u,
#if defined(KAGE_VITA_UPDATE_HITCH_LUA_COST)
      1u },
#else
      0u },
#endif
    { "smp(seq)", 1u,
#if defined(ISAAC_VITA_GUEST_SAMPLER)
      1u },
#else
      0u },
#endif
};

typedef struct KvuhSnapshot {
    uint32_t valid; /* bit per group */
    uint32_t v[KVUH_G_COUNT][KVUH_MAX_FIELDS];
} KvuhSnapshot;

static KvuhSnapshot s_kvuh_begin;
static uint64_t s_kvuh_begin_us;
static uint32_t s_kvuh_armed;
static uint32_t s_kvuh_events;
static uint32_t s_kvuh_banner;
#if defined(ISAAC_VITA_GUEST_SAMPLER)
/* Sampler scratch: game thread only, used between two updates. */
static uint32_t s_kvuh_self[KAGE_VITA_UPDATE_HITCH_TRAIL];
static uint32_t s_kvuh_caller[KAGE_VITA_UPDATE_HITCH_TRAIL];
#endif

/* Every counter is cumulative across one Game::Update: the window records
 * that zero some of them (ph120.sfx take, the GL counter exchange) run at
 * the outer-loop head, never between an update's begin and end; the Lua cost
 * unit's own 2 s window can close inside an update, which is why its
 * accessor returns lifetime totals. */
static void kvuh_snapshot(KvuhSnapshot *s)
{
    memset(s, 0, sizeof *s);
    {
        const IsaacVitaGlPhaseProfileCounters *g =
            &g_isaac_vita_gl_phase_profile_counters;
        s->v[KVUH_G_GL][0] = g->tex_image;
        s->v[KVUH_G_GL][1] = g->draw_elements;
        s->v[KVUH_G_GL][2] = g->clear;
        s->v[KVUH_G_GL][3] = g->bind_framebuffer;
        s->valid |= 1u << KVUH_G_GL;
    }
#if defined(ISAAC_VITA_CRT_FILE_LOOKUP_HINT)
    {
        uint32_t values[2];
        if (isaac_vita_crt_file_lookup_hint_get(values)) {
            s->v[KVUH_G_FH][0] = values[0];
            s->v[KVUH_G_FH][1] = values[1];
            s->valid |= 1u << KVUH_G_FH;
        }
    }
#endif
#if defined(ISAAC_VITA_PNG_TEXEL_INIT_ELISION)
    {
        IsaacVitaPngTexelInitSnapshot p;
        isaac_vita_png_texel_init_snapshot(&p);
        s->v[KVUH_G_PNGI][0] = p.attempts;
        s->v[KVUH_G_PNGI][1] = p.elided;
        s->v[KVUH_G_PNGI][2] = (uint32_t)p.bytes;
        s->valid |= 1u << KVUH_G_PNGI;
    }
#endif
#if defined(ISAAC_VITA_PNG_WINDOW_PROFILE)
    {
        IsaacVitaPngWindowSnapshot p;
        isaac_vita_png_window_snapshot(&p);
        s->v[KVUH_G_PNG][0] = p.images;
        s->v[KVUH_G_PNG][1] = p.native;
        s->v[KVUH_G_PNG][2] = (uint32_t)p.decode_us;
        s->v[KVUH_G_PNG][3] = (uint32_t)p.io_us;
        s->valid |= 1u << KVUH_G_PNG;
    }
#endif
#if defined(ISAAC_VITA_STATIC_SFX_PROFILE)
    {
        isaac_vita_static_sfx_window w;
        if (isaac_vita_static_sfx_profile_peek_window(&w)) {
            s->v[KVUH_G_SFX][0] = w.wav.count;
            s->v[KVUH_G_SFX][1] = w.wav.sum_us;
            s->v[KVUH_G_SFX][2] = w.ogg.count;
            s->v[KVUH_G_SFX][3] = w.ogg.sum_us;
            s->v[KVUH_G_SFX][4] = w.riff.count;
            s->v[KVUH_G_SFX][5] = w.riff.sum_us;
            s->v[KVUH_G_SFX][6] = w.upload.count;
            s->v[KVUH_G_SFX][7] = w.upload.sum_us;
            s->v[KVUH_G_SFX][8] = w.play.count;
            s->v[KVUH_G_SFX][9] = w.play.sum_us;
            s->valid |= 1u << KVUH_G_SFX;
        }
    }
#endif
#if defined(KAGE_VITA_UPDATE_HITCH_LUA_COST)
    isaac_vita_lua_cost_hitch_peek(s->v[KVUH_G_LUA]);
    s->valid |= 1u << KVUH_G_LUA;
#endif
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    s->v[KVUH_G_SMP][0] = kage_vita_guest_sampler_trail_seq();
    s->valid |= 1u << KVUH_G_SMP;
#endif
}

static size_t kvuh_put(char *out, size_t len, size_t cap, const char *fmt, ...)
{
    va_list args;
    int n;

    if (len >= cap)
        return len;
    va_start(args, fmt);
    n = vsnprintf(out + len, cap - len, fmt, args);
    va_end(args);
    if (n < 0)
        return len;
    len += (size_t)n;
    return len < cap ? len : cap - 1u;
}

static void kvuh_banner(void)
{
    char seams[128];
    size_t len = 0u;
    uint32_t g;

    if (s_kvuh_banner)
        return;
    s_kvuh_banner = 1u;
    seams[0] = '\0';
    for (g = 0u; g < KVUH_G_COUNT; ++g) {
        const char *name = s_kvuh_groups[g].name;
        len = kvuh_put(seams, len, sizeof seams, "%s%.*s=%s", g ? " " : "",
                       (int)strcspn(name, "("), name,
                       s_kvuh_groups[g].compiled ? "on" : "off");
    }
    KVPP_PRINTF("[kage-vita] KAGE VITA UPDATE HITCH PROBE: bid=%.32s abi=%u "
                "threshold_us=%u trail=%u top=%u seams: %s\n",
                ISAAC_VITA_PHASE_PROFILE_BUILD_ID, KAGE_VITA_UPDATE_HITCH_ABI,
                (unsigned)(ISAAC_VITA_UPDATE_HITCH_PROBE_THRESHOLD_US),
                KAGE_VITA_UPDATE_HITCH_TRAIL, KAGE_VITA_UPDATE_HITCH_TOP,
                seams);
}

void kage_vita_update_hitch_begin(uint64_t begin_us)
{
    kvuh_banner();
    kvuh_snapshot(&s_kvuh_begin);
    s_kvuh_begin_us = begin_us;
    s_kvuh_armed = 1u;
}

#if defined(ISAAC_VITA_GUEST_SAMPLER)
static size_t kvuh_put_top(char *out, size_t len, size_t cap,
                           const char *name, uint32_t *values, uint32_t n)
{
    uint32_t keys[KAGE_VITA_UPDATE_HITCH_TOP];
    uint32_t counts[KAGE_VITA_UPDATE_HITCH_TOP];
    uint32_t used = kage_vita_update_hitch_top(
        values, n, keys, counts, KAGE_VITA_UPDATE_HITCH_TOP);
    uint32_t i;

    len = kvuh_put(out, len, cap, " %s=", name);
    if (!used)
        return kvuh_put(out, len, cap, "-");
    for (i = 0u; i < used; ++i)
        len = kvuh_put(out, len, cap, "%s%x:%u", i ? "," : "",
                       (unsigned)keys[i], (unsigned)counts[i]);
    return len;
}
#endif

void kage_vita_update_hitch_end(uint64_t begin_us, uint64_t end_us,
                                uint32_t frame_before, uint32_t frame_after)
{
    KvuhSnapshot end;
    char line[768];
    size_t len = 0u;
    uint32_t g, f, armed = s_kvuh_armed;

    s_kvuh_armed = 0u;
    /* An end without this update's own begin has no baseline to subtract. */
    if (!armed || begin_us != s_kvuh_begin_us)
        return;
    if (!kage_vita_update_hitch_due(
            begin_us, end_us,
            (uint64_t)(ISAAC_VITA_UPDATE_HITCH_PROBE_THRESHOLD_US)))
        return;
    kvuh_snapshot(&end);
    ++s_kvuh_events;
    len = kvuh_put(line, len, sizeof line,
        "[kage-vita] ph120.uh bid=%.32s abi=%u n=%u win=%u loop=%u tid=%08x "
        "thr_us=%u us=%u begin=%08x%08x frame=%u>%u",
        ISAAC_VITA_PHASE_PROFILE_BUILD_ID, KAGE_VITA_UPDATE_HITCH_ABI,
        (unsigned)s_kvuh_events,
        (unsigned)(s_completed_loops / KAGE_VITA_PHASE_PROFILE_WINDOW + 1u),
        (unsigned)s_last_outer_loop, (unsigned)sceKernelGetThreadId(),
        (unsigned)(ISAAC_VITA_UPDATE_HITCH_PROBE_THRESHOLD_US),
        (unsigned)(end_us - begin_us > UINT32_MAX
                       ? UINT32_MAX : end_us - begin_us),
        (unsigned)(begin_us >> 32), (unsigned)begin_us,
        (unsigned)frame_before, (unsigned)frame_after);
    for (g = 0u; g < KVUH_G_COUNT; ++g) {
        const KvuhGroup *group = &s_kvuh_groups[g];
        len = kvuh_put(line, len, sizeof line, " %s=", group->name);
        if (!group->compiled) {
            len = kvuh_put(line, len, sizeof line, "off");
            continue;
        }
        if (((s_kvuh_begin.valid & end.valid) >> g & 1u) == 0u) {
            len = kvuh_put(line, len, sizeof line, "na");
            continue;
        }
        for (f = 0u; f < group->fields; ++f)
            len = kvuh_put(line, len, sizeof line, "%s%u", f ? "," : "",
                           (unsigned)(end.v[g][f] - s_kvuh_begin.v[g][f]));
    }
    KVPP_PRINTF("%s\n", line);
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    if ((s_kvuh_begin.valid & end.valid) >> KVUH_G_SMP & 1u) {
        uint32_t lost = 0u;
        uint32_t n = kage_vita_guest_sampler_trail_copy(
            s_kvuh_begin.v[KVUH_G_SMP][0], end.v[KVUH_G_SMP][0],
            s_kvuh_self, s_kvuh_caller, &lost);
        len = kvuh_put(line, 0u, sizeof line,
            "[kage-vita] ph120.uhs bid=%.32s abi=%u n=%u samples=%u lost=%u",
            ISAAC_VITA_PHASE_PROFILE_BUILD_ID, KAGE_VITA_UPDATE_HITCH_ABI,
            (unsigned)s_kvuh_events, (unsigned)n, (unsigned)lost);
        len = kvuh_put_top(line, len, sizeof line, "self", s_kvuh_self, n);
        len = kvuh_put_top(line, len, sizeof line, "caller", s_kvuh_caller, n);
        KVPP_PRINTF("%s\n", line);
    }
#endif
    (void)len;
}

#endif
