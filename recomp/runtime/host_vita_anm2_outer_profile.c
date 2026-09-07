/* One generated function-scope cleanup token covers all ordinary returns.
 * This module neither receives a CPU nor touches guest registers/flags/data. */
#include "host_vita_anm2_outer_profile.h"
#include "kage_vita_deep_profile.h"
#include <errno.h>
#include <limits.h>
#include <string.h>

#if !defined(ISAAC_VITA_ANM2_WINDOW_PROFILE)
#error "ANM2 outer attribution belongs to ISAAC_VITA_ANM2_WINDOW_PROFILE"
#endif
#if defined(ISAAC_VITA_ANM2_OUTER_PROFILE_ORACLE)
uint64_t sceKernelGetProcessTimeWide(void);
#else
#include <psp2/kernel/processmgr.h>
#endif

static IsaacVitaAnm2OuterSnapshot s_outer;
static uint64_t s_started_at;
static uint32_t s_generation, s_poisoned;
#if defined(ISAAC_VITA_DEEP_PROFILE)
static kage_vita_deep_token s_deep_anm2;
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

void isaac_vita_anm2_outer_snapshot(IsaacVitaAnm2OuterSnapshot *out)
{
    if (out)
        *out = s_outer;
}

uint64_t isaac_vita_anm2_outer_begin(void)
{
    if (s_poisoned)
        return 0u;
    if (s_outer.depth != 0u) {
        outer_inc(&s_outer.nested);
        if (s_outer.depth == UINT32_MAX) {
            s_outer.saturated = 1u;
            outer_inc(&s_outer.bad_sequence);
            s_poisoned = 1u;
            return 0u;
        }
        ++s_outer.depth;
    } else {
        if (s_generation == UINT32_MAX) {
            /* Never recycle a scope identity after wrap. */
            s_outer.saturated = 1u;
            outer_inc(&s_outer.bad_sequence);
            s_poisoned = 1u;
            return 0u;
        }
        ++s_generation;
        outer_inc(&s_outer.started);
        s_outer.depth = 1u;
        s_started_at = outer_clock();
#if defined(ISAAC_VITA_DEEP_PROFILE)
        s_deep_anm2 = kage_vita_deep_enter(KVD_ANM2, 0u);
#endif
    }
    return ((uint64_t)s_generation << 32) | s_outer.depth;
}

void isaac_vita_anm2_outer_cleanup(uint64_t *token)
{
    uint64_t now, elapsed;
    if (s_poisoned)
        return;
    if (!token || *token == 0u || s_outer.depth == 0u ||
        (uint32_t)(*token >> 32) != s_generation ||
        (uint32_t)*token != s_outer.depth) {
        outer_inc(&s_outer.bad_sequence);
        /* Cannot claim subsequent scopes are balanced after a lost token. */
        s_poisoned = 1u;
        return;
    }
    *token = 0u;
    if (--s_outer.depth != 0u)
        return;
#if defined(ISAAC_VITA_DEEP_PROFILE)
    kage_vita_deep_leave(&s_deep_anm2);
#endif
    now = outer_clock();
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

#if defined(ISAAC_VITA_ANM2_OUTER_PROFILE_ORACLE)
void isaac_vita_anm2_outer_oracle_reset(void)
{
    memset(&s_outer, 0, sizeof s_outer);
    s_started_at = 0u;
    s_generation = s_poisoned = 0u;
}

void isaac_vita_anm2_outer_oracle_seed(const IsaacVitaAnm2OuterSnapshot *value,
                                     uint32_t generation)
{
    s_outer = *value;
    s_generation = generation;
}
#endif
