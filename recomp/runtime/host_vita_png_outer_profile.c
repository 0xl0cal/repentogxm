/* Every-image outer attribution only. No image/decoder/guest state is changed.
 * The existing generated owner calls kage_vita_png_profile_image_begin/end.
 * Window-only builds use the aliases below; combined builds use the legacy
 * profiler's forwarding hooks and share its selected-image timestamps. */
#include "host_vita_png_outer_profile.h"
#include "kage_vita_deep_profile.h"
#include <errno.h>
#include <limits.h>
#include <string.h>

#if !defined(ISAAC_VITA_PNG_WINDOW_PROFILE)
#error "PNG outer attribution belongs to ISAAC_VITA_PNG_WINDOW_PROFILE"
#endif
#if defined(ISAAC_KAGE_VITA_PNG_DECODE_PROFILE_ORACLE)
uint64_t sceKernelGetProcessTimeWide(void);
#else
#include <psp2/kernel/processmgr.h>
#endif

static IsaacVitaPngOuterSnapshot s_outer;
static uint64_t s_started_at;
static uint32_t s_depth_lost;
#if defined(ISAAC_VITA_DEEP_PROFILE)
static kage_vita_deep_token s_deep_png;
#endif

static void outer_inc(uint32_t *value)
{
    if (*value == UINT32_MAX)
        s_outer.saturated = 1u;
    else
        ++*value;
}

static uint64_t outer_clock(void)
{
    const int saved_errno = errno;
    const uint64_t now = sceKernelGetProcessTimeWide();
    errno = saved_errno;
    return now;
}

void isaac_vita_png_outer_snapshot(IsaacVitaPngOuterSnapshot *out)
{
    if (out)
        *out = s_outer;
}

void isaac_vita_png_outer_begin_at(uint64_t now)
{
    if (s_depth_lost)
        return;
    if (s_outer.depth != 0u) {
        outer_inc(&s_outer.nested);
        if (s_outer.depth == UINT32_MAX) {
            s_outer.saturated = 1u;
            outer_inc(&s_outer.bad_sequence);
            /* Cannot reconstruct the matching unwind after depth overflow.
             * Leave the invalid active scope visible; never fabricate time. */
            s_depth_lost = 1u;
            return;
        }
        ++s_outer.depth;
        return;
    }
    outer_inc(&s_outer.started);
    s_outer.depth = 1u;
    s_started_at = now;
#if defined(ISAAC_VITA_DEEP_PROFILE)
    s_deep_png = kage_vita_deep_enter(KVD_PNG, 0u);
#endif
}

void isaac_vita_png_outer_end_at(uint64_t now)
{
    uint64_t elapsed;
    if (s_depth_lost)
        return;
    if (s_outer.depth == 0u) {
        outer_inc(&s_outer.bad_sequence);
        return;
    }
    if (--s_outer.depth != 0u)
        return;
#if defined(ISAAC_VITA_DEEP_PROFILE)
    kage_vita_deep_leave(&s_deep_png);
#endif
    outer_inc(&s_outer.completed);
    if (now < s_started_at) {
        outer_inc(&s_outer.bad_clock);
        return;
    }
    elapsed = now - s_started_at;
    outer_inc(&s_outer.timed);
    if (UINT64_MAX - s_outer.total_us < elapsed) {
        s_outer.total_us = UINT64_MAX;
        s_outer.saturated = 1u;
    } else {
        s_outer.total_us += elapsed;
    }
    if (elapsed > s_outer.max_us)
        s_outer.max_us = elapsed;
}

void isaac_vita_png_outer_begin(void)
{
    const uint64_t now = (!s_depth_lost && s_outer.depth == 0u)
        ? outer_clock() : 0u;
    isaac_vita_png_outer_begin_at(now);
}

void isaac_vita_png_outer_end(void)
{
    const uint64_t now = (!s_depth_lost && s_outer.depth == 1u)
        ? outer_clock() : 0u;
    isaac_vita_png_outer_end_at(now);
}

#if !defined(ISAAC_VITA_PNG_DECODE_PROFILE)
void kage_vita_png_profile_image_begin(void)
{
    isaac_vita_png_outer_begin();
}

void kage_vita_png_profile_image_end(void)
{
    isaac_vita_png_outer_end();
}
#endif

#if defined(ISAAC_KAGE_VITA_PNG_DECODE_PROFILE_ORACLE)
void isaac_vita_png_outer_oracle_reset(void)
{
    memset(&s_outer, 0, sizeof s_outer);
    s_started_at = 0u;
    s_depth_lost = 0u;
}

void isaac_vita_png_outer_oracle_seed(const IsaacVitaPngOuterSnapshot *value)
{
    s_outer = *value;
}
#endif
