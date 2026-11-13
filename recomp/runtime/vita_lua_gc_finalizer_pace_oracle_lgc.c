/* One lgc.c variant plus accessors, for vita_lua_gc_finalizer_pace_oracle.c.
 *
 * recomp/test_vita_lua_gc_finalizer_pace.py compiles this file as
 *
 *   clang -m32 -O2 -I<lua>/src -DluaC_step=oracle_real_luaC_step
 *         -DluaE_setdebt=oracle_lgc_setdebt_hook
 *         [-DORACLE_QUOTA=1 [-DORACLE_LATCH=1]]
 *         -include <variant>/lgc.c vita_lua_gc_finalizer_pace_oracle_lgc.c
 *
 * so the variant's lgc.c (pristine, a derivation, an earlier revision or a
 * mutant) is the first text of the translation unit and everything below
 * sees its file-static state and helpers.  The oracle then needs no copy of
 * any derived formula and predicts nothing:
 *
 *   - oracle_lgc_step_budget() is the isaac_pace_budget the variant's own
 *     lgc.c:1133 set for the luaC_step that just returned (-1: the step ran
 *     as a pristine step, a latched state's).  Every running luaC_step ends in a luaE_setdebt
 *     of lgc.c (setpause, line 945, or line 1141), which the rename above
 *     routes through oracle_lgc_setdebt_hook: the hook records the budget
 *     and calls lstate.c's luaE_setdebt, nothing else, so it also sees the
 *     rev4 step's own budget before that build restores the caller's
 *     (its line 1143).  Nothing of the oracle runs on the global_State
 *     before the step (an earlier oracle ran the derived
 *     cut on the real state before the step, which masked a mutant without
 *     the cut); oracle_lgc_step_begin only resets the record;
 *   - oracle_lgc_fin_cost / oracle_lgc_sweep_cost evaluate the variant's own
 *     GCFINALIZECOST / GCSWEEPCOST macros (lgc.c:45 / :39) at a given budget,
 *     restoring isaac_pace_budget afterwards;
 *   - with ORACLE_LATCH (the latch derivation and its mutants) the line-60
 *     schedule helpers and the line-61 script latch are exposed for the
 *     oracle's "helpers" boundary checks on synthetic global_States.
 *
 * Only the oracle links this object; no build of the game does.
 */
#if !defined(lgc_c)
#error "compile with -include <variant>/lgc.c"
#endif

#if !defined(ORACLE_QUOTA)
#define ORACLE_QUOTA 0
#endif
#if !defined(ORACLE_LATCH)
#define ORACLE_LATCH 0
#endif
#if ORACLE_LATCH && !ORACLE_QUOTA
#error "ORACLE_LATCH needs ORACLE_QUOTA"
#endif

#define ORACLE_NO_BUDGET (-2) /* a variant without line 44 (pristine, rev0) */
#define ORACLE_UNSEEN (-3)    /* no luaE_setdebt of lgc.c since step_begin */

/* lgc.c's luaE_setdebt calls were renamed to the hook; lstate.c's own
 * luaE_setdebt is the real one */
#undef luaE_setdebt
void luaE_setdebt (global_State *g, l_mem debt);

l_mem oracle_lgc_budget(void);
void oracle_lgc_step_begin(void);
l_mem oracle_lgc_step_budget(void);
lu_mem oracle_lgc_fin_cost(l_mem budget);
lu_mem oracle_lgc_sweep_cost(l_mem budget);

static l_mem oracle_step_budget = ORACLE_UNSEEN;

void oracle_lgc_setdebt_hook (global_State *g, l_mem debt)
{
#if ORACLE_QUOTA
    oracle_step_budget = isaac_pace_budget;
#else
    oracle_step_budget = ORACLE_NO_BUDGET;
#endif
    luaE_setdebt(g, debt);
}

/* isaac_pace_budget now */
l_mem oracle_lgc_budget(void)
{
#if ORACLE_QUOTA
    return isaac_pace_budget;
#else
    return ORACLE_NO_BUDGET;
#endif
}

void oracle_lgc_step_begin(void) { oracle_step_budget = ORACLE_UNSEEN; }

/* isaac_pace_budget at lgc.c's last luaE_setdebt since step_begin */
l_mem oracle_lgc_step_budget(void) { return oracle_step_budget; }

lu_mem oracle_lgc_fin_cost(l_mem budget)
{
#if ORACLE_QUOTA
    l_mem saved = isaac_pace_budget;
    lu_mem cost;
    isaac_pace_budget = budget;
    cost = (lu_mem)(GCFINALIZECOST);
    isaac_pace_budget = saved;
    return cost;
#else
    (void)budget;
    return (lu_mem)(GCFINALIZECOST);
#endif
}

lu_mem oracle_lgc_sweep_cost(l_mem budget)
{
#if ORACLE_QUOTA
    l_mem saved = isaac_pace_budget;
    lu_mem cost;
    isaac_pace_budget = budget;
    cost = (lu_mem)(GCSWEEPCOST);
    isaac_pace_budget = saved;
    return cost;
#else
    (void)budget;
    return (lu_mem)(GCSWEEPCOST);
#endif
}

#if ORACLE_LATCH
lu_mem oracle_lgc_cost(l_mem quota, l_mem cap, l_mem budget);
void oracle_lgc_set_budget(l_mem budget);
l_mem oracle_lgc_thr(const global_State *g);
long long oracle_lgc_extra(const global_State *g);
void oracle_lgc_wr(global_State *g, long long v);
l_mem oracle_lgc_rd(const global_State *g);
void oracle_lgc_setdebt(global_State *g, l_mem debt);
void oracle_lgc_open(global_State *g);
long long oracle_lgc_vend(const global_State *g);
void oracle_lgc_late(global_State *g, long long v);
lu_mem oracle_lgc_swept(global_State *g, int n, lu_mem c);
void oracle_lgc_forget(const global_State *g);

/* line 44 at an explicit quota and cap (the charge's base, budget-0 and cap
 * values) */
lu_mem oracle_lgc_cost(l_mem quota, l_mem cap, l_mem budget)
{
    l_mem saved = isaac_pace_budget;
    lu_mem cost;
    isaac_pace_budget = budget;
    cost = isaac_pace_cost(quota, cap);
    isaac_pace_budget = saved;
    return cost;
}

void oracle_lgc_set_budget(l_mem budget) { isaac_pace_budget = budget; }
l_mem oracle_lgc_thr(const global_State *g) { return isaac_pace_thr(g); }
long long oracle_lgc_extra(const global_State *g) { return isaac_pace_extra(g); }
void oracle_lgc_wr(global_State *g, long long v) { isaac_pace_wr(g, v); }
l_mem oracle_lgc_rd(const global_State *g) { return isaac_pace_rd(g); }
void oracle_lgc_setdebt(global_State *g, l_mem debt) { isaac_pace_setdebt(g, debt); }
void oracle_lgc_open(global_State *g) { isaac_pace_open(g); }
long long oracle_lgc_vend(const global_State *g) { return isaac_pace_vend(g); }
void oracle_lgc_late(global_State *g, long long v) { isaac_pace_late(g, v); }
lu_mem oracle_lgc_swept(global_State *g, int n, lu_mem c)
{
    return isaac_pace_swept(g, n, c);
}
void oracle_lgc_forget(const global_State *g) { isaac_pace_forget(g); }
#endif

/* ORACLE_ALLOC_LATCH=1: the variant was derived with --alloc-latch A > 0
 * (ISAAC_VITA_LUA_GC_FINALIZER_ALLOC_LATCH_BYTES, derived lgc.c:61/821/958);
 * the count is the derived file's own receipt. */
#if !defined(ORACLE_ALLOC_LATCH)
#define ORACLE_ALLOC_LATCH 0
#endif
#if ORACLE_ALLOC_LATCH && !ORACLE_LATCH
#error "ORACLE_ALLOC_LATCH needs ORACLE_LATCH"
#endif
unsigned int oracle_lgc_alloc_latches(void);
unsigned int oracle_lgc_alloc_latches(void)
{
#if ORACLE_ALLOC_LATCH
    return isaac_lua_gc_alloc_latches();
#else
    return 0u;
#endif
}
