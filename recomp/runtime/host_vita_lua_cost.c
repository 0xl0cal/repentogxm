/* ISAAC_VITA_LUA_COST_PROFILE: split the time of mod Lua callbacks.
 *
 * Measured 2026-10-17 (session P, Downpour, EID): the callback invoker
 * 0x40eef0 costs ~1.34 ms per call, ~2.7 ms per game tick, and the sampler
 * attributes only ~1/4 of its render-phase samples to native code directly
 * under it.  The rest is translated code reached through LuaBridge bindings,
 * which the sampler cannot name per binding.  This unit times, at the only
 * three boundaries the time can cross:
 *   - every top-level lua_pcallk import (no callback/GC active): pc, and
 *     per pcall site (guest return word) ps;
 *   - every top-level Lua -> translated callback inside such a pcall: cb,
 *     and per callback target (bounded 64-slot table, overflow counted);
 *   - every outermost automatic GC step, via GNU ld --wrap=luaC_step,
 *     classified as gcvm (VM, inside pc), gccb (inside a callback's API call)
 *     or gcext (outside any pcall, e.g. LuaEngine::Update's paced step);
 *   - every luaM_realloc_ outside lmem.c (--wrap), counted, 1 in 64 timed.
 * Callbacks inside a GC step are __gc finalizers (fin), callbacks inside a
 * callback are nested (nest), callbacks outside any pcall are out.  A Lua
 * error that longjmps past a callback/GC step is closed at the pcall import
 * (lost).  Nothing here changes a Lua-visible value or an import's result. */
#include "host_vita_lua_cost.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#endif

#if !defined(ISAAC_VITA_LUA_COST_PROFILE)
#error "host_vita_lua_cost.c is compiled only for ISAAC_VITA_LUA_COST_PROFILE"
#endif

struct lua_State;

#define COST_WINDOW_US 2000000U
#define COST_SLOTS 64U
#define COST_SITES 8U
#define COST_TOP 8U
#define COST_SITES_SHOWN 4U
#define COST_ALLOC_SAMPLE 64U
#define COST_CLOCK_PROBE 64U

typedef struct cost_slot {
    uint32_t key;
    uint32_t n;
    uint32_t us;
} cost_slot;

typedef struct cost_state {
    uint32_t win;
    int started;
    uint64_t win_start;
    uint32_t pc_depth;
    uint32_t cb_depth;
    uint32_t gc_depth;
    /* timed activations (at most one of each kind is open) */
    int pc_open, cb_open, gc_open;
    uint64_t pc_start, cb_start, gc_start;
    uint32_t pc_site, cb_target, gc_kind;
    /* window totals */
    uint32_t pc_n, pc_us, cb_n, cb_us;
    uint32_t nest, fin, out, lost;
    uint32_t gc_n[3], gc_us[3];
    uint32_t al_n, al_timed, al_us;
    uint32_t ovf;
    cost_slot cb[COST_SLOTS];
    cost_slot ps[COST_SITES];
} cost_state;

enum { GC_VM = 0, GC_CB = 1, GC_EXT = 2 };

static cost_state s_cost;

void isaac_vita_log(const char *format, ...);

static uint64_t cost_now(void)
{
#if defined(ISAAC_VITA_LUA_COST_PROFILE_ORACLE)
    return isaac_vita_lua_cost_oracle_clock();
#elif defined(__vita__)
    return sceKernelGetProcessTimeWide();
#else
#error "ISAAC_VITA_LUA_COST_PROFILE needs the Vita clock or the oracle clock"
#endif
}

static void cost_sink(const char *line)
{
#if defined(ISAAC_VITA_LUA_COST_PROFILE_ORACLE)
    isaac_vita_lua_cost_oracle_sink(line);
#else
    isaac_vita_log("%s", line);
#endif
}

static uint32_t cost_elapsed(uint64_t start, uint64_t now)
{
    uint64_t d = now > start ? now - start : 0U;
    return d > 0xffffffffULL ? 0xffffffffU : (uint32_t)d;
}

static void cost_add(cost_slot *table, uint32_t size, uint32_t key,
                     uint32_t us, uint32_t *overflow)
{
    uint32_t i, h = (key * 2654435761U) % size;
    for (i = 0U; i < size; ++i) {
        cost_slot *s = &table[(h + i) % size];
        if (s->key == key || s->key == 0U) {
            s->key = key;
            ++s->n;
            s->us += us;
            return;
        }
    }
    ++*overflow;
}

/* Writes up to `shown` entries ordered by us, then n, then key (descending,
 * descending, ascending) as " key:n/us" into out. */
static size_t cost_top(const cost_slot *table, uint32_t size, uint32_t shown,
                       char *out, size_t cap)
{
    unsigned char used[COST_SLOTS];
    size_t len = 0U;
    uint32_t k, i;
    memset(used, 0, sizeof used);
    for (k = 0U; k < shown; ++k) {
        int best = -1;
        for (i = 0U; i < size; ++i) {
            const cost_slot *s = &table[i];
            const cost_slot *b;
            if (!s->key || used[i])
                continue;
            if (best < 0) {
                best = (int)i;
                continue;
            }
            b = &table[best];
            if (s->us > b->us || (s->us == b->us &&
                (s->n > b->n || (s->n == b->n && s->key < b->key))))
                best = (int)i;
        }
        if (best < 0)
            break;
        used[best] = 1U;
        if (len < cap) {
            int w = snprintf(out + len, cap - len, "%s%x:%u/%u", k ? "," : "",
                             (unsigned)table[best].key,
                             (unsigned)table[best].n,
                             (unsigned)table[best].us);
            if (w > 0)
                len += (size_t)w;
        }
    }
    if (len >= cap)
        len = cap - 1U;
    out[len] = '\0';
    return len;
}

static void cost_emit(uint64_t now)
{
    char line[384];
    char list[200];
    uint64_t c0, c1;
    uint32_t i, clk_ns;

    c0 = c1 = cost_now();
    for (i = 0U; i < COST_CLOCK_PROBE; ++i)
        c1 = cost_now();
    c1 = cost_now();
    clk_ns = (uint32_t)(cost_elapsed(c0, c1) * 1000U / (COST_CLOCK_PROBE + 1U));

    (void)cost_top(s_cost.ps, COST_SITES, COST_SITES_SHOWN, list, sizeof list);
    (void)snprintf(line, sizeof line,
        "[isaac-lua] cost win=%u ms=%u pc=%u/%u cb=%u/%u nest=%u fin=%u "
        "out=%u lost=%u gcvm=%u/%u gccb=%u/%u gcext=%u/%u al=%u/%u/%u "
        "clk=%u ovf=%u ps=%s",
        (unsigned)s_cost.win, (unsigned)(cost_elapsed(s_cost.win_start, now) / 1000U),
        (unsigned)s_cost.pc_n, (unsigned)s_cost.pc_us,
        (unsigned)s_cost.cb_n, (unsigned)s_cost.cb_us,
        (unsigned)s_cost.nest, (unsigned)s_cost.fin,
        (unsigned)s_cost.out, (unsigned)s_cost.lost,
        (unsigned)s_cost.gc_n[GC_VM], (unsigned)s_cost.gc_us[GC_VM],
        (unsigned)s_cost.gc_n[GC_CB], (unsigned)s_cost.gc_us[GC_CB],
        (unsigned)s_cost.gc_n[GC_EXT], (unsigned)s_cost.gc_us[GC_EXT],
        (unsigned)s_cost.al_n, (unsigned)s_cost.al_timed,
        (unsigned)s_cost.al_us, (unsigned)clk_ns, (unsigned)s_cost.ovf, list);
    cost_sink(line);
    (void)cost_top(s_cost.cb, COST_SLOTS, COST_TOP, list, sizeof list);
    (void)snprintf(line, sizeof line, "[isaac-lua] cost-top win=%u top=%s",
                   (unsigned)s_cost.win, list);
    cost_sink(line);

    /* New window: totals and tables only; open activations stay open and
     * are charged to the window in which they close. */
    s_cost.pc_n = s_cost.pc_us = s_cost.cb_n = s_cost.cb_us = 0U;
    s_cost.nest = s_cost.fin = s_cost.out = s_cost.lost = 0U;
    memset(s_cost.gc_n, 0, sizeof s_cost.gc_n);
    memset(s_cost.gc_us, 0, sizeof s_cost.gc_us);
    s_cost.al_n = s_cost.al_timed = s_cost.al_us = s_cost.ovf = 0U;
    memset(s_cost.cb, 0, sizeof s_cost.cb);
    memset(s_cost.ps, 0, sizeof s_cost.ps);
    ++s_cost.win;
    s_cost.win_start = cost_now();
}

static void cost_start(void)
{
    if (!s_cost.started) {
        s_cost.started = 1;
        s_cost.win_start = cost_now();
    }
}

static void cost_cb_close(uint64_t now)
{
    uint32_t us = cost_elapsed(s_cost.cb_start, now);
    s_cost.cb_open = 0;
    ++s_cost.cb_n;
    s_cost.cb_us += us;
    cost_add(s_cost.cb, COST_SLOTS, s_cost.cb_target, us, &s_cost.ovf);
}

static void cost_gc_close(uint64_t now)
{
    s_cost.gc_open = 0;
    ++s_cost.gc_n[s_cost.gc_kind];
    s_cost.gc_us[s_cost.gc_kind] += cost_elapsed(s_cost.gc_start, now);
}

void isaac_vita_lua_cost_pcall_enter(uint32_t ret, isaac_vita_lua_cost_mark *mark)
{
    cost_start();
    mark->cb_depth = s_cost.cb_depth;
    mark->gc_depth = s_cost.gc_depth;
    mark->top = !s_cost.pc_depth && !s_cost.cb_depth && !s_cost.gc_depth;
    ++s_cost.pc_depth;
    if (mark->top) {
        s_cost.pc_open = 1;
        s_cost.pc_site = ret;
        s_cost.pc_start = cost_now();
    }
}

void isaac_vita_lua_cost_pcall_leave(const isaac_vita_lua_cost_mark *mark)
{
    uint64_t now = cost_now();
    if (s_cost.cb_depth > mark->cb_depth) {
        s_cost.lost += s_cost.cb_depth - mark->cb_depth;
        if (s_cost.cb_open && mark->cb_depth == 0U)
            cost_cb_close(now);
        s_cost.cb_depth = mark->cb_depth;
    }
    if (s_cost.gc_depth > mark->gc_depth) {
        if (s_cost.gc_open && mark->gc_depth == 0U)
            cost_gc_close(now);
        s_cost.gc_depth = mark->gc_depth;
    }
    if (s_cost.pc_depth)
        --s_cost.pc_depth;
    if (mark->top && s_cost.pc_open) {
        uint32_t us = cost_elapsed(s_cost.pc_start, now);
        uint32_t ignored = 0U;
        s_cost.pc_open = 0;
        ++s_cost.pc_n;
        s_cost.pc_us += us;
        cost_add(s_cost.ps, COST_SITES, s_cost.pc_site, us, &ignored);
        s_cost.ovf += ignored;
        if (cost_elapsed(s_cost.win_start, now) >= COST_WINDOW_US)
            cost_emit(now);
    }
}

void isaac_vita_lua_cost_cb_enter(uint32_t target)
{
    cost_start();
    if (s_cost.gc_depth)
        ++s_cost.fin;
    else if (s_cost.cb_depth)
        ++s_cost.nest;
    else if (!s_cost.pc_depth)
        ++s_cost.out;
    else {
        s_cost.cb_open = 1;
        s_cost.cb_target = target;
        s_cost.cb_start = cost_now();
    }
    ++s_cost.cb_depth;
}

void isaac_vita_lua_cost_cb_leave(void)
{
    if (!s_cost.cb_depth)
        return;
    if (--s_cost.cb_depth == 0U && s_cost.cb_open)
        cost_cb_close(cost_now());
}

void isaac_vita_lua_cost_flush(void)
{
    cost_start();
    cost_emit(cost_now());
}

/* GNU ld --wrap targets (recomp/vita/CMakeLists.txt); host oracles get the
 * same edges by renaming at compile time (test_vita_lua_cost.py). */
void __real_luaC_step(struct lua_State *L);
void *__real_luaM_realloc_(struct lua_State *L, void *block, size_t osize,
                           size_t nsize);

void __wrap_luaC_step(struct lua_State *L)
{
    int outer = s_cost.gc_depth++ == 0U;
    if (outer) {
        cost_start();
        s_cost.gc_kind = !s_cost.pc_depth ? GC_EXT
                       : s_cost.cb_depth ? GC_CB : GC_VM;
        s_cost.gc_open = 1;
        s_cost.gc_start = cost_now();
    }
    __real_luaC_step(L);
    /* A Lua error inside the step longjmps past this point; the pcall
     * import closes it (isaac_vita_lua_cost_pcall_leave). */
    if (s_cost.gc_depth)
        --s_cost.gc_depth;
    if (outer && s_cost.gc_open)
        cost_gc_close(cost_now());
}

void *__wrap_luaM_realloc_(struct lua_State *L, void *block, size_t osize,
                           size_t nsize)
{
    void *result;
    uint64_t start;
    if (++s_cost.al_n % COST_ALLOC_SAMPLE != 0U)
        return __real_luaM_realloc_(L, block, osize, nsize);
    start = cost_now();
    result = __real_luaM_realloc_(L, block, osize, nsize);
    ++s_cost.al_timed;
    s_cost.al_us += cost_elapsed(start, cost_now());
    return result;
}
