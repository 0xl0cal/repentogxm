/* Host oracle + device projection for ISAAC_VITA_LUA_GC_FINALIZER_PACE.
 *
 * Built twice by recomp/test_vita_lua_gc_finalizer_pace.py against the
 * manifest-verified Lua 5.3.3 sources: once with the pristine lgc.c and once
 * with the lgc.c derived by recomp/vita/lua53_gc_finalizer_pace.py.  lgc.c is
 * compiled with -DluaC_step=oracle_real_luaC_step, so every luaC_step -- the
 * paced lua_gc(LUA_GCSTEP) (lapi.c:1076-1078 -> luaC_checkGC) and every
 * allocation-driven luaC_checkGC inside an API call (lapi.c, ldo.c:333,
 * lstate.c:259, ...) -- passes through luaC_step below.  lgc.c itself never
 * calls luaC_step, so nothing bypasses the wrapper.
 *
 * Both builds run the same deterministic workload, shaped like
 * LuaEngine::Update with EID loaded (0x4022d0..0x4023cf: one
 * lua_gc(L, LUA_GCSTEP, K) per main loop, K = ISAAC_VITA_LUA_GCSTEP_CLAMP_KB,
 * because the game's own step never drops below 50 KB and the clamp applies
 * to every larger request):
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
 *   - per loop GARB small tables of plain garbage.
 *
 * Invariants checked in both builds (any violation fails the run):
 *   early   a finalizer runs while the object is still in the ring;
 *   weak    a finalized object is still a weak *value* (5.3 clears weak
 *           values before separating finalizers, lgc.c:1001-1006);
 *   order   within one finalizer batch ids must decrease (tobefnz is built
 *           newest-first from finobj, lgc.c separatetobefnz);
 *   twice/never  each id finalized exactly once, and after lua_close every
 *           created id was finalized;
 *   keys    after two full collections no weak key refers to a finalized
 *           object.
 * Pacing bound, per luaC_step (the derived build must hold it; the pristine
 * build is judged by the same numbers and is expected to violate it, which
 * shows the check has teeth): finalizers run inside one luaC_step <=
 * (budget + GCSTEPSIZE + 200) / COST + 2 * MAX, budget = getdebt(g) on
 * entry (lgc.c:1112-1121).  The loop of lgc.c:1133-1136 stops once the
 * finalizer work n*COST (lgc.c:1082-1085) exceeds budget + GCSTEPSIZE, the
 * last singlestep overshoots by at most MAX, and the tail
 * runafewfinalizers (lgc.c:1142) adds at most MAX.
 *
 * Coverage of the two derived lines is reported from observed state:
 * gcfinnum doubling (<MAX), saturating at MAX, reset to 0 when tobefnz
 * drains, and finalizers seen in GCScallfin vs in another state (the tail
 * runafewfinalizers of luaC_step, lgc.c:1142).
 *
 * Sweep (ISAAC_VITA_LUA_GC_SWEEP_COST, lgc.c:39): a step that starts in a
 * sweep state (GCSswpallgc..GCSswptobefnz) books its non-finalizer units as
 * swept objects, units / SWEEP_COST (sweepstep charges GCSWEEPMAX *
 * GCSWEEPCOST for GCSWEEPMAX objects, lgc.c:1026-1038); the steps per cycle
 * that start in a sweep state show the sweep line is live.
 *
 * Device projection: per luaC_step, work units = budget minus the debt left
 * (or the budget when the step ends the cycle), less the nominal
 * strt.size * sizeof(GCObject *) a step out of GCSpause books
 * (lgc.c:1046) without traversing anything, less the accounted finalizer
 * work.  Marking units cost NS_PER_UNIT, swept objects SWEEP_US each,
 * finalizers a per-finalizer cost.  A step that crosses from propagate or
 * atomic into the sweep is booked as marking (one step per cycle, so the
 * sweep share of that step is under-costed).  A frame is every step it ran.
 * Constants come from the console: 8.93 ns per work unit =
 * 1170 us per 128 KB step (P2b cost profile fit), 0.40 us per swept object
 * (GC profile: menu sweep steps 2.9 ms per ~10,240 objects, in-game 11-15 ms
 * steps without finalizers per ~26,000), 5/10/20 us per LuaBridge __gc.
 * Only counts are computed here; no host time is used. */
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
#if !defined(ORACLE_FIN_COST)
#define ORACLE_FIN_COST ((int)((sizeof(TString) + 4) / 4)) /* lgc.c:39,45 */
#endif
#if !defined(ORACLE_SWEEP_COST)
#define ORACLE_SWEEP_COST ((int)((sizeof(TString) + 4) / 4)) /* lgc.c:39 */
#endif
#define ORACLE_STEPMULADJ 200 /* lgc.c:52 STEPMULADJ */

#define MAX_IDS 4000000
#define RING 64
#define WEAK_ROTATE 256

typedef struct { int id; int slot; } Box;

static int *s_created_frame;
static int *s_finalized_frame;
static unsigned char *s_ring_kept;
static int *s_weak_gen;          /* weak-table generation an id joined */
static int s_gen;
static int s_closing;
static int s_bound_cost = 512, s_bound_max = 8;
static int s_next_id;
static int s_frame;
static int s_ring_ref, s_wv_ref, s_wk_ref;
static long s_fin_total;

static long s_early, s_weak, s_order, s_twice, s_finalized;
static int s_batch_last = -1;
static long s_batches;
static long s_cov_double, s_cov_cap, s_cov_drain, s_cov_callfin, s_cov_tail;
static long s_err_raised;

/* luaC_step wrapper accounting */
static int s_measure;           /* 1 inside the frame loop */
static long s_steps, s_steps_paced, s_bound_bad, s_behind;
static double s_frame_units;    /* marking units of all steps this frame */
static double s_frame_swept;    /* objects swept by all steps this frame */
static long s_frame_fins;       /* finalizers of all steps this frame */
static long s_max_step_fin;
static long s_sweep_steps;      /* steps that start in a sweep state */
static double s_max_step_swept, s_swept_total;
static double s_ns_per_unit = 8.93;
static double s_sweep_us = 0.40;
static const double s_cfin_us[3] = {5.0, 10.0, 20.0};
static struct {
    double ms;
    long budget, fins, frame;
    double units;
    int from, to;
} s_worst;

static int gc_cb(lua_State *L)
{
    Box *b = (Box *)lua_touserdata(L, 1);
    global_State *g = G(L);
    int id = b->id;

    if (s_finalized_frame[id] >= 0)
        ++s_twice;
    s_finalized_frame[id] = s_frame;
    ++s_finalized;
    ++s_fin_total;
    if (g->gcstate == GCScallfin)
        ++s_cov_callfin;
    else
        ++s_cov_tail;
    if (s_closing)
        return 0;   /* lua_close finalizes reachable objects by design */
    if (b->slot > 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, s_ring_ref);
        lua_rawgeti(L, -1, b->slot);
        if (lua_rawequal(L, -1, 1))
            ++s_early;
        lua_pop(L, 2);
    }
    if (s_weak_gen[id] == s_gen) {   /* its table is still the current one */
        lua_rawgeti(L, LUA_REGISTRYINDEX, s_wv_ref);
        if (lua_rawgeti(L, -1, id) != LUA_TNIL)
            ++s_weak;
        lua_pop(L, 2);
    }
    if (s_batch_last >= 0 && id >= s_batch_last)
        ++s_order;
    s_batch_last = id;
    if (g->tobefnz == NULL)   /* udata2finalize already popped this one */
        s_batch_last = -1, ++s_batches;
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
    l_mem debt = g->GCdebt, budget;
    long fin_before = s_fin_total, fins;
    int from = g->gcstate;
    double units, swept = 0.0, ms;
    int sweeping = from == GCSswpallgc || from == GCSswpfinobj ||
                   from == GCSswptobefnz;
    l_mem strt_nominal = (l_mem)g->strt.size * (l_mem)sizeof(GCObject *);

    if (!g->gcrunning) { /* lgc.c:1129-1132: no work */
        oracle_real_luaC_step(L);
        return;
    }
    /* lgc.c:1112-1121 getdebt */
    budget = debt > 0 ? (debt / ORACLE_STEPMULADJ + 1) * g->gcstepmul : 0;
    oracle_real_luaC_step(L);   /* a __gc error longjmps past the rest */
    fins = s_fin_total - fin_before;
    observe_finnum(L);
    ++s_steps;
    if (fins > s_max_step_fin)
        s_max_step_fin = fins;
    if (fins > (budget + GCSTEPSIZE + 200) / s_bound_cost + 2 * s_bound_max)
        ++s_bound_bad;
    if (g->gcstate == GCSpause && g->GCdebt > 0)
        ++s_behind;   /* setpause left a debt: the next cycle has no pause */
    if (!s_measure)
        return;
    if (sweeping)
        ++s_sweep_steps;
    units = g->gcstate != GCSpause ? (double)(budget - g->GCdebt)
                                   : (double)budget;
    if (from == GCSpause)
        units -= (double)strt_nominal;
    units -= (double)fins * ORACLE_FIN_COST;
    if (units < 0)
        units = 0;
    if (sweeping) {
        swept = units / ORACLE_SWEEP_COST;
        units = 0;
        s_swept_total += swept;
        if (swept > s_max_step_swept)
            s_max_step_swept = swept;
    }
    s_frame_units += units;
    s_frame_swept += swept;
    s_frame_fins += fins;
    ms = units * s_ns_per_unit * 1e-6 + swept * s_sweep_us * 1e-3 +
         fins * s_cfin_us[1] * 1e-3;
    if (ms > s_worst.ms) {
        s_worst.ms = ms;
        s_worst.budget = (long)budget;
        s_worst.fins = fins;
        s_worst.frame = s_frame;
        s_worst.units = units;
        s_worst.from = from;
        s_worst.to = g->gcstate;
    }
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

static void new_weak_tables(lua_State *L)
{
    lua_newtable(L);
    lua_newtable(L);
    lua_pushliteral(L, "v");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    s_wv_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L);
    lua_newtable(L);
    lua_pushliteral(L, "k");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    s_wk_ref = luaL_ref(L, LUA_REGISTRYINDEX);
}

static lua_State *new_world(int live_kb)
{
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    luaL_newmetatable(L, "fin");
    lua_pushcfunction(L, gc_cb);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    luaL_newmetatable(L, "finerr");
    lua_pushcfunction(L, gc_err_cb);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    lua_newtable(L);
    s_ring_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    new_weak_tables(L);
    if (luaL_loadstring(L, s_setup) != LUA_OK) {
        fprintf(stderr, "%s\n", lua_tostring(L, -1));
        exit(2);
    }
    lua_pushinteger(L, live_kb);
    lua_call(L, 1, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);
    return L;
}

static void reset_counters(void)
{
    int i;
    for (i = 0; i < MAX_IDS; ++i)
        s_created_frame[i] = s_finalized_frame[i] = -1, s_ring_kept[i] = 0,
        s_weak_gen[i] = -1;
    s_gen = 0;
    s_closing = 0;
    s_next_id = 1;
    s_frame = 0;
    s_fin_total = 0;
    s_early = s_weak = s_order = s_twice = s_finalized = 0;
    s_batch_last = -1;
    s_batches = 0;
    s_cov_double = s_cov_cap = s_cov_drain = s_cov_callfin = s_cov_tail = 0;
    s_err_raised = 0;
    s_measure = 0;
    s_steps = s_steps_paced = s_bound_bad = s_behind = 0;
    s_frame_units = 0;
    s_frame_swept = 0;
    s_frame_fins = 0;
    s_max_step_fin = 0;
    s_sweep_steps = 0;
    s_max_step_swept = s_swept_total = 0;
    memset(&s_worst, 0, sizeof s_worst);
}

static void one_frame(lua_State *L, int fin, int garb, int *ring_pos)
{
    int j;
    if (s_frame > 0 && s_frame % WEAK_ROTATE == 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, s_wv_ref);
        luaL_unref(L, LUA_REGISTRYINDEX, s_wk_ref);
        new_weak_tables(L);
        ++s_gen;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_ring_ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_wv_ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_wk_ref);
    for (j = 0; j < fin; ++j) {
        int id = s_next_id++;
        Box *b;
        if (id >= MAX_IDS) {
            fprintf(stderr, "id space exhausted\n");
            exit(2);
        }
        b = (Box *)lua_newuserdata(L, sizeof *b);
        b->id = id;
        b->slot = -1;
        luaL_setmetatable(L, "fin");
        s_created_frame[id] = s_frame;
        if (j % 4 == 0) {
            b->slot = (*ring_pos)++ % RING + 1;
            s_ring_kept[id] = 1;
            lua_pushvalue(L, -1);
            lua_rawseti(L, -5, b->slot);
        }
        if (j % 3 == 0) {
            lua_pushvalue(L, -1);
            lua_rawseti(L, -4, id);
            s_weak_gen[id] = s_gen;
        }
        if (j % 5 == 0) {
            lua_pushvalue(L, -1);
            lua_pushinteger(L, id);
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
    default: return "?";
    }
}

static int run_workload(int k, int live_kb, int fin, int garb, int frames,
                        int loops_per_s)
{
    lua_State *L;
    int f, ring_pos = 0, peak_kb = 0, cycles = 0;
    long sum_kb = 0, keys_bad = 0, max_frame_fin = 0, lag_max = 0;
    long over1 = 0, over2 = 0, over4 = 0; /* frames above 1/2/4 ms at c10 */
    double lag_sum = 0.0, dev_sum[3] = {0, 0, 0}, dev_max[3] = {0, 0, 0};
    double mark_units_sum = 0.0;
    long lag_n = 0, never = 0, i;

    reset_counters();
    L = new_world(live_kb);
    s_measure = 1;
    for (f = 0; f < frames; ++f) {
        int kb, c;
        s_frame = f;
        s_frame_units = 0;
        s_frame_swept = 0;
        s_frame_fins = 0;
        one_frame(L, fin, garb, &ring_pos);
        cycles += lua_gc(L, LUA_GCSTEP, k);
        ++s_steps_paced;
        if (s_frame_fins > max_frame_fin)
            max_frame_fin = s_frame_fins;
        mark_units_sum += s_frame_units;
        for (c = 0; c < 3; ++c) {
            double ms = s_frame_units * s_ns_per_unit * 1e-6 +
                        s_frame_swept * s_sweep_us * 1e-3 +
                        s_frame_fins * s_cfin_us[c] * 1e-3;
            dev_sum[c] += ms;
            if (ms > dev_max[c])
                dev_max[c] = ms;
            if (c == 1)
                over1 += ms > 1.0, over2 += ms > 2.0, over4 += ms > 4.0;
        }
        kb = lua_gc(L, LUA_GCCOUNT, 0);
        if (kb > peak_kb)
            peak_kb = kb;
        sum_kb += kb;
    }
    s_measure = 0;
    for (i = 1; i < s_next_id; ++i) {
        if (s_finalized_frame[i] >= 0 && !s_ring_kept[i]) {
            long lag = s_finalized_frame[i] - s_created_frame[i];
            lag_sum += (double)lag;
            ++lag_n;
            if (lag > lag_max)
                lag_max = lag;
        }
    }
    /* keys: two full collections, then no weak key may be a finalized id */
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_wk_ref);
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        int id = (int)lua_tointeger(L, -1);
        if (id > 0 && id < MAX_IDS && s_finalized_frame[id] >= 0)
            ++keys_bad;
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    s_closing = 1;
    lua_close(L);
    for (i = 1; i < s_next_id; ++i)
        if (s_finalized_frame[i] < 0)
            ++never;
    printf("K=%d live=%d fin=%d garb=%d frames=%d created=%d finalized=%ld "
           "cycles=%d batches=%ld per_cycle_frames=%.1f peak_kb=%d mean_kb=%ld "
           "steps=%ld paced=%ld max_step_fin=%ld max_frame_fin=%ld "
           "behind=%ld lag_mean=%.1f lag_max=%ld\n",
           k, live_kb, fin, garb, frames, s_next_id - 1, s_finalized, cycles,
           s_batches, cycles ? (double)frames / cycles : 0.0, peak_kb,
           sum_kb / frames, s_steps, s_steps_paced, s_max_step_fin,
           max_frame_fin, s_behind, lag_n ? lag_sum / lag_n : 0.0, lag_max);
    printf("  violations early=%ld weak=%ld order=%ld twice=%ld never=%ld "
           "keys=%ld bound=%ld\n",
           s_early, s_weak, s_order, s_twice, never, keys_bad, s_bound_bad);
    printf("  coverage double=%ld cap=%ld drain=%ld callfin=%ld tail=%ld\n",
           s_cov_double, s_cov_cap, s_cov_drain, s_cov_callfin, s_cov_tail);
    printf("  sweep cost=%d steps=%ld per_cycle=%.1f max_step_objs=%.0f "
           "objs_per_cycle=%.0f\n",
           ORACLE_SWEEP_COST, s_sweep_steps,
           cycles ? (double)s_sweep_steps / cycles : 0.0, s_max_step_swept,
           cycles ? s_swept_total / cycles : 0.0);
    printf("  worst-step ms@c10=%.2f frame=%ld budget=%ld units=%.0f fins=%ld "
           "%s->%s\n",
           s_worst.ms, s_worst.frame, s_worst.budget, s_worst.units,
           s_worst.fins, state_name(s_worst.from), state_name(s_worst.to));
    printf("  device@%d/s mark_units_per_frame=%.0f "
           "gc_ms_per_s(c5,c10,c20)=%.1f,%.1f,%.1f "
           "max_frame_ms(c5,c10,c20)=%.2f,%.2f,%.2f "
           "frames_over(1,2,4ms@c10)=%ld,%ld,%ld cycle_s=%.2f\n",
           loops_per_s, mark_units_sum / frames,
           dev_sum[0] / frames * loops_per_s, dev_sum[1] / frames * loops_per_s,
           dev_sum[2] / frames * loops_per_s, dev_max[0], dev_max[1],
           dev_max[2], over1, over2, over4,
           cycles ? (double)frames / cycles / loops_per_s : 0.0);
    return (s_early || s_weak || s_order || s_twice || never || keys_bad)
               ? 1 : 0;
}

/* Error inside a finalizer during a paced step: both builds must surface
 * LUA_ERRGCMM "error in __gc metamethod (boom)" to the protected caller and
 * keep finalizing the rest afterwards (GCTM propagateerrors=1 path,
 * lgc.c:824-833). */
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
    reset_counters();
    L = new_world(64);
    for (i = 0; i < 64; ++i) {
        Box *b = (Box *)lua_newuserdata(L, sizeof *b);
        b->id = s_next_id++;
        b->slot = (i == 40) ? -2 : -1;
        luaL_setmetatable(L, "finerr");
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
    return (s_finalized == 64 && s_err_raised == 1 && errors == 1 &&
            strcmp(msg, "5:error in __gc metamethod (boom)") == 0 &&
            !s_order && !s_twice) ? 0 : 1;
}

int main(int argc, char **argv)
{
    int bad = 0;
    s_created_frame = (int *)malloc(sizeof(int) * MAX_IDS);
    s_finalized_frame = (int *)malloc(sizeof(int) * MAX_IDS);
    s_ring_kept = (unsigned char *)malloc(MAX_IDS);
    s_weak_gen = (int *)malloc(sizeof(int) * MAX_IDS);
    if (!s_created_frame || !s_finalized_frame || !s_ring_kept || !s_weak_gen)
        return 2;
    printf("build fin_max=%d fin_cost=%d sweep_cost=%d gcstepsize=%d "
           "sizeof(TString)=%d\n",
           ORACLE_FIN_MAX, ORACLE_FIN_COST, ORACLE_SWEEP_COST, (int)GCSTEPSIZE,
           (int)sizeof(TString));
    if (argc >= 2 && strcmp(argv[1], "error") == 0)
        return run_error_case();
    if (argc >= 8 && strcmp(argv[1], "run") == 0) {
        /* run K live_kb fin garb frames loops_per_s
         *     [bound_cost bound_max [ns_per_unit [sweep_us]]] */
        if (argc >= 10)
            s_bound_cost = atoi(argv[8]), s_bound_max = atoi(argv[9]);
        if (argc >= 11)
            s_ns_per_unit = atof(argv[10]);
        if (argc >= 12)
            s_sweep_us = atof(argv[11]);
        if (s_bound_cost <= 0 || s_bound_max < 0)
            return 2;
        bad = run_workload(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]),
                           atoi(argv[5]), atoi(argv[6]), atoi(argv[7]));
        return bad;
    }
    fprintf(stderr, "usage: %s error | run K live_kb fin garb frames "
                    "loops_per_s [bound_cost bound_max [ns_per_unit "
                    "[sweep_us]]]\n",
            argv[0]);
    return 2;
}
