/* Host oracle + device projection for ISAAC_VITA_LUA_GC_FINALIZER_PACE.
 *
 * Built by recomp/test_vita_lua_gc_finalizer_pace.py against the
 * manifest-verified Lua 5.3.3 sources, once per lgc.c/lbaselib.c variant:
 * pristine, the derivation of recomp/vita/lua53_gc_finalizer_pace.py
 * (the script-control latch, with and without the sweep line), earlier
 * revisions of it -- rev4 (c447d9e, no latch: it paces collectgarbage
 * control legs the latch hands back to pristine), rev1 (eaec5e2, quotas
 * without the schedule rule), rev0 (0f5324a, a constant 512-unit
 * finalizer charge) -- and mutants that each drop
 * or move one derived rule.  The variant's lgc.c is compiled into
 * vita_lua_gc_finalizer_pace_oracle_lgc.c (-include, see there) with
 * -DluaC_step=oracle_real_luaC_step, so every luaC_step -- the paced
 * lua_gc(LUA_GCSTEP) (lapi.c:1075-1078 -> luaC_checkGC), every
 * allocation-driven luaC_checkGC inside an API call (lapi.c, ldo.c:333,
 * lstate.c:259, ...) and every collectgarbage("step") of a control script --
 * passes through luaC_step below.  lgc.c itself never calls luaC_step, so
 * nothing bypasses the wrapper.  After each step the wrapper reads the
 * budget the variant's own lgc.c:1133 chose (oracle_lgc_step_budget: -1 =
 * the step ran as a pristine step), recorded at the step's last luaE_setdebt
 * of lgc.c; nothing of the oracle touches the global_State before the step.
 *
 * Every build runs the same workload (runs differ only through Lua's
 * time- and address-derived string-hash seed, lstate.c luai_makeseed),
 * shaped like LuaEngine::Update with EID loaded (0x4022d0..0x4023cf: one
 * lua_gc(L, LUA_GCSTEP, K) per main loop, K = ISAAC_VITA_LUA_GCSTEP_CLAMP_KB,
 * because the game's own step never drops below 50 KB and the clamp applies
 * to every larger request) and one lua_gc(L, LUA_GCCOUNT) per loop:
 *
 *   - a live heap of LIVE_KB of tables/strings in 256-entry chunks (EID:
 *     ~11.3 MB, P2b ph120.mem lua(l,...) 11223..11674 KiB);
 *   - per loop FIN userdata of 8 bytes with a C __gc (EID: ~443 LuaBridge
 *     finalizers/s at ~42 loops/s in P2b = ~10.5 per loop; LuaBridge
 *     UserdataPtr is 8 bytes on x86-32: lua_newuserdata(8));
 *     every 4th is kept in a ring and dies RING loops later, every 3rd is a
 *     weak value, every 5th a weak key (the two weak tables are replaced by
 *     fresh ones every WEAK_ROTATE loops; EID itself has no weak tables --
 *     0 "__mode" in its 22 files -- and one table keyed by an ever-growing
 *     id would become a multi-MB single propagatemark that no real heap
 *     has);
 *   - per loop GARB small tables of plain garbage;
 *   - optionally (key=value options after the positional ones):
 *       burst=P,L,N  loops f with f % P < L create N finalizable userdata
 *                    instead of FIN (the device's 14 s runs at 50-70/step);
 *       spike=F,N    loop F creates N more (one cycle's 8,328 in one step);
 *       cap_kb=N     the Lua allocator refuses to hold more than N KB (an
 *                    arena with no fallback): a refusal makes Lua run an
 *                    emergency collection (lmem.c luaM_realloc_), a second
 *                    refusal raises LUA_ERRMEM, which the oracle's panic
 *                    handler reports as "PANIC" (exit 3); emergency mode runs
 *                    no finalizer, so whatever tobefnz holds stays allocated;
 *       ctl=NAME[:A[,B]]  a Lua control script (repeatable, applied in
 *                    order): Lua code that calls collectgarbage() through
 *                    the base library -- the derived lbaselib.c in a latch
 *                    build, so it reaches the latch exactly as a mod's call
 *                    does -- at setup, before the loop's work (pre) or after
 *                    it, before the pacer's step (post); see s_ctls;
 *       collect=N    the engine's own lua_gc(LUA_GCCOLLECT) every N-th loop
 *                    (from C, like the engine's clamp path: no latch);
 *       fin_alloc=N  every C __gc allocates N small tables (a LuaBridge
 *                    destructor that touches Lua): GCTM clears gcrunning
 *                    around the call, so those allocations reach luaC_step's
 *                    stopped path (lgc.c:1130, pristine's 10 x GCSTEPSIZE
 *                    credit), also from the tail runafewfinalizers after a
 *                    paced step's setdebt (lgc.c:1142);
 *       fin_str=N    every C __gc makes one N-byte string (N >= 64: a long
 *                    string, a fresh allocation per call);
 *       err=P        the __gc of every P-th object raises "boom"; the loop's
 *                    work and the engine's lua_gc(LUA_GCSTEP) then each run
 *                    inside lua_pcall (both protected);
 *       hostpush=N   N unprotected 16-byte lua_newuserdata per loop, ahead
 *                    of the protected work: with err=P, a raising __gc that
 *                    an allocation-driven step runs there panics ("PANIC",
 *                    exit 3) -- after an error unwound a step, pristine's
 *                    stopped-path credit (GCTM's own "boom" allocation)
 *                    keeps the next unprotected allocation from stepping;
 *       birth=1      latch the state at lua_newstate, before any library is
 *                    open (a latch build only; elsewhere ignored), so the whole
 *                    run is the latched (pristine-charged) path;
 *       abort_kb=N   end the run (exit 4, "ABORT") once the heap passes N
 *                    KB, so a diverging build fails fast;
 *       seed=N       fold the collector's state after every loop (gcstate,
 *                    total bytes, GCdebt, GCestimate, gcfinnum, tobefnz
 *                    length, the loop's finalizers and luaC_steps) into a
 *                    digest, printed as "exact seed=N digest=..."; N is
 *                    also what oracle_seed() returns, which the exact
 *                    builds' lstate.c uses as its whole string-hash seed
 *                    (the pace test's EXACT legs: with the seed fixed and
 *                    the address space not randomized and the package
 *                    library left out -- its registry key is the static
 *                    address &CLIBS, which differs between two links --
 *                    a run is a function of the collector code, so a
 *                    latched build must reproduce pristine's digest bit
 *                    for bit; elsewhere lstate.c's own time/address seed
 *                    stands, every library is open, and the digest only
 *                    describes that run);
 *       trace=N      print heap, tobefnz length and GC state every N loops.
 *
 * Invariants checked in every build (any violation fails the run):
 *   early   a finalizer runs while the object is still in the ring;
 *   weak    a finalized object is still a weak *value* (5.3 clears weak
 *           values before separating finalizers, lgc.c:1001-1006);
 *   order   within one finalizer batch ids must decrease (tobefnz is built
 *           newest-first from finobj, lgc.c separatetobefnz); an emergency
 *           collection may append two newer batches (order_slack);
 *   twice/never  each id finalized exactly once, and after lua_close every
 *           created id was finalized;
 *   keys    after two full collections no weak key refers to a finalized
 *           object.
 * Pacing bound, per paced luaC_step (the derived builds must hold it;
 * pristine and the quota/cap/budget mutants are judged by the same numbers
 * and must violate it, which shows the check has teeth): finalizers run
 * inside one luaC_step <= (budget + GCSTEPSIZE) / c + 2 * MAX + 1, budget =
 * getdebt(g) on entry (lgc.c:1112-1121) and c the finalizer charge of that
 * step (ORACLE_QUOTA: the variant's own GCFINALIZECOST at that budget;
 * otherwise the constant bound_cost).  The loop of lgc.c:1133-1136 stops
 * once the charged work reaches budget + GCSTEPSIZE, the last singlestep
 * overshoots by at most MAX, and the tail runafewfinalizers (lgc.c:1142)
 * adds at most MAX (atomic sets gcfinnum to 1 and the derived lgc.c:848
 * saturates its doubling at MAX in a paced step past atomic).  bound0 is the
 * budget-free form the device gate checks:
 * floor(5 * Q / 4) + 2 * MAX for a quota build, (K * 1024 + GCSTEPSIZE) /
 * bound_cost + 2 * MAX for the others; over_bound0 counts the paced steps
 * above it.  Steps the variant ran as pristine steps (budget -1: every
 * step of a latched state) are counted apart ("pristine steps") with their
 * own max.  Steps from a refusal (cap_kb) until the next
 * atomic are not judged: the emergency collection leaves its batch in
 * tobefnz outside the cycle's sweep/callfin states, where lgc.c:848 keeps
 * the pristine doubling on purpose; they are reported as "emergency steps".
 *
 * Coverage of the derived lines is reported from observed state: gcfinnum
 * doubling (<MAX), saturating at MAX, reset to 0 when tobefnz drains,
 * finalizers seen in GCScallfin vs in another state (the tail
 * runafewfinalizers of luaC_step, lgc.c:1142), steps that ran finalizers at
 * a charge other than the pristine 5 (quota) and allocation-driven steps
 * that ran finalizers (alloc_fin: the lgc.c:1141 credit clamp at work; with
 * the rev0 constant charge almost every finalizer runs in the paced step).
 *
 * Sweep (ISAAC_VITA_LUA_GC_SWEEP_STEP, lgc.c:39): a step that starts in a
 * sweep state (GCSswpallgc..GCSswptobefnz) books its non-finalizer units as
 * swept objects, units / sweep charge (sweepstep charges GCSWEEPMAX * cost
 * for GCSWEEPMAX objects, lgc.c:1026-1038); the steps per cycle that start
 * in a sweep state show the sweep line is live.
 *
 * Latch (ORACLE_LATCH, the latch derivation): "latch scripted=S frame=F ..."
 * reports whether the state was latched (isaac_lua_gc_scripted, the derived
 * lgc.c:61 itself) and from which loop, the latch=S,B control's hand-over
 * (the loop it fired in, the GC state and tobefnz length there, the loops
 * until tobefnz drained, the peak heap and the worst projected loop after
 * it) and whether lua_close forgot the state (lgc.c:975).  "oracle helpers"
 * checks the derived line-60 schedule helpers (the late rule's V) and the
 * line-61 latch at their boundaries on synthetic global_States -- see run_helpers -- and
 * prints "helpers checks=N bad=M" (exit 1 if M > 0); a build without the
 * latch prints "helpers: not a latch build" and exits 2.
 *
 * Device projection: per luaC_step, work units = budget minus the debt left
 * (a step that ends the cycle: the finalizer charge when it started in
 * GCSswpend or GCScallfin, otherwise the budget), less the nominal
 * strt.size * sizeof(GCObject *) a step out of GCSpause books
 * (lgc.c:1046) without traversing anything, less the charged finalizer
 * work.  Marking units cost NS_PER_UNIT, swept objects SWEEP_US each,
 * finalizers FIN_US each.  A step that crosses from propagate or atomic into
 * the sweep is booked as marking (one step per cycle, so the sweep share of
 * that step is under-costed).  A loop is every step it ran; a full
 * collection (collectgarbage("collect"), collect=N) does not go through
 * luaC_step and is not projected, in any build.  Constants (device-measured):
 * 8.93 ns per work unit = 1170 us per 128 KB step (P2b cost profile fit),
 * 0.40 us per swept object (GC profile: menu sweep steps 2.9 ms per ~10,240
 * objects, in-game 11-15 ms steps without finalizers per ~26,000), 3.5 us
 * per LuaBridge __gc (device burst windows, max_us ~ 6,972 +
 * 3.48 * fin_step over 905 windows; fin_us=10 reproduces the earlier c10
 * column).  Only counts are computed here; no host time is used. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lstate.h"
#include "lgc.h"

#if !defined(ORACLE_FIN_MAX)
#define ORACLE_FIN_MAX 0 /* pristine build: no cap */
#endif
/* ORACLE_QUOTA=1: the variant has the derived line 44 (isaac_pace_budget and
 * the quota charge); ORACLE_FIN_STEP / ORACLE_SWEEP_STEP are the quotas it
 * was derived with (lines 45 and 39; a sweep quota of 0 leaves line 39
 * pristine), used for bound0 and the report only -- every charge comes from
 * the variant's own macros (oracle_lgc_fin_cost / oracle_lgc_sweep_cost).
 * ORACLE_LATCH=1: the variant has the script latch (line 61) and the
 * oracle checks its helpers. */
#if !defined(ORACLE_QUOTA)
#define ORACLE_QUOTA 0
#endif
#if !defined(ORACLE_LATCH)
#define ORACLE_LATCH 0
#endif
#if !defined(ORACLE_FIN_STEP)
#define ORACLE_FIN_STEP 0
#endif
#if !defined(ORACLE_SWEEP_STEP)
#define ORACLE_SWEEP_STEP 0
#endif
#if ORACLE_QUOTA && (ORACLE_FIN_STEP <= 0 || ORACLE_FIN_MAX <= 0)
#error "ORACLE_QUOTA needs ORACLE_FIN_STEP and ORACLE_FIN_MAX"
#endif
#if ORACLE_LATCH && !ORACLE_QUOTA
#error "ORACLE_LATCH needs ORACLE_QUOTA"
#endif
#define ORACLE_STEPMULADJ 200 /* lgc.c:52 STEPMULADJ */
#define PRISTINE_COST ((l_mem)((sizeof(TString) + 4) / 4)) /* lgc.c:39 */

/* vita_lua_gc_finalizer_pace_oracle_lgc.c */
l_mem oracle_lgc_budget(void);
void oracle_lgc_step_begin(void);
l_mem oracle_lgc_step_budget(void);
lu_mem oracle_lgc_fin_cost(l_mem budget);
lu_mem oracle_lgc_sweep_cost(l_mem budget);
unsigned int oracle_lgc_alloc_latches(void);
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
/* the derived lgc.c:61 itself (LUAI_FUNC, external) */
int isaac_lua_gc_scripted(const global_State *g);
void isaac_lua_gc_script_control(lua_State *L, int what);
#endif

#define RING 64
#define WEAK_ROTATE 256
#define F_FINALIZED 1u
#define F_RING 2u
#define F_GC 4u    /* its __gc metatable is set: a raise inside lua_newuserdata
                     * (err=P) leaves an id that was never finalizable */

typedef struct { int id; int slot; } Box;

static int *s_created_frame;
static unsigned char *s_flags;
static unsigned short *s_weak_gen; /* weak-table generation an id joined */
static long s_ids_cap;
static unsigned short s_gen;
static int s_closing;
static int s_bound_cost = 512, s_bound_max = ORACLE_FIN_MAX > 0 ? ORACLE_FIN_MAX : 8;
static long s_next_id;
static int s_frame;
static int s_ring_ref, s_wv_ref, s_wk_ref;
static long s_fin_total;

static long s_early, s_weak, s_order, s_twice, s_finalized;
static long s_batch_last = -1;
static long s_batches;
static long s_cov_double, s_cov_cap, s_cov_drain, s_cov_callfin, s_cov_tail;
static long s_cov_quota, s_cov_alloc_fin;
static long s_err_raised;
static int s_fin_alloc;         /* fin_alloc=N: tables each __gc allocates */
static double s_lag_sum;
static long s_lag_n, s_lag_max;

/* luaC_step wrapper accounting */
static int s_measure;           /* 1 inside the loop */
static int s_in_explicit;       /* 1 inside the paced lua_gc(LUA_GCSTEP) */
static long s_steps, s_steps_paced, s_bound_bad, s_behind;
static long s_cycles;            /* measured steps that reached GCSpause */
static double s_frame_units;    /* marking units of all steps this loop */
static double s_frame_swept;    /* objects swept by all steps this loop */
static long s_frame_fins;       /* finalizers of all steps this loop */
static long s_frame_steps;      /* luaC_steps this loop */
static long s_max_step_fin, s_max_explicit_fin, s_explicit_over_bound0;

/* seed=N: the exact builds' string-hash seed and the per-loop state digest
 * (FNV-1a, 64 bits) */
static unsigned int s_seed;
static int s_seed_set;
static unsigned long long s_digest = 14695981039346656037ULL;

unsigned int oracle_seed(void);
unsigned int oracle_seed(void) { return s_seed; }

static void digest_add(long long v)
{
    int i;
    for (i = 0; i < 8; ++i) {
        s_digest ^= (unsigned long long)((v >> (8 * i)) & 0xff);
        s_digest *= 1099511628211ULL;
    }
}
static long s_over_bound0;     /* paced steps above bound0 */
static long s_fin_explicit, s_fin_other;
static long s_bound0;           /* quota: 5Q/4 + 2MAX, else (K*1024+GCSTEPSIZE)/COST + 2MAX */
static long s_sweep_steps;      /* steps that start in a sweep state */
static double s_max_step_swept, s_swept_total;
static double s_ns_per_unit = 8.93;
static double s_sweep_us = 0.40;
static double s_fin_us = 3.5;

/* ORACLE cap: the Lua heap may hold at most cap bytes (no fallback) */
static size_t s_cap_bytes, s_used_bytes;
static long s_alloc_fail;
/* A refusal makes Lua run an emergency collection (lmem.c luaM_realloc_ ->
 * luaC_fullgc(L, 1)): it finishes the running cycle and runs one more, and
 * each atomic appends its batch behind whatever tobefnz still holds
 * (separatetobefnz finds the list's end; emergency mode runs no
 * finalizer), so ids may rise twice per refusal; s_order beyond
 * s_order_slack is a violation. */
static long s_order_slack;
/* 1 from a refusal until a step starts past the next atomic: tobefnz may
 * hold the emergency's batch outside GCSswpallgc..GCScallfin, where the
 * derived lgc.c:848 keeps the pristine doubling; such steps are counted
 * (emergency_steps) and kept out of the pacing bound. */
static int s_emergency;
static long s_emergency_steps, s_max_step_fin_emergency;
/* steps the variant ran as pristine steps (its lgc.c:1133 set budget -1:
 * a latched state); kept out of the pacing bound, counted with their own
 * max */
static long s_pristine_steps, s_max_step_fin_pristine;
/* paced steps whose lgc.c:1133 budget is not getdebt(g) on entry, or that
 * ended without a luaE_setdebt of lgc.c: the record or the variant is
 * broken (or a step nested inside a step) */
static long s_budget_mismatch;
static size_t s_abort_bytes;    /* abort_kb: stop a run whose heap passes it */

/* latch=S,B hand-over (any build: pristine runs the same control call) */
static int s_latch_frame = -1, s_latch_state = -1;
static long s_latch_backlog = -1;
static int s_scripted_frame = -1;  /* ORACLE_LATCH: first loop latched */
static long s_latched_steps;        /* ORACLE_LATCH: steps of a latched state */

/* fin_str=N: each C __gc makes one N-byte string (a long string above 40
 * bytes: a fresh allocation every call).  err=P: the __gc of every P-th
 * object raises "boom"; the loop's work and the engine's step then run
 * inside lua_pcall (both protected), hostpush=N adds N
 * unprotected 16-byte allocations per loop ahead of them (the host's own
 * pushes): a raising __gc that runs in one of those panics, in any build. */
static int s_fin_str;
static char *s_fin_str_buf;
static long s_err_every, s_errs_frame, s_errs_step;
static int s_hostpush;

/* The charges lgc.c applies, inside a luaC_step whose budget was `budget`,
 * to one finalizer (line 45) and to one swept object (line 39): the
 * variant's own macros. */
static l_mem oracle_fin_cost(l_mem budget)
{
    return (l_mem)oracle_lgc_fin_cost(budget);
}

static l_mem oracle_sweep_cost(l_mem budget)
{
    return (l_mem)oracle_lgc_sweep_cost(budget);
}

static int gc_cb(lua_State *L)
{
    Box *b = (Box *)lua_touserdata(L, 1);
    global_State *g = G(L);
    long id = b->id;
    int i;

    if (s_flags[id] & F_FINALIZED)
        ++s_twice;
    s_flags[id] |= F_FINALIZED;
    ++s_finalized;
    ++s_fin_total;
    if (g->gcstate == GCScallfin)
        ++s_cov_callfin;
    else
        ++s_cov_tail;
    if (s_closing)
        return 0;   /* lua_close finalizes reachable objects by design */
    for (i = 0; i < s_fin_alloc; ++i) {   /* fin_alloc=N */
        lua_createtable(L, 2, 0);
        lua_pushfstring(L, "x%d", (int)id);
        lua_rawseti(L, -2, 1);
        lua_pop(L, 1);
    }
    if (s_fin_str > 0) {                   /* fin_str=N */
        memcpy(s_fin_str_buf, &id, sizeof id);
        lua_pushlstring(L, s_fin_str_buf, (size_t)s_fin_str);
        lua_pop(L, 1);
    }
    if (s_measure && !(s_flags[id] & F_RING)) {
        long lag = s_frame - s_created_frame[id];
        s_lag_sum += (double)lag;
        ++s_lag_n;
        if (lag > s_lag_max)
            s_lag_max = lag;
    }
    if (b->slot > 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, s_ring_ref);
        lua_rawgeti(L, -1, b->slot);
        if (lua_rawequal(L, -1, 1))
            ++s_early;
        lua_pop(L, 2);
    }
    if (b->slot != -3 && s_weak_gen[id] == s_gen) { /* still the current table */
        lua_rawgeti(L, LUA_REGISTRYINDEX, s_wv_ref);
        if (lua_rawgeti(L, -1, (lua_Integer)id) != LUA_TNIL)
            ++s_weak;
        lua_pop(L, 2);
    }
    if (s_batch_last >= 0 && id >= s_batch_last)
        ++s_order;
    s_batch_last = id;
    if (g->tobefnz == NULL)   /* udata2finalize already popped this one */
        s_batch_last = -1, ++s_batches;
    if (s_err_every && id % s_err_every == 0) {   /* err=P */
        ++s_err_raised;
        lua_pushstring(L, "boom");
        return lua_error(L);
    }
    return 0;
}

static int gc_err_cb(lua_State *L)
{
    Box *b = (Box *)lua_touserdata(L, 1);
    gc_cb(L);
    if (b->slot == -2) {
        ++s_err_raised;
        lua_pushstring(L, "boom");
        return lua_error(L);
    }
    return 0;
}

static void observe_finnum(lua_State *L)
{
    global_State *g = G(L);
    unsigned n = g->gcfinnum;
    if (g->tobefnz) {
        if (ORACLE_FIN_MAX && n == (unsigned)ORACLE_FIN_MAX)
            ++s_cov_cap;
        else if (n > 1u && (!ORACLE_FIN_MAX || n < (unsigned)ORACLE_FIN_MAX))
            ++s_cov_double;
    } else if (n == 0u) {
        ++s_cov_drain;
    }
}

void oracle_real_luaC_step(lua_State *L); /* lgc.c:1126, renamed */

void luaC_step(lua_State *L)
{
    global_State *g = G(L);
    l_mem debt = g->GCdebt, budget, used, cost, bound_cost, sweep_cost;
    long fin_before = s_fin_total, fins, fail_before = s_alloc_fail;
    int from = g->gcstate, emergency, pristine_step;
    double units, swept = 0.0;
    int sweeping = from == GCSswpallgc || from == GCSswpfinobj ||
                   from == GCSswptobefnz;
    l_mem strt_nominal = (l_mem)g->strt.size * (l_mem)sizeof(GCObject *);

    if (!g->gcrunning) { /* lgc.c:1129-1132: no work */
        oracle_real_luaC_step(L);
        return;
    }
    /* lgc.c:1112-1121 getdebt */
    budget = debt > 0 ? (debt / ORACLE_STEPMULADJ + 1) * g->gcstepmul : 0;
    if (from >= GCSswpallgc && from <= GCScallfin)
        s_emergency = 0;   /* an atomic ran since the last refusal */
    emergency = s_emergency;
    oracle_lgc_step_begin();
    oracle_real_luaC_step(L);   /* a __gc error longjmps past the rest */
    /* what the variant's lgc.c:1133 chose for this step, as recorded at the
     * step's last luaE_setdebt of lgc.c */
    used = oracle_lgc_step_budget();
    pristine_step = ORACLE_QUOTA && used == -1;
    if (ORACLE_QUOTA && !pristine_step && used != budget)
        ++s_budget_mismatch;
#if ORACLE_LATCH
    if (isaac_lua_gc_scripted(g))
        ++s_latched_steps;
#endif
    cost = oracle_fin_cost(pristine_step ? -1 : budget);
    bound_cost = ORACLE_QUOTA ? cost : s_bound_cost;
    fins = s_fin_total - fin_before;
    observe_finnum(L);
    ++s_steps;
    if (s_measure && from != GCSpause && g->gcstate == GCSpause)
        ++s_cycles;
    if (fins > 0) {
        if (cost != PRISTINE_COST)
            ++s_cov_quota;      /* the line-45 charge is not the pristine 5 */
        if (!s_in_explicit)
            ++s_cov_alloc_fin;  /* an allocation-driven step drained some */
    }
    if (emergency || s_alloc_fail != fail_before) {
        ++s_emergency_steps;
        if (fins > s_max_step_fin_emergency)
            s_max_step_fin_emergency = fins;
    } else if (pristine_step) {
        ++s_pristine_steps;
        if (fins > s_max_step_fin_pristine)
            s_max_step_fin_pristine = fins;
    } else {
        if (fins > s_max_step_fin)
            s_max_step_fin = fins;
        if (fins > (budget + GCSTEPSIZE) / bound_cost + 2 * s_bound_max + 1)
            ++s_bound_bad;
        if (fins > s_bound0)
            ++s_over_bound0;
    }
    if (g->gcstate == GCSpause && g->GCdebt > 0)
        ++s_behind;   /* setpause left a debt: the next cycle has no pause */
    if (s_in_explicit) {
        s_fin_explicit += fins;
        if (fins > s_max_explicit_fin)
            s_max_explicit_fin = fins;
        if (fins > s_bound0)
            ++s_explicit_over_bound0;
    } else {
        s_fin_other += fins;
    }
    if (!s_measure)
        return;
    if (sweeping)
        ++s_sweep_steps;
    /* A step that ends the cycle stops at GCSpause without spending its
     * budget; from GCSswpend or GCScallfin its only work was finalizers
     * (lgc.c:1077-1091: swpend charges 0, callfin n * GCFINALIZECOST). */
    if (g->gcstate != GCSpause)
        units = (double)(budget - g->GCdebt);
    else if (from == GCSswpend || from == GCScallfin)
        units = (double)fins * (double)cost;
    else
        units = (double)budget;
    if (from == GCSpause)
        units -= (double)strt_nominal;
    units -= (double)fins * (double)cost;
    if (units < 0)
        units = 0;
    if (sweeping) {
        sweep_cost = oracle_sweep_cost(pristine_step ? -1 : budget);
        swept = units / (double)sweep_cost;
        units = 0;
        s_swept_total += swept;
        if (swept > s_max_step_swept)
            s_max_step_swept = swept;
    }
    s_frame_units += units;
    s_frame_swept += swept;
    s_frame_fins += fins;
    ++s_frame_steps;
}

static void *cap_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    size_t old = ptr ? osize : 0;
    (void)ud;
    if (nsize == 0) {
        free(ptr);
        s_used_bytes -= old;
        return NULL;
    }
    if (s_cap_bytes && nsize > old &&
        s_used_bytes - old + nsize > s_cap_bytes) {
        ++s_alloc_fail;
        s_order_slack += 2;
        s_emergency = 1;
        return NULL;
    }
    ptr = realloc(ptr, nsize);
    if (ptr)
        s_used_bytes = s_used_bytes - old + nsize;
    return ptr;
}

static int cap_panic(lua_State *L)
{
    printf("PANIC frame=%d: %s alloc_fail=%ld used_kb=%lu fin=%ld\n", s_frame,
           lua_tostring(L, -1), s_alloc_fail,
           (unsigned long)(s_used_bytes >> 10), s_fin_total);
    fflush(stdout);
    exit(3);
}

static const char *s_setup =
    "local live_kb = ...\n"
    "keep = {}\n"
    "local i, chunk = 0, nil\n"
    "while collectgarbage('count') < live_kb do\n"
    "  if i % 256 == 0 then chunk = {}; keep[#keep + 1] = chunk end\n"
    "  i = i + 1\n"
    "  chunk[i % 256 + 1] = {x = i, y = i * 2, name = 'e' .. i, sub = {i, i + 1}}\n"
    "end\n";

/* Control scripts (ctl=NAME[:A[,B]]): each chunk receives A and B and
 * returns its pre(f) and post(f) loop hooks (either may be nil); code
 * outside them runs once, after the setup's full collection.  Every
 * collectgarbage() call here goes through the base library's
 * luaB_collectgarbage, like a mod's.  gcstate() is g->gcstate (lgc.h: 0
 * propagate .. 7 pause), backlog() the length of tobefnz, latchmark()
 * records the hand-over point. */
typedef struct { const char *name; const char *chunk; } Ctl;
static const Ctl s_ctls[] = {
    /* stop the collector, retune or step it */
    {"stop", "collectgarbage('stop') return nil, nil"},
    {"stepmul", "local A = ... collectgarbage('setstepmul', A) return nil, nil"},
    {"setpause", "local A = ... collectgarbage('setpause', A) return nil, nil"},
    {"luasteps", "local A = ... return nil, function(f) "
                 "for s = 1, A do collectgarbage('step') end end"},
    /* restart it */
    {"restart", "local A = ... return function(f) "
                "if f % A == 0 then collectgarbage('restart') end end, nil"},
    {"stoprestart", "return function(f) collectgarbage('stop') end, "
                    "function(f) collectgarbage('restart') end"},
    {"restartstop", "return function(f) collectgarbage('restart') "
                    "collectgarbage('stop') end, nil"},
    /* restart or step in a chosen GC state; count-gated control */
    {"restartstate", "local A = ... local done = false return function(f) "
                     "local s = gcstate() if s == 7 or s == 0 then done = false "
                     "elseif s == A and not done then collectgarbage('restart') "
                     "done = true end end, nil"},
    {"step0state", "local A = ... local done = false return function(f) "
                   "local s = gcstate() if s == 7 or s == 0 then done = false "
                   "elseif s == A and not done then collectgarbage('step') "
                   "done = true end end, nil"},
    {"countstep", "local A = ... return nil, function(f) "
                  "if collectgarbage('count') > A then latchmark() collectgarbage('step') end end"},
    {"countcollect", "local A = ... return nil, function(f) "
                     "if collectgarbage('count') > A then latchmark() collectgarbage('collect') end end"},
    {"collect", "local A = ... return nil, function(f) "
                "if f % A == A - 1 then collectgarbage('collect') end end"},
    {"hyst", "local A = ... return function(f) local c = collectgarbage('count') "
             "if c > A then collectgarbage('restart') "
             "elseif c < A - 300 then collectgarbage('stop') end end, nil"},
    /* setstepmul / setpause toggled per loop or per period */
    {"smtoggle", "local A = ... return function(f) "
                 "collectgarbage('setstepmul', (f % 2 == 0) and 40 or A) end, nil"},
    {"smper", "local A, B = ... return function(f) if f % B == 0 then "
              "collectgarbage('setstepmul', ((f // B) % 2 == 0) and A or 200) end end, nil"},
    {"pausetoggle", "local A = ... return function(f) "
                    "collectgarbage('setpause', (f % 2 == 0) and A or 200) end, nil"},
    {"pauseslow", "local A = ... return function(f) "
                  "collectgarbage('setpause', (f % 600 < 300) and A or 100) end, nil"},
    {"pauseper", "local A, B = ... return function(f) if f % B == 0 then "
                 "collectgarbage('setpause', ((f // B) % 2 == 0) and A or 200) end end, nil"},
    /* the options the latch must NOT see as control */
    {"count", "return nil, function(f) local c = collectgarbage('count') end"},
    {"isrunning", "return nil, function(f) local r = collectgarbage('isrunning') end"},
    /* latch hand-over: a no-op setpause 200 (the default) at the first loop
     * start in GC state A with at least B objects in tobefnz */
    {"latch", "local A, B = ... local done = false return function(f) "
              "if not done and gcstate() == A and backlog() >= B then "
              "latchmark() collectgarbage('setpause', 200) done = true end end, nil"},
    /* the same, polled after each of up to 64 small allocations per loop
     * start (each a luaC_checkGC that may step): a script observes states
     * a loop boundary rarely shows, such as GCSatomic after the
     * propagatemark that emptied the gray list (GCSswpend is never left
     * at the end of a step: entering it returns no work) */
    {"latchpoll", "local A, B = ... local done = false return function(f) "
                  "if done then return end for i = 1, 64 do local t = {i} "
                  "if gcstate() == A and backlog() >= B then latchmark() "
                  "collectgarbage('setpause', 200) done = true return end end end, nil"},
};
#define MAX_CTL 4

typedef struct {
    int k, live_kb, fin, garb, frames, lps;
    int burst_p, burst_l, burst_n;
    int spike_f, spike_n;
    int trace, collect, birth;
    int nctl;
    const Ctl *ctl[MAX_CTL];
    int ctl_a[MAX_CTL], ctl_b[MAX_CTL];
} Workload;

static int s_pre_ref[MAX_CTL], s_post_ref[MAX_CTL];

static long tobefnz_length(lua_State *L)
{
    long n = 0;
    GCObject *o;
    for (o = G(L)->tobefnz; o; o = o->next)
        ++n;
    return n;
}

static int l_gcstate(lua_State *L)
{
    lua_pushinteger(L, G(L)->gcstate);
    return 1;
}

static int l_backlog(lua_State *L)
{
    lua_pushinteger(L, tobefnz_length(L));
    return 1;
}

static int l_latchmark(lua_State *L)
{
    if (s_latch_frame < 0) {
        s_latch_frame = s_frame;
        s_latch_state = G(L)->gcstate;
        s_latch_backlog = tobefnz_length(L);
    }
    return 0;
}

static void new_weak_tables(lua_State *L)
{
    int wv, wk;
    lua_newtable(L);
    lua_newtable(L);
    lua_pushliteral(L, "v");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    wv = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L);
    lua_newtable(L);
    lua_pushliteral(L, "k");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    wk = luaL_ref(L, LUA_REGISTRYINDEX);
    s_wv_ref = wv;   /* no allocation from here to the caller's ++s_gen */
    s_wk_ref = wk;
}

static lua_State *new_world(int live_kb, int birth)
{
    lua_State *L = lua_newstate(cap_alloc, NULL);
    if (!L) {
        fprintf(stderr, "lua_newstate failed\n");
        exit(2);
    }
#if ORACLE_LATCH
    if (birth)  /* latched before any library or object exists */
        isaac_lua_gc_script_control(L, LUA_GCSTOP);
#else
    (void)birth;
#endif
    lua_atpanic(L, cap_panic);
    if (s_seed_set) {
        /* exact: every library but package, whose registry key is the
         * light userdata &CLIBS (loadlib.c, lua_rawsetp) -- a static
         * address that differs between two links, so it moves the
         * registry's hash layout and with it the marking order */
        static const luaL_Reg libs[] = {
            {"_G", luaopen_base}, {LUA_COLIBNAME, luaopen_coroutine},
            {LUA_TABLIBNAME, luaopen_table}, {LUA_IOLIBNAME, luaopen_io},
            {LUA_OSLIBNAME, luaopen_os}, {LUA_STRLIBNAME, luaopen_string},
            {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8},
            {LUA_DBLIBNAME, luaopen_debug}, {NULL, NULL}};
        const luaL_Reg *lib;
        for (lib = libs; lib->func; ++lib) {
            luaL_requiref(L, lib->name, lib->func, 1);
            lua_pop(L, 1);
        }
    } else {
        luaL_openlibs(L);
    }
    luaL_newmetatable(L, "fin");
    lua_pushcfunction(L, gc_cb);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    luaL_newmetatable(L, "finerr");
    lua_pushcfunction(L, gc_err_cb);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    lua_register(L, "gcstate", l_gcstate);
    lua_register(L, "backlog", l_backlog);
    lua_register(L, "latchmark", l_latchmark);
    lua_newtable(L);
    s_ring_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    new_weak_tables(L);
    if (luaL_loadstring(L, s_setup) != LUA_OK) {
        fprintf(stderr, "%s\n", lua_tostring(L, -1));
        exit(2);
    }
    lua_pushinteger(L, live_kb);
    lua_call(L, 1, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);   /* the engine's, from C: no latch */
    return L;
}

/* Load each control chunk, run its setup part and keep its hooks. */
static void load_ctls(lua_State *L, const Workload *w)
{
    int i;
    for (i = 0; i < w->nctl; ++i) {
        if (luaL_loadstring(L, w->ctl[i]->chunk) != LUA_OK) {
            fprintf(stderr, "ctl %s: %s\n", w->ctl[i]->name,
                    lua_tostring(L, -1));
            exit(2);
        }
        lua_pushinteger(L, w->ctl_a[i]);
        lua_pushinteger(L, w->ctl_b[i]);
        lua_call(L, 2, 2);
        s_post_ref[i] = luaL_ref(L, LUA_REGISTRYINDEX);  /* LUA_REFNIL for nil */
        s_pre_ref[i] = luaL_ref(L, LUA_REGISTRYINDEX);
    }
}

static void call_hooks(lua_State *L, const Workload *w, const int *refs,
                       int f)
{
    int i;
    for (i = 0; i < w->nctl; ++i) {
        if (refs[i] == LUA_REFNIL)
            continue;
        lua_rawgeti(L, LUA_REGISTRYINDEX, refs[i]);
        lua_pushinteger(L, f);
        lua_call(L, 1, 0);
    }
}

static void alloc_ids(long cap)
{
    s_ids_cap = cap;
    s_created_frame = (int *)malloc(sizeof(int) * (size_t)cap);
    s_flags = (unsigned char *)calloc((size_t)cap, 1);
    s_weak_gen = (unsigned short *)malloc(sizeof(unsigned short) * (size_t)cap);
    if (!s_created_frame || !s_flags || !s_weak_gen) {
        fprintf(stderr, "cannot allocate %ld ids\n", cap);
        exit(2);
    }
    memset(s_weak_gen, 0xff, sizeof(unsigned short) * (size_t)cap);
}

static long new_id(void)
{
    long id = s_next_id++;
    if (id >= s_ids_cap) {
        fprintf(stderr, "id space exhausted\n");
        exit(2);
    }
    return id;
}

static void one_frame(lua_State *L, int fin, int garb, int *ring_pos)
{
    int j;
    if (s_frame > 0 && s_frame % WEAK_ROTATE == 0) {
        /* the new tables first: a finalizer that their allocations run
         * still reads s_wv_ref, which must name a live table until the
         * swap (unreferencing first left an integer free-list link in that
         * registry slot, and gc_cb's lua_rawgeti on it faulted once
         * collectgarbage("restart") made such steps frequent) */
        int old_wv = s_wv_ref, old_wk = s_wk_ref;
        new_weak_tables(L);
        ++s_gen;
        luaL_unref(L, LUA_REGISTRYINDEX, old_wv);
        luaL_unref(L, LUA_REGISTRYINDEX, old_wk);
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_ring_ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_wv_ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_wk_ref);
    for (j = 0; j < fin; ++j) {
        long id = new_id();
        Box *b = (Box *)lua_newuserdata(L, sizeof *b);
        b->id = (int)id;
        b->slot = -1;
        luaL_setmetatable(L, "fin");
        s_flags[id] |= F_GC;
        s_created_frame[id] = s_frame;
        if (j % 4 == 0) {
            b->slot = (*ring_pos)++ % RING + 1;
            s_flags[id] |= F_RING;
            lua_pushvalue(L, -1);
            lua_rawseti(L, -5, b->slot);
        }
        if (j % 3 == 0) {
            lua_pushvalue(L, -1);
            lua_rawseti(L, -4, (lua_Integer)id);
            s_weak_gen[id] = s_gen;
        }
        if (j % 5 == 0) {
            lua_pushvalue(L, -1);
            lua_pushinteger(L, (lua_Integer)id);
            lua_rawset(L, -4);
        }
        lua_pop(L, 1);
    }
    for (j = 0; j < garb; ++j) {
        lua_createtable(L, 2, 0);
        lua_pushinteger(L, s_frame);
        lua_rawseti(L, -2, 1);
        lua_pushinteger(L, j);
        lua_rawseti(L, -2, 2);
        lua_pop(L, 1);
    }
    lua_pop(L, 3);
}

/* spike=F,N: N finalizable userdata die in one loop (plain garbage, no ring
 * or weak entry), like a room's worth of entity userdata */
static void spike_frame(lua_State *L, int n)
{
    int j;
    for (j = 0; j < n; ++j) {
        long id = new_id();
        Box *b = (Box *)lua_newuserdata(L, sizeof *b);
        b->id = (int)id;
        b->slot = -3;
        luaL_setmetatable(L, "fin");
        s_flags[id] |= F_GC;
        s_created_frame[id] = s_frame;
        lua_pop(L, 1);
    }
}

static const char *state_name(int s)
{
    switch (s) {
    case GCSpropagate: return "propagate";
    case GCSatomic: return "atomic";
    case GCSswpallgc: return "swpallgc";
    case GCSswpfinobj: return "swpfinobj";
    case GCSswptobefnz: return "swptobefnz";
    case GCSswpend: return "swpend";
    case GCScallfin: return "callfin";
    case GCSpause: return "pause";
    default: return "-";
    }
}

/* err=P: the loop's work inside lua_pcall (frame_protected) and the
 * engine's step too (step_protected, below) */
static int s_frame_fin, s_frame_garb, *s_frame_ring;
static int step_protected(lua_State *L);

static int frame_protected(lua_State *L)
{
    one_frame(L, s_frame_fin, s_frame_garb, s_frame_ring);
    return 0;
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static int run_workload(const Workload *w)
{
    lua_State *L;
    const global_State *g_closed;
    int f, ring_pos = 0, peak_kb = 0, peak_first = 0, peak_second = 0;
    int last_kb = 0, drain = -1, post_peak_kb = 0, forgotten = -1;
    long cycles;
    long long sum_kb = 0;
    long keys_bad = 0, max_frame_fin = 0, never = 0, i, tob_max = 0;
    long over_2ms = 0, pending_age_max = 0;
    double dev_sum = 0.0, dev_max = 0.0, dev10_max = 0.0;
    double post_ms_max = 0.0, pre_ms_max = 0.0;
    double mark_units_sum = 0.0;
    double worst_units = 0.0, worst_swept = 0.0;
    long worst_steps = 0, worst_fins = 0;
    int worst_f = -1;
    float *frame_ms;
    long ids;
    int fin_peak = w->fin > w->burst_n ? w->fin : w->burst_n;

    ids = (long)w->frames * fin_peak + w->spike_n + 16;
    alloc_ids(ids);
    frame_ms = (float *)malloc(sizeof(float) * (size_t)w->frames);
    if (!frame_ms)
        return 2;
    s_next_id = 1;
    s_bound0 = ORACLE_QUOTA
                   ? (long)ORACLE_FIN_STEP * 5 / 4 + 2L * s_bound_max
                   : ((long)w->k * 1024 + GCSTEPSIZE) / s_bound_cost +
                         2L * s_bound_max;
    L = new_world(w->live_kb, w->birth);
    load_ctls(L, w);
    s_measure = 1;
    for (f = 0; f < w->frames; ++f) {
        int kb, fin_now = w->fin;
        double ms, ms10;
        s_frame = f;
        s_frame_units = 0;
        s_frame_swept = 0;
        s_frame_fins = 0;
        s_frame_steps = 0;
        if (w->burst_p > 0 && f % w->burst_p < w->burst_l)
            fin_now = w->burst_n;
        for (i = 0; i < s_hostpush; ++i) {   /* hostpush=N: unprotected */
            (void)lua_newuserdata(L, 16);
            lua_pop(L, 1);
        }
        call_hooks(L, w, s_pre_ref, f);
        if (s_err_every) {
            s_frame_fin = fin_now, s_frame_garb = w->garb;
            s_frame_ring = &ring_pos;
            lua_pushcfunction(L, frame_protected);
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                ++s_errs_frame;
                lua_pop(L, 1);
            }
        } else {
            one_frame(L, fin_now, w->garb, &ring_pos);
        }
        if (w->spike_n && f == w->spike_f)
            spike_frame(L, w->spike_n);
        call_hooks(L, w, s_post_ref, f);
        if (w->collect > 0 && f % w->collect == w->collect - 1)
            (void)lua_gc(L, LUA_GCCOLLECT, 0); /* the engine's: no latch */
        s_in_explicit = 1;
        if (s_err_every) {
            lua_pushcfunction(L, step_protected);
            lua_pushinteger(L, w->k);
            if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                ++s_errs_step;
                lua_pop(L, 1);
            }
        } else {
            (void)lua_gc(L, LUA_GCSTEP, w->k);
        }
        s_in_explicit = 0;
        ++s_steps_paced;
#if ORACLE_LATCH
        if (s_scripted_frame < 0 && isaac_lua_gc_scripted(G(L)))
            s_scripted_frame = f;
#endif
        if (s_frame_fins > max_frame_fin)
            max_frame_fin = s_frame_fins;
        mark_units_sum += s_frame_units;
        ms = s_frame_units * s_ns_per_unit * 1e-6 +
             s_frame_swept * s_sweep_us * 1e-3 +
             (double)s_frame_fins * s_fin_us * 1e-3;
        ms10 = s_frame_units * s_ns_per_unit * 1e-6 +
               s_frame_swept * s_sweep_us * 1e-3 +
               (double)s_frame_fins * 10.0 * 1e-3;
        frame_ms[f] = (float)ms;
        dev_sum += ms;
        if (ms > dev_max) {
            dev_max = ms;
            worst_f = f, worst_steps = s_frame_steps, worst_fins = s_frame_fins;
            worst_units = s_frame_units, worst_swept = s_frame_swept;
        }
        if (ms10 > dev10_max)
            dev10_max = ms10;
        over_2ms += ms > 2.0;
        kb = lua_gc(L, LUA_GCCOUNT, 0);   /* the engine's: no latch */
        last_kb = kb;
        if (s_seed_set) {
            global_State *g = G(L);
            digest_add(f);
            digest_add(g->gcstate);
            digest_add((long long)gettotalbytes(g));
            digest_add((long long)g->GCdebt);
            digest_add((long long)g->GCestimate);
            digest_add((long long)g->gcfinnum);
            digest_add(tobefnz_length(L));
            digest_add(s_frame_fins);
            digest_add(s_frame_steps);
        }
        if (s_abort_bytes && ((size_t)kb << 10) > s_abort_bytes) {
            printf("ABORT frame=%d kb=%d over abort_kb=%lu tobefnz=%ld\n", f, kb,
                   (unsigned long)(s_abort_bytes >> 10), tobefnz_length(L));
            fflush(stdout);
            exit(4);
        }
        if (kb > peak_kb)
            peak_kb = kb;
        if (f < w->frames / 2) {
            if (kb > peak_first)
                peak_first = kb;
        } else if (kb > peak_second) {
            peak_second = kb;
        }
        sum_kb += kb;
        if (s_latch_frame >= 0) {
            if (kb > post_peak_kb)
                post_peak_kb = kb;
            if (ms > post_ms_max)
                post_ms_max = ms;
            if (drain < 0 && G(L)->tobefnz == NULL)
                drain = f - s_latch_frame;
        } else if (ms > pre_ms_max) {
            pre_ms_max = ms;
        }
        if (f % 600 == 599) {
            long t = tobefnz_length(L);
            if (t > tob_max)
                tob_max = t;
        }
        if (w->trace && f % w->trace == w->trace - 1) {
            printf("  trace t=%ds kb=%d peak=%d tobefnz=%ld state=%s "
                   "alloc_fail=%ld lag_max=%ld\n",
                   (f + 1) / w->lps, kb, peak_kb, tobefnz_length(L),
                   state_name(G(L)->gcstate), s_alloc_fail, s_lag_max);
            fflush(stdout);
        }
    }
    s_measure = 0;
    s_err_every = 0;    /* the closing collections below run unprotected */
    cycles = s_cycles;
    for (i = 1; i < s_next_id; ++i)   /* dead and still waiting: lag so far */
        if ((s_flags[i] & (F_FINALIZED | F_RING | F_GC)) == F_GC &&
            w->frames - 1 - s_created_frame[i] > pending_age_max)
            pending_age_max = w->frames - 1 - s_created_frame[i];
    /* keys: two full collections, then no weak key may be a finalized id */
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_wk_ref);
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        lua_Integer id = lua_tointeger(L, -1);
        if (id > 0 && id < s_next_id && (s_flags[id] & F_FINALIZED))
            ++keys_bad;
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    s_closing = 1;
    g_closed = G(L);
    lua_close(L);
#if ORACLE_LATCH
    /* lgc.c:975: a closed state leaves the latch table (its address may
     * come back as a new state's) */
    forgotten = !isaac_lua_gc_scripted(g_closed);
#else
    (void)g_closed;
#endif
    for (i = 1; i < s_next_id; ++i)
        if ((s_flags[i] & (F_FINALIZED | F_GC)) == F_GC)
            ++never;
    qsort(frame_ms, (size_t)w->frames, sizeof frame_ms[0], cmp_float);
    printf("K=%d live=%d fin=%d garb=%d frames=%d created=%ld finalized=%ld "
           "cycles=%ld batches=%ld per_cycle_frames=%.1f peak_kb=%d "
           "mean_kb=%lld last_kb=%d peak_first_kb=%d peak_second_kb=%d "
           "steps=%ld paced=%ld max_step_fin=%ld max_frame_fin=%ld "
           "max_explicit_fin=%ld explicit_over_bound0=%ld bound0=%ld "
           "behind=%ld lag_mean=%.1f lag_max=%ld pending_age_max=%ld "
           "tobefnz_max=%ld alloc_fail=%ld\n",
           w->k, w->live_kb, w->fin, w->garb, w->frames, s_next_id - 1,
           s_finalized, cycles, s_batches,
           cycles ? (double)w->frames / cycles : 0.0, peak_kb,
           sum_kb / w->frames, last_kb, peak_first, peak_second, s_steps,
           s_steps_paced, s_max_step_fin, max_frame_fin, s_max_explicit_fin,
           s_explicit_over_bound0, s_bound0, s_behind,
           s_lag_n ? s_lag_sum / s_lag_n : 0.0, s_lag_max, pending_age_max,
           tob_max, s_alloc_fail);
    printf("  violations early=%ld weak=%ld order=%ld twice=%ld never=%ld "
           "keys=%ld bound=%ld order_slack=%ld\n",
           s_early, s_weak, s_order, s_twice, never, keys_bad, s_bound_bad,
           s_order_slack);
    printf("  coverage double=%ld cap=%ld drain=%ld callfin=%ld tail=%ld "
           "quota=%ld alloc_fin=%ld over_bound0=%ld\n",
           s_cov_double, s_cov_cap, s_cov_drain, s_cov_callfin, s_cov_tail,
           s_cov_quota, s_cov_alloc_fin, s_over_bound0);
    printf("  emergency steps=%ld max_step_fin=%ld\n", s_emergency_steps,
           s_max_step_fin_emergency);
    printf("  pristine steps=%ld max_step_fin=%ld budget_mismatch=%ld\n",
           s_pristine_steps, s_max_step_fin_pristine, s_budget_mismatch);
    printf("  latch build=%d scripted=%d frame=%d steps=%ld forgotten=%d "
           "birth=%d mark=%d state=%s backlog=%ld drain=%d post_peak_kb=%d "
           "post_frame_ms_max=%.3f pre_frame_ms_max=%.3f\n",
           ORACLE_LATCH, s_scripted_frame >= 0, s_scripted_frame,
           s_latched_steps, forgotten, ORACLE_LATCH && w->birth,
           s_latch_frame, state_name(s_latch_state), s_latch_backlog, drain,
           post_peak_kb, post_ms_max, pre_ms_max);
    /* the derived lgc.c's own count of latches by the finalizer-allocation
     * budget (0 in a build without it) */
    printf("  guard alloc_latches=%u\n", oracle_lgc_alloc_latches());
    printf("  sweep step=%d steps=%ld per_cycle=%.1f max_step_objs=%.0f "
           "objs_per_cycle=%.0f\n",
           ORACLE_QUOTA ? ORACLE_SWEEP_STEP : 0, s_sweep_steps,
           cycles ? (double)s_sweep_steps / cycles : 0.0, s_max_step_swept,
           cycles ? s_swept_total / cycles : 0.0);
    printf("  split fin_explicit=%ld fin_other=%ld\n", s_fin_explicit,
           s_fin_other);
    printf("  gcerr raised=%ld frame_errors=%ld step_errors=%ld "
           "hostpush=%d fin_str=%d\n", s_err_raised, s_errs_frame,
           s_errs_step, s_hostpush, s_fin_str);
    if (s_seed_set)
        printf("  exact seed=%u digest=%016llx\n", s_seed, s_digest);
    printf("  device@%d/s fin_us=%.2f mark_units_per_frame=%.0f "
           "gc_ms_per_s=%.2f frame_ms_p50=%.3f frame_ms_p99=%.3f "
           "frame_ms_max=%.3f frame_ms_max_c10=%.3f frames_over_2ms=%ld "
           "cycle_s=%.2f\n",
           w->lps, s_fin_us, mark_units_sum / w->frames,
           dev_sum / w->frames * w->lps, frame_ms[w->frames / 2],
           frame_ms[(long)w->frames * 99 / 100], dev_max, dev10_max, over_2ms,
           cycles ? (double)w->frames / cycles / w->lps : 0.0);
    printf("  worst frame=%d steps=%ld mark_units=%.0f swept=%.0f fins=%ld\n",
           worst_f, worst_steps, worst_units, worst_swept, worst_fins);
    free(frame_ms);
    return (s_early || s_weak || s_order > s_order_slack || s_twice || never ||
            keys_bad)
               ? 1 : 0;
}

/* Errors inside finalizers during paced steps: every build must surface
 * each raising __gc as its own LUA_ERRGCMM "error in __gc metamethod
 * (boom)" to the protected caller and keep finalizing the rest afterwards
 * (GCTM propagateerrors=1 path, lgc.c:824-833): 16 of the 64 objects
 * raise. */
#define ERROR_CASE_RAISING 16
static int step_protected(lua_State *L)
{
    lua_gc(L, LUA_GCSTEP, (int)lua_tointeger(L, 1));
    return 0;
}

static int run_error_case(void)
{
    lua_State *L;
    int i, status, errors = 0, steps = 0;
    char msg[128] = "";
    alloc_ids(256);
    s_next_id = 1;
    L = new_world(64, 0);
    for (i = 0; i < 64; ++i) {
        Box *b = (Box *)lua_newuserdata(L, sizeof *b);
        b->id = (int)new_id();
        b->slot = (i % (64 / ERROR_CASE_RAISING) == 0) ? -2 : -1;
        luaL_setmetatable(L, "finerr");
        s_flags[b->id] |= F_GC;
        lua_pop(L, 1);
    }
    while (s_finalized < 64 && steps < 100000) {
        lua_pushcfunction(L, step_protected);
        lua_pushinteger(L, 16);
        status = lua_pcall(L, 1, 0, 0);
        ++steps;
        if (status != LUA_OK) {
            ++errors;
            snprintf(msg, sizeof msg, "%d:%s", status, lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_close(L);
    printf("error-case finalized=%ld raised=%ld errors=%d msg=%s "
           "violations order=%ld twice=%ld\n",
           s_finalized, s_err_raised, errors, msg, s_order, s_twice);
    return (s_finalized == 64 && s_err_raised == ERROR_CASE_RAISING &&
            errors == ERROR_CASE_RAISING &&
            strcmp(msg, "5:error in __gc metamethod (boom)") == 0 &&
            !s_order && !s_twice) ? 0 : 1;
}

#if ORACLE_LATCH
/* The derived line-60 schedule helpers and the line-61 latch at their
 * boundaries, on synthetic global_States that set only the fields the
 * helpers read (every oracle_lgc_* is the variant's own static, see
 * vita_lua_gc_finalizer_pace_oracle_lgc.c, so a mutated boundary fails here,
 * not only in a pin):
 *   schedule  the swept-object booking; open sets the GCScallfin offset to
 *             totalbytes - threshold + E; vend = GCdebt + offset; setdebt
 *             keeps V in GCScallfin (for a credit or a new debt) and leaves
 *             GCmemtrav alone elsewhere; the late rule only moves the pause
 *             earlier and clips to the total bytes; the quota charge's base,
 *             budget-0 and cap values;
 *   latch     "count", "countb" and "isrunning" never latch and leave the
 *             budget alone; each of stop, restart, collect, step, setpause
 *             and setstepmul latches and sets the budget to -1 (the call's
 *             own collection, if any, is a pristine one); a latched state
 *             has no late rule; latches are per state, idempotent, four
 *             slots, forget frees a slot, and a fifth latched state makes
 *             every state pristine. */
static int s_helper_checks, s_helper_bad;

static void helper_check(int ok, const char *what)
{
    ++s_helper_checks;
    if (!ok) {
        printf("helper FAIL %s\n", what);
        ++s_helper_bad;
    }
}

static int run_helpers(void)
{
    static global_State gs[7];
    static lua_State ls[7];
    static const int control[] = {LUA_GCSTOP, LUA_GCRESTART, LUA_GCCOLLECT,
                                  LUA_GCSTEP, LUA_GCSETPAUSE,
                                  LUA_GCSETSTEPMUL};
    static const char *const control_name[] = {"stop", "restart", "collect",
                                               "step", "setpause",
                                               "setstepmul"};
    static const int query[] = {LUA_GCCOUNT, LUA_GCCOUNTB, LUA_GCISRUNNING};
    static const char *const query_name[] = {"count", "countb", "isrunning"};
    global_State *g = &gs[0];
    char what[160];
    l_mem debt;
    int i;
    memset(gs, 0, sizeof gs);
    memset(ls, 0, sizeof ls);
    for (i = 0; i < 7; ++i) {
        gs[i].gcstepmul = 200;   /* = STEPMULADJ: E / stepmul * 200 = E */
        gs[i].gcpause = 200;
        gs[i].GCestimate = 100000;
        gs[i].totalbytes = 199600;
        gs[i].gcstate = GCSpause;
        ls[i].l_G = &gs[i];
    }
    helper_check(oracle_lgc_thr(g) == 200000, "thr = GCestimate / 100 * pause");
    /* the swept-object booking: returns n * c, adds n * (c - 5) to E
     * (n * c in swptobefnz) */
    g->gcstate = GCSswpallgc;
    g->GCmemtrav = 0;
    helper_check(oracle_lgc_swept(g, 100, 8) == 800 && g->GCmemtrav == 300,
                 "swept: 100 objects at 8 add 300 units");
    g->gcstate = GCSswptobefnz;
    helper_check(oracle_lgc_swept(g, 100, 8) == 800 && g->GCmemtrav == 1100,
                 "swept in swptobefnz adds the whole charge");
    helper_check(oracle_lgc_extra(g) == 1000,
                 "E converts as lgc.c:1140 (1100 / 200 * 200)");
    g->gcstepmul = 400;
    helper_check(oracle_lgc_extra(g) == 400,
                 "E converts as lgc.c:1140 (1100 / 400 * 200)");
    g->gcstepmul = 200;
    /* setdebt outside GCScallfin: GCmemtrav (E) untouched */
    debt = g->GCdebt;
    oracle_lgc_setdebt(g, -GCSTEPSIZE);
    helper_check(g->GCmemtrav == 1100 && g->GCdebt == -GCSTEPSIZE &&
                 debt != g->GCdebt, "setdebt in a sweep state keeps E");
    /* open: the GCScallfin offset = totalbytes - threshold + E */
    g->gcstate = GCSswpend;
    g->totalbytes = 190000;
    g->GCdebt = 0;
    g->GCmemtrav = 2400;
    g->gcstate = GCScallfin;
    oracle_lgc_open(g);
    helper_check(oracle_lgc_rd(g) == -7600,
                 "open: offset = 190000 - 200000 + 2400");
    helper_check(oracle_lgc_vend(g) == -7600, "vend = GCdebt + offset");
    g->GCdebt = 99;
    helper_check(oracle_lgc_vend(g) == -7501, "vend moves with the debt");
    /* the offset is signed and saturates */
    oracle_lgc_wr(g, -(long long)MAX_LMEM - 5);
    helper_check(oracle_lgc_rd(g) == -MAX_LMEM, "offset saturates below");
    oracle_lgc_wr(g, (long long)MAX_LMEM + 5);
    helper_check(oracle_lgc_rd(g) == MAX_LMEM, "offset saturates above");
    /* setdebt in GCScallfin keeps V = GCdebt + offset */
    g->GCdebt = -100;
    oracle_lgc_wr(g, 50);
    oracle_lgc_setdebt(g, -GCSTEPSIZE * 10);
    helper_check(oracle_lgc_vend(g) == -50 && g->GCdebt == -GCSTEPSIZE * 10,
                 "setdebt to a credit keeps V");
    oracle_lgc_setdebt(g, 300);
    helper_check(oracle_lgc_vend(g) == -50 && g->GCdebt == 300,
                 "setdebt to a debt keeps V");
    /* the late rule: setdebt(min(v, total)) only when v > GCdebt */
    g->gcstate = GCSpause;
    g->totalbytes = 1000;
    g->GCdebt = -500;
    oracle_lgc_late(g, 100);
    helper_check(g->GCdebt == 100 && gettotalbytes(g) == 500,
                 "late: the pause ends at the schedule");
    oracle_lgc_late(g, 100);
    oracle_lgc_late(g, 50);
    helper_check(g->GCdebt == 100, "late: never later than setpause");
    oracle_lgc_late(g, 100000);
    helper_check(g->GCdebt == 500 && gettotalbytes(g) == 500,
                 "late: clipped to the total bytes");
    /* the quota charge (line 44) */
    helper_check(oracle_lgc_cost(192, 1000, -1) == (lu_mem)PRISTINE_COST,
                 "budget -1 charges the pristine cost");
    helper_check(oracle_lgc_cost(192, 1000, 0) == (lu_mem)(GCSTEPSIZE / 192),
                 "budget 0 charges GCSTEPSIZE / quota");
    helper_check(oracle_lgc_cost(192, 1000, 192 * 1000) == 1000,
                 "the charge cap");
    helper_check(oracle_lgc_cost(192, 1000, 192) == (lu_mem)PRISTINE_COST + 4,
                 "the charge at budget 192 is (192 + 1600) / 192 = 9");
    /* the latch (line 61): queries never latch */
    for (i = 0; i < 3; ++i) {
        oracle_lgc_set_budget(77);
        isaac_lua_gc_script_control(&ls[0], query[i]);
        snprintf(what, sizeof what, "%s does not latch or touch the budget",
                 query_name[i]);
        helper_check(!isaac_lua_gc_scripted(&gs[0]) &&
                     oracle_lgc_budget() == 77, what);
    }
    /* every control option latches, its own collection runs pristine */
    for (i = 0; i < 6; ++i) {
        oracle_lgc_set_budget(77);
        isaac_lua_gc_script_control(&ls[0], control[i]);
        snprintf(what, sizeof what, "%s latches and sets the budget to -1",
                 control_name[i]);
        helper_check(isaac_lua_gc_scripted(&gs[0]) &&
                     oracle_lgc_budget() == -1, what);
        oracle_lgc_forget(&gs[0]);
        snprintf(what, sizeof what, "forget ends the %s latch",
                 control_name[i]);
        helper_check(!isaac_lua_gc_scripted(&gs[0]), what);
    }
    /* a latched state has no late rule; latches are per state */
    isaac_lua_gc_script_control(&ls[1], LUA_GCSTOP);
    helper_check(isaac_lua_gc_scripted(&gs[1]) && !isaac_lua_gc_scripted(&gs[0]),
                 "latches are per state");
    gs[1].gcstate = GCScallfin;
    gs[1].GCdebt = -5000;
    oracle_lgc_wr(&gs[1], 100000);   /* an offset, V > GCdebt */
    helper_check(oracle_lgc_vend(&gs[1]) == -(long long)MAX_LMEM - 1,
                 "a latched state's vend is none");
    gs[1].gcstate = GCSpause;
    debt = gs[1].GCdebt;
    oracle_lgc_late(&gs[1], oracle_lgc_vend(&gs[1]));
    helper_check(gs[1].GCdebt == debt, "a latched state has no late rule");
    gs[0].gcstate = GCScallfin;      /* the unlatched one keeps its rule */
    gs[0].GCdebt = -5000;
    oracle_lgc_wr(&gs[0], 100000);
    helper_check(oracle_lgc_vend(&gs[0]) == 95000,
                 "an unlatched state beside a latched one keeps its schedule");
    /* idempotent, four slots, forget frees one, the fifth overflows */
    isaac_lua_gc_script_control(&ls[1], LUA_GCSETPAUSE);
    isaac_lua_gc_script_control(&ls[2], LUA_GCSTEP);
    isaac_lua_gc_script_control(&ls[3], LUA_GCRESTART);
    isaac_lua_gc_script_control(&ls[0], LUA_GCCOLLECT);
    helper_check(isaac_lua_gc_scripted(&gs[0]) && isaac_lua_gc_scripted(&gs[1]) &&
                 isaac_lua_gc_scripted(&gs[2]) && isaac_lua_gc_scripted(&gs[3]) &&
                 !isaac_lua_gc_scripted(&gs[4]),
                 "a second latch of one state takes no second slot");
    oracle_lgc_forget(&gs[4]);       /* an unlatched state: nothing */
    oracle_lgc_forget(&gs[3]);
    helper_check(!isaac_lua_gc_scripted(&gs[3]) && isaac_lua_gc_scripted(&gs[2]),
                 "forget frees exactly its own slot");
    isaac_lua_gc_script_control(&ls[4], LUA_GCSETSTEPMUL);
    helper_check(isaac_lua_gc_scripted(&gs[4]) && !isaac_lua_gc_scripted(&gs[3]) &&
                 !isaac_lua_gc_scripted(&gs[5]),
                 "a freed slot is reused without overflow");
    isaac_lua_gc_script_control(&ls[5], LUA_GCSTOP);
    helper_check(isaac_lua_gc_scripted(&gs[5]) && isaac_lua_gc_scripted(&gs[6]),
                 "a fifth latched state makes every state pristine");
    printf("helpers checks=%d bad=%d\n", s_helper_checks, s_helper_bad);
    return s_helper_bad ? 1 : 0;
}
#endif

/* "a,b[,c]" -> ints; 0 when the text is not exactly n integers */
static int parse_ints(const char *v, int n, int *out)
{
    int i;
    for (i = 0; i < n; ++i) {
        char *end;
        long x = strtol(v, &end, 10);
        if (end == v || (i + 1 < n ? *end != ',' : *end != '\0'))
            return 0;
        out[i] = (int)x;
        v = end + 1;
    }
    return 1;
}

static int option(const char *arg, const char *key, const char **value)
{
    size_t n = strlen(key);
    if (strncmp(arg, key, n) == 0 && arg[n] == '=') {
        *value = arg + n + 1;
        return 1;
    }
    return 0;
}

/* ctl=NAME[:A[,B]] */
static int parse_ctl(Workload *w, const char *v)
{
    const char *colon = strchr(v, ':');
    size_t n = colon ? (size_t)(colon - v) : strlen(v), i;
    int x[2] = {0, 0};
    if (w->nctl >= MAX_CTL)
        return 0;
    for (i = 0; i < sizeof s_ctls / sizeof s_ctls[0]; ++i) {
        if (strlen(s_ctls[i].name) == n && strncmp(s_ctls[i].name, v, n) == 0)
            break;
    }
    if (i == sizeof s_ctls / sizeof s_ctls[0])
        return 0;
    if (colon && !parse_ints(colon + 1, 1, x) && !parse_ints(colon + 1, 2, x))
        return 0;
    w->ctl[w->nctl] = &s_ctls[i];
    w->ctl_a[w->nctl] = x[0];
    w->ctl_b[w->nctl] = x[1];
    ++w->nctl;
    return 1;
}

int main(int argc, char **argv)
{
    Workload w;
    int a;
    printf("build fin_max=%d fin_cost0=%lu sweep_cost0=%lu quota_build=%d "
           "latch_build=%d fin_step=%d sweep_step=%d gcstepsize=%d "
           "sizeof(TString)=%d\n",
           ORACLE_FIN_MAX, (unsigned long)oracle_lgc_fin_cost(0),
           (unsigned long)oracle_lgc_sweep_cost(0), ORACLE_QUOTA,
           ORACLE_LATCH, ORACLE_FIN_STEP, ORACLE_SWEEP_STEP, (int)GCSTEPSIZE,
           (int)sizeof(TString));
    if (argc >= 2 && strcmp(argv[1], "error") == 0)
        return run_error_case();
    if (argc >= 2 && strcmp(argv[1], "helpers") == 0) {
#if ORACLE_LATCH
        return run_helpers();
#else
        printf("helpers: not a latch build\n");
        return 2;
#endif
    }
    if (argc >= 8 && strcmp(argv[1], "run") == 0) {
        /* run K live_kb fin garb frames loops_per_s [key=value ...] */
        memset(&w, 0, sizeof w);
        w.k = atoi(argv[2]);
        w.live_kb = atoi(argv[3]);
        w.fin = atoi(argv[4]);
        w.garb = atoi(argv[5]);
        w.frames = atoi(argv[6]);
        w.lps = atoi(argv[7]);
        for (a = 8; a < argc; ++a) {
            const char *v;
            if (option(argv[a], "bound_cost", &v))
                s_bound_cost = atoi(v);
            else if (option(argv[a], "bound_max", &v))
                s_bound_max = atoi(v);
            else if (option(argv[a], "ns", &v))
                s_ns_per_unit = atof(v);
            else if (option(argv[a], "sweep_us", &v))
                s_sweep_us = atof(v);
            else if (option(argv[a], "fin_us", &v))
                s_fin_us = atof(v);
            else if (option(argv[a], "burst", &v)) {
                int x[3];
                if (!parse_ints(v, 3, x))
                    return 2;
                w.burst_p = x[0], w.burst_l = x[1], w.burst_n = x[2];
            } else if (option(argv[a], "spike", &v)) {
                int x[2];
                if (!parse_ints(v, 2, x))
                    return 2;
                w.spike_f = x[0], w.spike_n = x[1];
            } else if (option(argv[a], "cap_kb", &v))
                s_cap_bytes = (size_t)atol(v) << 10;
            else if (option(argv[a], "ctl", &v)) {
                if (!parse_ctl(&w, v)) {
                    fprintf(stderr, "bad ctl %s\n", v);
                    return 2;
                }
            } else if (option(argv[a], "collect", &v))
                w.collect = atoi(v);
            else if (option(argv[a], "fin_alloc", &v))
                s_fin_alloc = atoi(v);
            else if (option(argv[a], "fin_str", &v))
                s_fin_str = atoi(v);
            else if (option(argv[a], "err", &v))
                s_err_every = atol(v);
            else if (option(argv[a], "hostpush", &v))
                s_hostpush = atoi(v);
            else if (option(argv[a], "birth", &v))
                w.birth = atoi(v);
            else if (option(argv[a], "abort_kb", &v))
                s_abort_bytes = (size_t)atol(v) << 10;
            else if (option(argv[a], "trace", &v))
                w.trace = atoi(v);
            else if (option(argv[a], "seed", &v)) {
                s_seed = (unsigned int)strtoul(v, NULL, 10);
                s_seed_set = 1;
            } else {
                fprintf(stderr, "unknown option %s\n", argv[a]);
                return 2;
            }
        }
        if (s_bound_cost <= 0 || s_bound_max < 0 || w.frames <= 0 ||
            w.lps <= 0 || w.k < 0 || w.collect < 0 || s_fin_alloc < 0 ||
            (s_fin_str != 0 && s_fin_str < 64) || s_err_every < 0 ||
            s_hostpush < 0)
            return 2;
        if (s_fin_str) {
            s_fin_str_buf = (char *)calloc((size_t)s_fin_str, 1);
            if (!s_fin_str_buf)
                return 2;
        }
        return run_workload(&w);
    }
    fprintf(stderr, "usage: %s error | helpers | run K live_kb fin garb frames "
                    "loops_per_s [bound_cost=N bound_max=N ns=X sweep_us=X "
                    "fin_us=X burst=P,L,N spike=F,N cap_kb=N ctl=NAME[:A[,B]] "
                    "collect=N fin_alloc=N fin_str=N err=P hostpush=N "
                    "birth=1 abort_kb=N trace=N seed=N]\n",
            argv[0]);
    return 2;
}
