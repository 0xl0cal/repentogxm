/* Host oracle for ISAAC_VITA_LUA_GC_STATE_PROFILE and
 * ISAAC_VITA_LUA_GC_IDLE_STEP (host_vita_lua_gc.c), driven by
 * recomp/test_vita_lua_gc_state_idle.py.
 *
 * host_vita_lua_gc.c is the last text of this translation unit, so the unit
 * under test is the file the game compiles, with three seams:
 *   - guest.h is the stand-in of vita_lua_gc_state_idle_oracle_guest/ (the
 *     lua_gc call site is g_oracle_gc_site);
 *   - VITA_LUA_GC_ORACLE_CLOCK routes its clock to this file: a virtual one
 *     (every read advances 3 us, a scripted cost is added per lua_gc step),
 *     or the host's monotonic clock for the "real" modes;
 *   - lua_gc is renamed to a recorder that forwards to Lua's own.
 * Lua is the real 5.3.3 with the derived lgc.c / lbaselib.c the test built
 * (with or without --state-profile); nothing of the collector is modelled.
 *
 * Modes (argv[1]):
 *   state-virtual   two allocation regimes, 40 windows.  Prints the unit's
 *                   own window lines, then REF: the atomic crossings seen
 *                   from outside through G(L)->gcstate, split into "during
 *                   the frame's Lua code" (allocation-driven steps) and
 *                   "during the Update-site lua_gc", and DIGEST over what
 *                   the collector did (count, countb, gcstate, finalizers
 *                   per frame).  The test compares REF with the summed
 *                   window fields and DIGEST with the build without marks.
 *   state-real H    H = weak | control: a heap whose atomic re-traverses
 *                   and clears a large weak table and many back-barriered
 *                   tables, against the same objects held strongly and
 *                   left alone.  Host clock; prints the window lines.
 *   idle            (ISAAC_VITA_LUA_GC_IDLE_STEP builds) scripted scheduler
 *                   answers around the two lua_gc calls of LuaEngine::Update
 *                   (LUA_GCCOUNT from 0x4022ec, LUA_GCSTEP from 0x4023c6).
 *                   Judged from outside the unit, through the recorder and
 *                   G(L): every step ran once, in order, with its own k, in
 *                   its own call or at the start of the next; which steps
 *                   were carried (never from a tail that is not BUSY, never a
 *                   step that is the atomic, restarts the cycle or runs
 *                   nothing; always a sweeping one); a carried step ran
 *                   before the next count; and, when no Lua code runs in
 *                   between, a carried step never ran atomic().  Prints the
 *                   unit's "gc idle" lines.
 *   idle-off        the same call sequence against a build without the
 *                   knob: prints the DIGEST of the steps Lua received.
 *   ctl CLAMP       the game's own step controller (sub_004022d0, copied
 *                   from the disassembly) closed around the unit for 3600
 *                   ticks: prints the k it requested.  The test compares an
 *                   IDLE build with the build without the knob.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include "lgc.h"
#include "lstate.h"

#include "vita_lua_gc_state_idle_oracle_guest/guest.h"

uint32_t g_oracle_gc_site;

static int s_real_clock;
static uint64_t s_virtual_us;

static uint64_t oracle_real_us(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER frequency;
    LARGE_INTEGER ticks;
    if (!frequency.QuadPart)
        QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&ticks);
    return (uint64_t)(ticks.QuadPart / frequency.QuadPart) * 1000000ULL +
        (uint64_t)(ticks.QuadPart % frequency.QuadPart) * 1000000ULL /
            (uint64_t)frequency.QuadPart;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000ULL + (uint64_t)now.tv_nsec / 1000U;
#endif
}

uint64_t vita_lua_gc_oracle_now_us(void);
uint64_t vita_lua_gc_oracle_now_us(void)
{
    if (s_real_clock)
        return oracle_real_us();
    s_virtual_us += 3U;
    return s_virtual_us;
}

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;
    char body[384];                     /* recomp/vita/platform.c:208 */

    va_start(arguments, format);
    vsnprintf(body, sizeof body, format, arguments);
    va_end(arguments);
    printf("%s\n", body);
}

void isaac_vita_lua_arena_stats(uint32_t *live_kb, uint32_t *peak_kb,
                                uint32_t *fallbacks);
void isaac_vita_lua_arena_stats(uint32_t *live_kb, uint32_t *peak_kb,
                                uint32_t *fallbacks)
{
    *live_kb = *peak_kb = *fallbacks = 0U;
}

/* --- the lua_gc recorder -------------------------------------------------- */
#define ORACLE_MAX_STEPS 8192U
typedef struct OracleStep {
    int kb;
    uint32_t call;                      /* tick whose import forwarded it */
    uint8_t in_count;                   /* forwarded by the GCCOUNT import */
    uint8_t before_count;               /* ... before that tick's count */
    uint8_t flipped;                    /* atomic() ran: current white flipped */
    int8_t state_before;
    int8_t state_after;
} OracleStep;
static OracleStep s_steps[ORACLE_MAX_STEPS];
static uint32_t s_ran;
static uint32_t s_call;                 /* index of the import call running */
static uint32_t s_ref_atomic_in_gc;
static int s_in_count_import;           /* the oracle is inside oracle_count */
static uint32_t s_counts_forwarded;     /* LUA_GCCOUNT forwards of this tick */
static uint32_t s_step_cost_us;         /* virtual cost of a lua_gc step */
static uint32_t s_atomic_cost_us;       /* ... of one that ran atomic() */

static int oracle_marking(int gcstate)
{
    return gcstate <= GCSatomic || gcstate == GCSpause;
}

static int oracle_crossed_atomic(int before, int after)
{
    return oracle_marking(before) &&
        (!oracle_marking(after) || (before != GCSpause && after == GCSpause));
}

static int oracle_lua_gc(lua_State *L, int what, int data)
{
    int before = G(L)->gcstate;
    lu_byte white = G(L)->currentwhite;
    int flipped;
    int result;

    result = (lua_gc)(L, what, data);
    if (what == LUA_GCCOUNT) {
        ++s_counts_forwarded;
        return result;
    }
    if (what != LUA_GCSTEP)
        return result;
    flipped = ((white ^ G(L)->currentwhite) & WHITEBITS) != 0;
    if (s_ran < ORACLE_MAX_STEPS) {
        OracleStep *step = &s_steps[s_ran];
        step->kb = data;
        step->call = s_call;
        step->in_count = (uint8_t)s_in_count_import;
        step->before_count = (uint8_t)(s_counts_forwarded == 0U);
        step->flipped = (uint8_t)flipped;
        step->state_before = (int8_t)before;
        step->state_after = (int8_t)G(L)->gcstate;
    }
    ++s_ran;
    s_virtual_us += flipped && s_atomic_cost_us ? s_atomic_cost_us :
        s_step_cost_us;
    if (oracle_crossed_atomic(before, G(L)->gcstate))
        ++s_ref_atomic_in_gc;
    return result;
}

#if defined(ISAAC_VITA_LUA_GC_IDLE_STEP)
static int s_idle_kind;
static uint64_t s_idle_deadline_us;     /* the wait target of a FREE tail */
static uint32_t s_idle_queries;
#endif

#define lua_gc oracle_lua_gc
#include "host_vita_lua_gc.c"
#undef lua_gc

#if defined(ISAAC_VITA_LUA_GC_IDLE_STEP)
/* As the real query: the slack is the time left to the wait target, so a
 * second query of one tail sees what the first step left. */
int kage_vita_fullspeed_scheduler_gc_idle_query(uint32_t *slack_us)
{
    ++s_idle_queries;
    *slack_us = s_idle_kind == KAGE_VITA_GC_IDLE_FREE &&
        s_idle_deadline_us > s_virtual_us ?
        (uint32_t)(s_idle_deadline_us - s_virtual_us) : 0U;
    return s_idle_kind;
}
#endif

#define CHECK(condition, ...) \
    do { \
        if (!(condition)) { \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
            exit(1); \
        } \
    } while (0)

static uint64_t oracle_fnv(uint64_t hash, uint64_t value)
{
    uint32_t i;
    for (i = 0U; i < 8U; ++i) {
        hash ^= (value >> (8U * i)) & 0xffU;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static const char *s_setup =
    "local n_keep, weak = ...\n"
    "keep = {}\n"
    "wk = setmetatable({}, {__mode = 'v'})\n"
    "fincount = 0\n"
    "local mt = {__gc = function() fincount = fincount + 1 end}\n"
    "for i = 1, n_keep do\n"
    "  local o = {i, i + 1}\n"
    "  keep[i] = o\n"
    "  if weak then wk[i] = o end\n"
    "end\n"
    "function frame(n, touch)\n"
    "  for j = 1, n do\n"
    "    local v = {j, j}\n"
    "    if j % 8 == 0 then setmetatable(v, mt) end\n"
    "  end\n"
    "  for j = 1, touch do\n"
    "    keep[(j * 7919) % n_keep + 1][1] = {j}\n"
    "  end\n"
    "  return fincount\n"
    "end\n";

static lua_State *oracle_state(int keep, int weak)
{
    lua_State *L = luaL_newstate();

    CHECK(L != NULL, "luaL_newstate");
    /* The base library only.  luaL_openlibs would open `package`, which
     * keys the registry by the ADDRESS of a static (loadlib.c:280, &CLIBS):
     * the registry's node order, hence the order the roots are marked in,
     * hence the frame a cycle's atomic lands on, then depend on the link
     * layout of this executable -- and two builds of the oracle (with and
     * without the state marks) stop being comparable step for step. */
    luaL_requiref(L, "_G", luaopen_base, 1);
    lua_pop(L, 1);
    CHECK(luaL_loadstring(L, s_setup) == LUA_OK, "%s", lua_tostring(L, -1));
    lua_pushinteger(L, keep);
    lua_pushboolean(L, weak);
    CHECK(lua_pcall(L, 2, 0, 0) == LUA_OK, "%s", lua_tostring(L, -1));
    return L;
}

static int oracle_frame(lua_State *L, int allocations, int touch)
{
    int finalizers;

    lua_getglobal(L, "frame");
    lua_pushinteger(L, allocations);
    lua_pushinteger(L, touch);
    CHECK(lua_pcall(L, 2, 1, 0) == LUA_OK, "%s", lua_tostring(L, -1));
    finalizers = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    return finalizers;
}

static int oracle_step(lua_State *L, int kb)
{
    CPU cpu = { 0U, 0U };
    g_oracle_gc_site = VITA_LUA_GC_UPDATE_STEP_RETURN_RVA;
    return isaac_vita_lua_gc_call(&cpu, L, LUA_GCSTEP, kb, kb);
}

/* --- state modes ---------------------------------------------------------- */
static int oracle_state_run(int keep, int weak, int windows, int touch,
                            int two_regimes)
{
    lua_State *L = oracle_state(keep, weak);
    uint32_t frames = (uint32_t)windows * 120U;
    uint32_t ref_alloc = 0U;
    uint32_t ref_upd = 0U;
    uint64_t digest = 1469598103934665603ULL;
    uint32_t f;
    int state_after = G(L)->gcstate;

#if defined(ISAAC_VITA_LUA_GC_STATE_PROFILE)
    /* The heap's construction is not part of the transcript: REF starts
     * here, so the unit's window starts here too. */
    CHECK(!s_vita_lua_gc_state.open, "a luaC_step is open after the setup");
    memset(&s_vita_lua_gc_state, 0, sizeof s_vita_lua_gc_state);
#endif
    for (f = 0U; f < frames; ++f) {
        int heavy = two_regimes && f < frames / 2U;
        int allocations = two_regimes ? (heavy ? 200 : 10) : 50;
        int kb = two_regimes ? (heavy ? 1 : 64) : 64;
        int finalizers = oracle_frame(L, allocations, touch);
        int state_before = G(L)->gcstate;
        uint32_t in_gc = s_ref_atomic_in_gc;

        if (oracle_crossed_atomic(state_after, state_before))
            ++ref_alloc;
        s_call = f;
        (void)oracle_step(L, kb);
        ref_upd += s_ref_atomic_in_gc - in_gc;
        state_after = G(L)->gcstate;
        digest = oracle_fnv(digest, (uint64_t)(lua_gc)(L, LUA_GCCOUNT, 0));
        digest = oracle_fnv(digest, (uint64_t)(lua_gc)(L, LUA_GCCOUNTB, 0));
        digest = oracle_fnv(digest, (uint64_t)state_after);
        digest = oracle_fnv(digest, (uint64_t)finalizers);
    }
    printf("REF frames=%u atomic_upd=%u atomic_alloc=%u steps=%u\n",
           (unsigned)frames, (unsigned)ref_upd, (unsigned)ref_alloc,
           (unsigned)s_ran);
    printf("DIGEST %016llx\n", (unsigned long long)digest);
    lua_close(L);
    return 0;
}

/* --- idle modes ----------------------------------------------------------- */
#define ORACLE_UPDATE_COUNT_RETURN_RVA 0x004022ecU  /* sub_004022d0: 4022e6 call */

static int oracle_idle_kb(uint32_t call)
{
    return 10 + (int)(call % 50U);
}

/* LuaEngine::Update's LUA_GCCOUNT (the first lua_gc of sub_004022d0). */
static int oracle_count(lua_State *L)
{
    CPU cpu = { 0U, 0U };
    int result;

    g_oracle_gc_site = ORACLE_UPDATE_COUNT_RETURN_RVA;
    s_in_count_import = 1;
    result = isaac_vita_lua_gc_call(&cpu, L, LUA_GCCOUNT, 0, 0);
    s_in_count_import = 0;
    return result;
}

/* The game's step controller, sub_004022d0 between its two lua_gc calls
 * (frozen PE, image base 0x400000; the fields are LuaEngine's, at edi):
 *   4022ec  edx = [edi+0x58]; esi = count; cmovs edx, esi     prev < 0: none
 *   402307  ring[[edi+0x50] % 10] = count - prev              [edi+0x28..]
 *   402312  ++[edi+0x50]; [edi+0x58] = count
 *   402321  ten times: v = ring[(n + i) % 10]; sum += v; flag &= v >= 0
 *   40234b  flag && sum > 0: [edi+0x54] = 600 (0x258);
 *           k = min([edi+0x24] + sum / 10, 10000 (0x2710))
 *   40237c  else [edi+0x54] > 0: --[edi+0x54], k unchanged (to 4023ba)
 *   40238c  else k = [edi+0x24] - max([edi+0x24] / 100, 10)
 *   4023ad  [edi+0x24] = max(k, 50 (0x32))
 *   4023ba  lua_gc(L, LUA_GCSTEP, k)
 * The initial field values are this oracle's (the constructor was not
 * read): no previous count, an empty ring, countdown 0, k = 200. */
typedef struct OracleController {
    int ring[10];
    int n;
    int countdown;
    int last;
    int k;
    uint32_t grew;
    uint32_t decayed;
} OracleController;

static void oracle_controller_reset(OracleController *c)
{
    memset(c, 0, sizeof *c);
    c->last = -1;
    c->k = 200;
}

static int oracle_controller_k(OracleController *c, int count)
{
    int prev = c->last < 0 ? count : c->last;
    int sum = 0;
    int flag = 1;
    int k;
    int i;

    c->ring[c->n % 10] = count - prev;
    ++c->n;
    c->last = count;
    for (i = 0; i < 10; ++i) {
        int v = c->ring[(c->n + i) % 10];
        sum += v;
        if (v < 0)
            flag = 0;
    }
    if (flag && sum > 0) {
        c->countdown = 600;
        k = c->k + sum / 10;
        if (k > 10000)
            k = 10000;
        ++c->grew;
    } else if (c->countdown > 0) {
        --c->countdown;
        return c->k;
    } else {
        int dec = c->k / 100;
        if (dec < 10)
            dec = 10;
        k = c->k - dec;
        ++c->decayed;
    }
    if (k < 50)
        k = 50;
    c->k = k;
    return k;
}

/* A mutator with phases, for the controller replay: garbage every tick, a
 * retained object now and then, and a stretch that drops a third of them. */
static const char *s_ctl_setup =
    "function ctl_frame(t)\n"
    "  local phase = (t // 300) % 4\n"
    "  for j = 1, 40 do local v = {j, j} end\n"
    "  if phase == 0 then\n"
    "    for j = 1, 12 do keep[#keep + 1] = {t, j, {t}} end\n"
    "  elseif phase == 2 then\n"
    "    for j = 1, 16 do\n"
    "      local n = #keep\n"
    "      if n > 4000 then keep[n] = nil end\n"
    "    end\n"
    "  end\n"
    "end\n";

/* ctl CLAMP: the controller above closed around the unit, CLAMP as
 * vita_lua_gcstep_clamp (host_vita_lua.c: k > CLAMP -> CLAMP; 0 = none).
 * On an IDLE build the scheduler answers BUSY / FREE in turn; the build
 * without the knob prints the reference. */
static int oracle_ctl_run(int clamp)
{
    lua_State *L = oracle_state(20000, 0);
    OracleController controller;
    uint32_t ticks = 3600U;
    uint64_t sum_k = 0U;
    uint64_t sum_count = 0U;
    int min_k = 0x7fffffff;
    int max_k = 0;
    uint32_t tick;
    uint32_t flips = 0U;

    CHECK(luaL_loadstring(L, s_ctl_setup) == LUA_OK &&
          lua_pcall(L, 0, 0, 0) == LUA_OK, "%s", lua_tostring(L, -1));
    oracle_controller_reset(&controller);
    s_ran = 0U;
    s_step_cost_us = 300U;
    for (tick = 0U; tick < ticks; ++tick) {
        CPU cpu = { 0U, 0U };
        int count;
        int k;
        uint32_t ran = s_ran;

        lua_getglobal(L, "ctl_frame");
        lua_pushinteger(L, (lua_Integer)tick);
        CHECK(lua_pcall(L, 1, 0, 0) == LUA_OK, "%s", lua_tostring(L, -1));
        s_call = tick;
        s_counts_forwarded = 0U;
#if defined(ISAAC_VITA_LUA_GC_IDLE_STEP)
        s_idle_kind = (tick & 1U) ? KAGE_VITA_GC_IDLE_FREE :
            KAGE_VITA_GC_IDLE_BUSY;
        s_idle_deadline_us = s_virtual_us + 6000U;
#endif
        count = oracle_count(L);
        k = oracle_controller_k(&controller, count);
        g_oracle_gc_site = VITA_LUA_GC_UPDATE_STEP_RETURN_RVA;
        (void)isaac_vita_lua_gc_call(&cpu, L, LUA_GCSTEP, k,
                                     clamp > 0 && k > clamp ? clamp : k);
        sum_k += (uint64_t)k;
        sum_count += (uint64_t)count;
        if (k < min_k)
            min_k = k;
        if (k > max_k)
            max_k = k;
        for (; ran < s_ran && ran < ORACLE_MAX_STEPS; ++ran)
            flips += s_steps[ran].flipped;
    }
    printf("CTL clamp=%d ticks=%u sum_k=%llu final_k=%d min_k=%d max_k=%d "
           "grew=%u decayed=%u mean_count_kb=%llu cycles=%u steps=%u\n",
           clamp, (unsigned)ticks, (unsigned long long)sum_k, controller.k,
           min_k, max_k, (unsigned)controller.grew,
           (unsigned)controller.decayed,
           (unsigned long long)(sum_count / ticks), (unsigned)flips,
           (unsigned)s_ran);
    s_step_cost_us = 0U;
    lua_close(L);
    return 0;
}

#if defined(ISAAC_VITA_LUA_GC_IDLE_STEP)
typedef struct OracleIdleScript {
    const char *name;
    int kinds[2];                        /* alternating scheduler answers */
    uint32_t calls;
    int keep;                            /* live tables of the heap */
    uint32_t slack_us;
    uint32_t cost_us;
    uint32_t atomic_us;                  /* cost of a step that ran atomic() */
    /* quiet: no Lua code runs between a BUSY tail and the next tick, so a
     * carried step meets the collector state its decision read */
    int quiet;
    int no_count;                        /* the GCCOUNT import never arrives */
} OracleIdleScript;

#define ORACLE_MAX_TICKS 2400U
static int8_t s_tick_state[ORACLE_MAX_TICKS];   /* gcstate at the step import */
static uint8_t s_tick_kind[ORACLE_MAX_TICKS];
static uint8_t s_tick_noop[ORACLE_MAX_TICKS];   /* no luaC_step would run */

static int oracle_sweeping(int gcstate)
{
    return gcstate >= GCSswpallgc && gcstate <= GCScallfin;
}

static void oracle_idle_script(const OracleIdleScript *script)
{
    lua_State *L = oracle_state(script->keep, 0);
    uint32_t call;
    uint32_t i;
    uint32_t carried = 0U;
    uint32_t carried_by[3] = { 0U, 0U, 0U };   /* prop, sweep, callfin */
    uint32_t inline_by[4] = { 0U, 0U, 0U, 0U };/* noop, pause, prop, atomic */
    uint32_t flips_own = 0U;
    uint32_t flips_carried = 0U;
    uint32_t flips_busy_inline = 0U;
    uint32_t queries_before = s_idle_queries;

    CHECK(script->calls <= ORACLE_MAX_TICKS, "%s: calls", script->name);
    printf("IDLE-BEGIN %s\n", script->name);
    s_ran = 0U;
    s_step_cost_us = script->cost_us;
    s_atomic_cost_us = script->atomic_us;
    for (call = 0U; call < script->calls; ++call) {
        int kind = script->kinds[call & 1U];
        int previous = call ? script->kinds[(call - 1U) & 1U] :
            KAGE_VITA_GC_IDLE_UNKNOWN;
        int kb = oracle_idle_kb(call);

        if (!script->quiet || previous != KAGE_VITA_GC_IDLE_BUSY)
            (void)oracle_frame(L, script->quiet ? 120 : 60, 0);
        s_call = call;
        s_counts_forwarded = 0U;
        s_idle_kind = kind;
        s_idle_deadline_us = s_virtual_us + script->slack_us;
        if (!script->no_count)
            (void)oracle_count(L);
        s_tick_state[call] = (int8_t)G(L)->gcstate;
        s_tick_kind[call] = (uint8_t)(kind == KAGE_VITA_GC_IDLE_BUSY);
        s_tick_noop[call] = (uint8_t)((l_mem)kb * 1024 + G(L)->GCdebt <= 0);
        (void)oracle_step(L, kb);
        CHECK(s_vita_lua_gc_idle.owed <= VITA_LUA_GC_IDLE_OWED_MAX,
              "%s: owed bound at call %u", script->name, (unsigned)call);
    }
    s_step_cost_us = 0U;
    s_atomic_cost_us = 0U;
    /* No lost, duplicated, merged, split or reordered step: the n-th step
     * Lua received is the n-th the game asked for, with its own k, and it
     * ran in the call that asked or at the start of the next one. */
    CHECK(s_ran + s_vita_lua_gc_idle.owed == script->calls,
          "%s: ran %u + owed %u != %u calls", script->name, (unsigned)s_ran,
          (unsigned)s_vita_lua_gc_idle.owed, (unsigned)script->calls);
    for (i = 0U; i < s_ran; ++i) {
        const OracleStep *step = &s_steps[i];
        int state = s_tick_state[i];
        int own = step->call == i;

        CHECK(step->kb == oracle_idle_kb(i),
              "%s: step %u ran with k=%d, asked %d", script->name,
              (unsigned)i, step->kb, oracle_idle_kb(i));
        CHECK(own || step->call == i + 1U,
              "%s: step %u ran in call %u", script->name, (unsigned)i,
              (unsigned)step->call);
        /* the decision, judged from outside the unit */
        if (!s_tick_kind[i]) {
            CHECK(own, "%s: step %u carried from a tail that is not BUSY",
                  script->name, (unsigned)i);
        } else if (s_tick_noop[i] || state == GCSpause ||
                   state == GCSatomic) {
            CHECK(own, "%s: step %u carried from gcstate %d (noop %d): a "
                  "step that is the atomic, restarts the cycle or runs "
                  "nothing", script->name, (unsigned)i, state,
                  (int)s_tick_noop[i]);
            ++inline_by[s_tick_noop[i] ? 0 : state == GCSpause ? 1 : 3];
        } else if (oracle_sweeping(state)) {
            CHECK(!own, "%s: BUSY step %u in gcstate %d was not carried",
                  script->name, (unsigned)i, state);
        } else if (own) {
            ++inline_by[2];             /* propagate, not proven */
        }
        if (own) {
            CHECK(!step->in_count, "%s: step %u, an own step, ran in the "
                  "GCCOUNT import", script->name, (unsigned)i);
            flips_own += step->flipped;
            if (s_tick_kind[i])
                flips_busy_inline += step->flipped;
            /* a BUSY tail's own step that did not start as the atomic and
             * is not a cycle restart must have been "not proven" */
        } else {
            ++carried;
            ++carried_by[state == GCScallfin ? 2 :
                         oracle_sweeping(state) ? 1 : 0];
            flips_carried += step->flipped;
            /* the carried step runs before LuaEngine::Update reads the
             * heap -- inside the GCCOUNT import, ahead of its count */
            if (!script->no_count)
                CHECK(step->in_count && step->before_count,
                      "%s: carried step %u ran %s", script->name,
                      (unsigned)i, step->in_count ? "after the count" :
                      "outside the GCCOUNT import");
            else
                CHECK(!step->in_count, "%s: carried step %u", script->name,
                      (unsigned)i);
            /* with nothing between the decision and the run, a carried
             * step never runs atomic() */
            if (script->quiet)
                CHECK(!step->flipped && step->state_before == state &&
                      step->state_after != GCSatomic,
                      "%s: carried step %u (issued in gcstate %d) ran "
                      "gcstate %d>%d, atomic=%d", script->name, (unsigned)i,
                      state, step->state_before, step->state_after,
                      (int)step->flipped);
        }
    }
    CHECK(s_idle_queries - queries_before == script->calls + carried,
          "%s: %u scheduler queries for %u calls and %u carried steps",
          script->name, (unsigned)(s_idle_queries - queries_before),
          (unsigned)script->calls, (unsigned)carried);
    printf("IDLE %s: calls=%u ran=%u owed_end=%u carried=%u "
           "carried(prop,sweep,callfin)=%u,%u,%u "
           "busy_inline(noop,pause,prop,atomic)=%u,%u,%u,%u "
           "flips(own,carried,busy_inline)=%u,%u,%u quiet=%d PASS\n",
           script->name, (unsigned)script->calls, (unsigned)s_ran,
           (unsigned)s_vita_lua_gc_idle.owed,
           (unsigned)carried, (unsigned)carried_by[0],
           (unsigned)carried_by[1], (unsigned)carried_by[2],
           (unsigned)inline_by[0], (unsigned)inline_by[1],
           (unsigned)inline_by[2], (unsigned)inline_by[3],
           (unsigned)flips_own, (unsigned)flips_carried,
           (unsigned)flips_busy_inline, script->quiet);
    /* A step still owed is run before the state is closed (a new state can
     * be allocated at the same address, and the unit keys on the pointer);
     * it lands in the next window's ran/owed_kb.in, where the test's
     * balance expects it. */
    if (s_vita_lua_gc_idle.owed) {
        s_idle_kind = KAGE_VITA_GC_IDLE_LATE;
        s_counts_forwarded = 0U;
        (void)oracle_count(L);
        CHECK(!s_vita_lua_gc_idle.owed, "%s: epilogue", script->name);
    }
    lua_close(L);
}

/* Drive `L` with BUSY tails until the unit owes it a step. */
static void oracle_idle_owe(lua_State *L, const char *what)
{
    uint32_t i;

    s_idle_kind = KAGE_VITA_GC_IDLE_BUSY;
    for (i = 0U; i < 20000U && !s_vita_lua_gc_idle.owed; ++i) {
        (void)oracle_frame(L, 60, 0);
        s_counts_forwarded = 0U;
        (void)oracle_count(L);
        (void)oracle_step(L, 11);
    }
    CHECK(s_vita_lua_gc_idle.owed == 1U, "%s: no step was ever carried", what);
}

static int oracle_idle_run(void)
{
    static const OracleIdleScript scripts[] = {
        /* scheduler not in charge: exactly the build without the knob */
        { "unknown", { KAGE_VITA_GC_IDLE_UNKNOWN, KAGE_VITA_GC_IDLE_UNKNOWN },
          240U, 20000, 0U, 300U, 0U, 0, 0 },
        /* idle room: the head always sleeps */
        { "idle-15ms", { KAGE_VITA_GC_IDLE_FREE, KAGE_VITA_GC_IDLE_FREE },
          240U, 20000, 15000U, 300U, 0U, 0, 0 },
        /* fight: full tick BUSY, non-full tick before a 6 ms floor wait;
         * many collector cycles on a small heap, quiet */
        { "fight-6ms", { KAGE_VITA_GC_IDLE_BUSY, KAGE_VITA_GC_IDLE_FREE },
          2400U, 3000, 6000U, 300U, 0U, 1, 0 },
        /* the same one call later: the windows close on BUSY tails, so a
         * step can be owed across a window */
        { "fight-shift", { KAGE_VITA_GC_IDLE_FREE, KAGE_VITA_GC_IDLE_BUSY },
          2400U, 3000, 6000U, 300U, 0U, 1, 0 },
        /* the atomic costs 13 ms against 6 ms of slack: it must
         * run where the build without the knob runs it */
        { "fight-6ms-atomic13",
          { KAGE_VITA_GC_IDLE_BUSY, KAGE_VITA_GC_IDLE_FREE },
          2400U, 3000, 6000U, 300U, 13000U, 1, 0 },
        /* Lua code between the BUSY tail and the next tick (the render's
         * callbacks): allocation-driven steps move the collector on */
        { "fight-render-allocs",
          { KAGE_VITA_GC_IDLE_BUSY, KAGE_VITA_GC_IDLE_FREE },
          2400U, 3000, 6000U, 300U, 13000U, 0, 0 },
        /* a large heap: long propagate, long gray list */
        { "fight-big-heap", { KAGE_VITA_GC_IDLE_BUSY, KAGE_VITA_GC_IDLE_FREE },
          1200U, 20000, 6000U, 300U, 0U, 1, 0 },
        /* no slack at all */
        { "zero-slack", { KAGE_VITA_GC_IDLE_BUSY, KAGE_VITA_GC_IDLE_LATE },
          1200U, 3000, 0U, 300U, 0U, 1, 0 },
        /* every tail BUSY */
        { "busy-busy", { KAGE_VITA_GC_IDLE_BUSY, KAGE_VITA_GC_IDLE_BUSY },
          1200U, 3000, 0U, 300U, 0U, 0, 0 },
        /* the GCCOUNT import never arrives: the GCSTEP import settles */
        { "no-count", { KAGE_VITA_GC_IDLE_BUSY, KAGE_VITA_GC_IDLE_FREE },
          1200U, 3000, 6000U, 300U, 0U, 1, 1 },
        /* an answer outside the enum is UNKNOWN */
        { "bad-kind", { 7, -1 }, 240U, 20000, 0U, 300U, 0U, 0, 0 },
    };
    uint32_t i;

    for (i = 0U; i < sizeof scripts / sizeof scripts[0]; ++i)
        oracle_idle_script(&scripts[i]);

    /* A carried step belongs to its lua_State: dropped at the GCCOUNT
     * import of another state, and at its GCSTEP import. */
    {
        lua_State *a = oracle_state(2000, 0);
        lua_State *b = oracle_state(2000, 0);
        uint32_t dropped;
        uint32_t dropped_kb;

        printf("IDLE-BEGIN state-change\n");
        oracle_idle_owe(a, "state a");
        dropped = s_vita_lua_gc_idle.dropped;
        dropped_kb = s_vita_lua_gc_idle.dropped_kb;
        s_ran = 0U;
        s_idle_kind = KAGE_VITA_GC_IDLE_LATE;
        (void)oracle_count(b);
        CHECK(s_ran == 0U && s_vita_lua_gc_idle.owed == 0U &&
              s_vita_lua_gc_idle.dropped == dropped + 1U &&
              s_vita_lua_gc_idle.dropped_kb == dropped_kb + 11U,
              "a's carried step was not dropped at b's GCCOUNT import");
        (void)oracle_step(b, 12);
        CHECK(s_ran == 1U && s_steps[0].kb == 12, "b ran its own step only");

        oracle_idle_owe(a, "state a again");
        dropped = s_vita_lua_gc_idle.dropped;
        s_ran = 0U;
        s_idle_kind = KAGE_VITA_GC_IDLE_LATE;
        (void)oracle_step(b, 13);
        CHECK(s_ran == 1U && s_steps[0].kb == 13 &&
              s_vita_lua_gc_idle.owed == 0U &&
              s_vita_lua_gc_idle.dropped == dropped + 1U,
              "a's carried step was not dropped at b's GCSTEP import");
        printf("IDLE state-change: dropped=2 PASS\n");
        lua_close(a);
        lua_close(b);
    }
    return 0;
}
#else
static int oracle_idle_off(void)
{
    lua_State *L = oracle_state(20000, 0);
    uint64_t digest = 1469598103934665603ULL;
    uint32_t call;
    uint32_t i;

    s_ran = 0U;
    for (call = 0U; call < 240U; ++call) {
        (void)oracle_frame(L, 60, 0);
        s_call = call;
        s_counts_forwarded = 0U;
        (void)oracle_count(L);
        (void)oracle_step(L, oracle_idle_kb(call));
    }
    CHECK(s_ran == 240U, "one step per call");
    for (i = 0U; i < s_ran; ++i) {
        CHECK(s_steps[i].kb == oracle_idle_kb(i) && s_steps[i].call == i &&
              !s_steps[i].in_count, "step %u", (unsigned)i);
        digest = oracle_fnv(digest, (uint64_t)s_steps[i].kb);
        digest = oracle_fnv(digest, (uint64_t)s_steps[i].call);
        digest = oracle_fnv(digest, (uint64_t)s_steps[i].state_after);
    }
    printf("IDLE off: calls=240 ran=%u DIGEST %016llx PASS\n",
           (unsigned)s_ran, (unsigned long long)digest);
    lua_close(L);
    return 0;
}
#endif

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!strcmp(mode, "state-virtual"))
        return oracle_state_run(60000, 0, 40, 0, 1);
    if (!strcmp(mode, "state-real") && argc > 2) {
        int weak = !strcmp(argv[2], "weak");
        s_real_clock = 1;
        return oracle_state_run(300000, weak, 10, weak ? 2000 : 0, 0);
    }
    if (!strcmp(mode, "ctl") && argc > 2)
        return oracle_ctl_run(atoi(argv[2]));
#if defined(ISAAC_VITA_LUA_GC_IDLE_STEP)
    if (!strcmp(mode, "idle"))
        return oracle_idle_run();
#else
    if (!strcmp(mode, "idle-off"))
        return oracle_idle_off();
#endif
    printf("FAIL unknown mode '%s'\n", mode);
    return 2;
}
