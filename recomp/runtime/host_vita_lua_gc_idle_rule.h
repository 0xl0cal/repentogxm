/* ISAAC_VITA_LUA_GC_IDLE_STEP: the carry rule, in one place.
 *
 * host_vita_lua_gc.c classifies the paced step LuaEngine::Update is about to
 * run (from Lua's own collector state, read only) and asks this function
 * whether to carry it one loop tail; the scheduler model
 * (kage_vita_gc_idle_scheduler_oracle.c) calls the same function with the
 * phases of its synthetic collector cycle, so the MODEL lines its test
 * prints cannot drift from the shipped rule.
 *
 * A step is carried only when it CANNOT run atomic() at the tail the game
 * issued it on:
 *   SWEEP / CALLFIN   luaC_step's loop ends at GCSpause (lgc.c:1136), and
 *                     atomic is behind pause and propagate;
 *   PROP_BOUND        propagate, and the gray objects already queued cost
 *                     more than the step's whole budget, so the loop stops
 *                     with the list non-empty (GCSatomic is entered only on
 *                     an empty list, lgc.c:1055-1056);
 * and never when it is, or may reach, the atomic:
 *   ATOMIC            gcstate == GCSatomic: the step is the atomic;
 *   PROP              propagate without that proof;
 *   PAUSE             the step restarts the cycle and marks from the roots;
 *   NOOP              GCdebt + k*1024 <= 0: lua_gc runs no luaC_step at all
 *                     (lapi.c:1076-1078, lgc.h:110); nothing to move.
 * The atomic therefore stays where the build without the knob runs it. */
#ifndef ISAAC_HOST_VITA_LUA_GC_IDLE_RULE_H
#define ISAAC_HOST_VITA_LUA_GC_IDLE_RULE_H

enum {
    VITA_GCI_PHASE_NOOP,
    VITA_GCI_PHASE_PAUSE,
    VITA_GCI_PHASE_PROP,
    VITA_GCI_PHASE_PROP_BOUND,
    VITA_GCI_PHASE_ATOMIC,
    VITA_GCI_PHASE_SWEEP,
    VITA_GCI_PHASE_CALLFIN,
    VITA_GCI_PHASES
};

/* 1 = carry the step to the next LuaEngine GC call, 0 = run it now.
 * `busy`: the scheduler answered KAGE_VITA_GC_IDLE_BUSY for this tail. */
static inline int vita_lua_gc_idle_carry(int busy, unsigned owed,
                                         unsigned owed_max, int phase)
{
    if (!busy || owed >= owed_max)
        return 0;
    return phase == VITA_GCI_PHASE_SWEEP || phase == VITA_GCI_PHASE_CALLFIN ||
        phase == VITA_GCI_PHASE_PROP_BOUND;
}

#endif
