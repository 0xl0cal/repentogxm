/* Lua GC policy at the frozen lua_gc import: the floor-transition GCCOLLECT
 * clamp (ISAAC_VITA_LUA_GCCOLLECT_CLAMP) and the per-window GC profile
 * (ISAAC_VITA_LUA_GC_PROFILE).  host_vita_lua.c routes its lua_gc import
 * through isaac_vita_lua_gc_call while either option is on; with both off
 * this unit is not compiled and the bridge is byte-identical.
 *
 * Finding (perf:wf-prof-v6 sampler, EID enabled): the Level-init family
 * sub_003b0e80 and its fragments (sub_003b1a90/3b26d3/3b2b25/3b548f/3b5499/
 * 3b57ea, seven entry points in the corpus) share one tail at RVA 0x3b585b:
 *
 *   3b585b  mov edi,[0xbfd674]         LuaEngine
 *   3b5861  mov esi,[IAT lua_gc]       0xa06238
 *   3b5867  push 0; push 2; push [edi+0x18]; call esi   -> ret 0x3b5870  (GCCOLLECT)
 *   3b5873  push 0; push 4; push [edi+0x18]; call esi   -> ret 0x3b587c  (GCCOUNTB)
 *   3b587f  push eax; push 0; push 3; ...; call esi     -> ret 0x3b5889  (GCCOUNT)
 *   printf("Lua mem usage: %d KB and %d bytes")
 *
 * (PE bytes 8b3d74d6bf00 8b353862a000 6a00 6a02 ff7718 ffd6 at 0x3b585b; the
 * corpus pushes return words as RVAs, so the single pin 0x003b5870 covers all
 * seven fragments.)  Every floor transition therefore runs luaC_fullgc: mark
 * the ~13 MB EID heap, sweep it and run every pending LuaBridge __gc
 * finalizer synchronously -- 130-286 ms per transition (ph120.hotc upd
 * 3b57ea in 13 of 16 windows with upd max > 700 ms).  The clamp replaces that
 * one call with lua_gc(LUA_GCSTEP, ISAAC_VITA_LUA_GCCOLLECT_STEP_KB) (or
 * nothing when the step is 0) and returns 0 exactly like LUA_GCCOLLECT; the
 * GCCOUNTB/GCCOUNT/printf that follow are untouched.  The other GCCOLLECT
 * site, LuaEngine::RunScript (sub_0040af40, ret 0x40b6ff, mod load), is left
 * alone; the remaining lua_gc sites are GCCOUNT/GCCOUNTB queries
 * (0x26a0e6/0x26a0fc/0x40e5b0/0x4022ec) and the paced GCSTEP of
 * LuaEngine::Update (ret 0x4023c6, see vita_lua_gcstep_clamp).
 *
 * Same behaviour class as the GCSTEP clamp: no script-visible value changes
 * other than the timing of __gc finalizers (they now run in the following
 * frames' paced steps instead of inside the transition) and the "Lua mem
 * usage" figure the game prints right after (the not-yet-collected size).
 * Lua's own allocation-debt pacing (pause 200 %) still bounds the heap.
 *
 * Profile: LuaEngine::Update calls lua_gc(L, LUA_GCSTEP, step) once per
 * frame from ret 0x4023c6.  Every 120 of those (one ph120 window) this unit
 * prints one bounded line: step n/sum/p95/max microseconds, finalizers run
 * (guest callbacks entered inside lua_gc; `fin` = targets inside the
 * LuaBridge CFunc::gcMetaMethod<T> stub ranges, `cb` = all), the worst
 * finalizer count of a single step, GCCOUNT at window end, arena
 * live/peak/fallbacks, requested vs applied step KB, the GCCOLLECT
 * clamp/full counts and how often a Lua longjmp unwound the nesting depth. */
#include "host_vita_lua_gc.h"

#include <string.h>
#include <time.h>

#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#endif

#if !defined(ISAAC_VITA_LUA_GCCOLLECT_CLAMP) && \
    !defined(ISAAC_VITA_LUA_GC_PROFILE)
#error "host_vita_lua_gc.c is compiled only for its opt-in features"
#endif

/* recomp/vita/platform.h; declared here so the host oracle can stub it. */
void isaac_vita_log(const char *format, ...);

/* LuaEngine::Update's paced GCSTEP (0x4023c0 call [IAT lua_gc]). */
#define VITA_LUA_GC_UPDATE_STEP_RETURN_RVA 0x004023c6U

static uint64_t vita_lua_gc_now_us(void)
{
#if defined(__vita__)
    return sceKernelGetProcessTimeWide();
#else
    return (uint64_t)clock() * 1000000ULL / (uint64_t)CLOCKS_PER_SEC;
#endif
}

static uint32_t vita_lua_gc_elapsed_us(uint64_t started)
{
    uint64_t now = vita_lua_gc_now_us();
    uint64_t elapsed = now > started ? now - started : 0U;
    return elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
}

/* The import handler runs with the guest's return word at [ESP]: the
 * translated call sites push their RVA (never BASE+rva). */
static uint32_t vita_lua_gc_site(CPU *__restrict c)
{
    uint32_t stack_address = guest_stack_address(c, c->esp, 4U, 0U);
    return stack_address ? ld32(stack_address) : 0U;
}

/* --- GCCOLLECT clamp ---------------------------------------------------- */
#if defined(ISAAC_VITA_LUA_GCCOLLECT_CLAMP)
#if !defined(ISAAC_VITA_LUA_GCCOLLECT_STEP_KB)
#define ISAAC_VITA_LUA_GCCOLLECT_STEP_KB 256
#endif

/* Return words of the Level-init family's GCCOLLECT call(s); pinned against
 * the PE bytes above.  One tail, seven fragments, one RVA. */
static const uint32_t s_vita_lua_gccollect_sites[] = {
    0x003b5870U
};

static uint32_t s_vita_lua_gccollect_clamps;

static int vita_lua_gccollect_site_pinned(uint32_t site)
{
    size_t i;
    for (i = 0U; i < sizeof s_vita_lua_gccollect_sites /
                     sizeof s_vita_lua_gccollect_sites[0]; ++i)
        if (s_vita_lua_gccollect_sites[i] == site)
            return 1;
    return 0;
}

static int vita_lua_gccollect_clamp(lua_State *state, uint32_t site)
{
    uint64_t started = vita_lua_gc_now_us();
    int before = lua_gc(state, LUA_GCCOUNT, 0);
    int after;
    uint32_t us;

#if ISAAC_VITA_LUA_GCCOLLECT_STEP_KB > 0
    (void)lua_gc(state, LUA_GCSTEP, ISAAC_VITA_LUA_GCCOLLECT_STEP_KB);
#endif
    after = lua_gc(state, LUA_GCCOUNT, 0);
    us = vita_lua_gc_elapsed_us(started);
    if (s_vita_lua_gccollect_clamps != UINT32_MAX)
        ++s_vita_lua_gccollect_clamps;
    isaac_vita_log("[isaac-lua] gccollect clamp: site=%08x -> step %u KB, "
                   "before=%u KB after=%u KB, us=%u",
                   (unsigned)site,
                   (unsigned)ISAAC_VITA_LUA_GCCOLLECT_STEP_KB,
                   (unsigned)(before < 0 ? 0 : before),
                   (unsigned)(after < 0 ? 0 : after),
                   (unsigned)us);
    return 0; /* lua_gc(L, LUA_GCCOLLECT, 0) returns 0 */
}
#endif

/* --- GC profile --------------------------------------------------------- */
#if defined(ISAAC_VITA_LUA_GC_PROFILE)
#define VITA_LUA_GC_WINDOW 120U

static uint32_t vita_lua_gc_add_saturating(uint32_t value, uint32_t add)
{
    return value > UINT32_MAX - add ? UINT32_MAX : value + add;
}

/* LuaBridge CFunc::gcMetaMethod<T> instantiations (PE census over the 130
 * "__gc" rawsetfield references): 65 identical 0x20-byte stubs
 * (push ebp; mov ecx,[ebp+8]; push typeid; call 0x7f8c80; ...; push 0;
 * call [edx] -- the virtual destructor) in two runs.  Callback targets are
 * BASE+rva (the guest passes absolute function pointers to
 * lua_pushcclosure). */
static const uint32_t s_vita_lua_gc_finalizer_ranges[][2] = {
    { 0x0045d450U, 0x0045db30U },
    { 0x004603a0U, 0x004604e0U }
};

typedef struct vita_lua_gc_profile {
    uint32_t depth;              /* nested lua_gc imports (finalizer re-entry) */
    uint32_t windows;
    uint32_t steps;              /* Update-site GCSTEP calls this window */
    uint32_t step_us[VITA_LUA_GC_WINDOW];
    uint32_t step_sum_us;
    uint32_t step_max_us;
    uint32_t requested_kb;       /* sum of the guest's step requests */
    uint32_t applied_kb;         /* sum after the GCSTEP clamp */
    uint32_t callbacks;          /* guest callbacks entered inside lua_gc */
    uint32_t finalizers;         /* ... whose target is a __gc stub */
    uint32_t finalizers_call;    /* finalizers inside the current lua_gc */
    uint32_t finalizers_max;     /* worst single lua_gc call */
    uint32_t collect_clamped;    /* GCCOLLECT calls replaced by a step */
    uint32_t collect_full;       /* GCCOLLECT calls run as full collections */
    uint32_t collect_us;         /* both kinds, summed */
    uint32_t other;              /* GCCOUNT/GCCOUNTB/... queries */
    uint32_t unwinds;            /* depth restored after a Lua longjmp */
} vita_lua_gc_profile;

static vita_lua_gc_profile s_vita_lua_gc_profile;

static int vita_lua_gc_target_is_finalizer(uint32_t target)
{
    uint32_t rva = target - (uint32_t)GUEST_IMAGE_BASE;
    size_t i;
    for (i = 0U; i < sizeof s_vita_lua_gc_finalizer_ranges /
                     sizeof s_vita_lua_gc_finalizer_ranges[0]; ++i)
        if (rva >= s_vita_lua_gc_finalizer_ranges[i][0] &&
            rva < s_vita_lua_gc_finalizer_ranges[i][1])
            return 1;
    return 0;
}

void isaac_vita_lua_gc_profile_note_callback(uint32_t target)
{
    vita_lua_gc_profile *p = &s_vita_lua_gc_profile;
    if (!p->depth)
        return;
    p->callbacks = vita_lua_gc_add_saturating(p->callbacks, 1U);
    if (vita_lua_gc_target_is_finalizer(target)) {
        p->finalizers = vita_lua_gc_add_saturating(p->finalizers, 1U);
        p->finalizers_call =
            vita_lua_gc_add_saturating(p->finalizers_call, 1U);
    }
}

/* A Lua error raised inside lua_gc (a __gc finalizer failing, or the
 * trampoline's "translated Lua callback fault") longjmps to the nearest
 * native lua_pcall past isaac_vita_lua_gc_call, skipping its depth
 * decrement.  The lua_pcallk import records the depth before the call and
 * restores it afterwards, exactly like its callback/require marks; without
 * that, every later lua_gc would look nested (no window ever printed again,
 * every callback counted as a finalizer). */
uint32_t isaac_vita_lua_gc_profile_depth(void)
{
    return s_vita_lua_gc_profile.depth;
}

void isaac_vita_lua_gc_profile_unwind(uint32_t depth)
{
    vita_lua_gc_profile *p = &s_vita_lua_gc_profile;
    if (p->depth <= depth)
        return;
    p->depth = depth;
    if (!depth)
        p->finalizers_call = 0U;
    p->unwinds = vita_lua_gc_add_saturating(p->unwinds, 1U);
}

static void vita_lua_gc_sort(uint32_t *values, uint32_t count)
{
    uint32_t i;
    for (i = 1U; i < count; ++i) {
        uint32_t value = values[i];
        uint32_t j = i;
        while (j != 0U && values[j - 1U] > value) {
            values[j] = values[j - 1U];
            --j;
        }
        values[j] = value;
    }
}

static void vita_lua_gc_profile_emit(lua_State *state)
{
    vita_lua_gc_profile *p = &s_vita_lua_gc_profile;
    uint32_t ordered[VITA_LUA_GC_WINDOW];
    uint32_t p95 = 0U;
    uint32_t live_kb = 0U;
    uint32_t peak_kb = 0U;
    uint32_t fallbacks = 0U;
    int count_kb;

    if (p->steps) {
        memcpy(ordered, p->step_us, p->steps * sizeof ordered[0]);
        vita_lua_gc_sort(ordered, p->steps);
        /* Nearest-rank percentile: ceil(0.95 * n) - 1. */
        p95 = ordered[(95U * p->steps + 99U) / 100U - 1U];
    }
    count_kb = lua_gc(state, LUA_GCCOUNT, 0);
    isaac_vita_lua_arena_stats(&live_kb, &peak_kb, &fallbacks);
    if (p->windows != UINT32_MAX)
        ++p->windows;
    isaac_vita_log("[isaac-lua] gc window: win=%u "
                   "gcstep(n,sum_us,p95_us,max_us)=%u,%u,%u,%u "
                   "fin(n,max_step,cb)=%u,%u,%u count=%u KB "
                   "arena(live,peak,fb)=%u,%u,%u kb(req,app)=%u,%u "
                   "collect(clamp,full,us)=%u,%u,%u other=%u unwind=%u",
                   (unsigned)p->windows,
                   (unsigned)p->steps, (unsigned)p->step_sum_us,
                   (unsigned)p95, (unsigned)p->step_max_us,
                   (unsigned)p->finalizers, (unsigned)p->finalizers_max,
                   (unsigned)p->callbacks,
                   (unsigned)(count_kb < 0 ? 0 : count_kb),
                   (unsigned)live_kb, (unsigned)peak_kb, (unsigned)fallbacks,
                   (unsigned)p->requested_kb, (unsigned)p->applied_kb,
                   (unsigned)p->collect_clamped, (unsigned)p->collect_full,
                   (unsigned)p->collect_us, (unsigned)p->other,
                   (unsigned)p->unwinds);
    /* Window reset keeps depth, windows and the in-flight finalizer count. */
    p->steps = 0U;
    p->step_sum_us = 0U;
    p->step_max_us = 0U;
    p->requested_kb = 0U;
    p->applied_kb = 0U;
    p->callbacks = 0U;
    p->finalizers = 0U;
    p->finalizers_max = 0U;
    p->collect_clamped = 0U;
    p->collect_full = 0U;
    p->collect_us = 0U;
    p->other = 0U;
    p->unwinds = 0U;
}

static void vita_lua_gc_profile_account(lua_State *state, uint32_t site,
                                        int what, int requested, int data,
                                        uint32_t us, int clamped)
{
    vita_lua_gc_profile *p = &s_vita_lua_gc_profile;

    if (p->finalizers_call > p->finalizers_max)
        p->finalizers_max = p->finalizers_call;
    if (what == LUA_GCCOLLECT) {
        if (clamped)
            p->collect_clamped =
                vita_lua_gc_add_saturating(p->collect_clamped, 1U);
        else
            p->collect_full =
                vita_lua_gc_add_saturating(p->collect_full, 1U);
        p->collect_us = vita_lua_gc_add_saturating(p->collect_us, us);
        return;
    }
    if (what != LUA_GCSTEP || site != VITA_LUA_GC_UPDATE_STEP_RETURN_RVA) {
        p->other = vita_lua_gc_add_saturating(p->other, 1U);
        return;
    }
    p->step_us[p->steps++] = us;
    p->step_sum_us = vita_lua_gc_add_saturating(p->step_sum_us, us);
    if (us > p->step_max_us)
        p->step_max_us = us;
    p->requested_kb = vita_lua_gc_add_saturating(
        p->requested_kb, requested > 0 ? (uint32_t)requested : 0U);
    p->applied_kb = vita_lua_gc_add_saturating(
        p->applied_kb, data > 0 ? (uint32_t)data : 0U);
    if (p->steps == VITA_LUA_GC_WINDOW)
        vita_lua_gc_profile_emit(state);
}
#endif

/* --- import endpoint ---------------------------------------------------- */
int isaac_vita_lua_gc_call(CPU *__restrict c, lua_State *state, int what,
                           int requested, int data)
{
    uint32_t site = vita_lua_gc_site(c);
    int clamped = 0;
    int result;
#if defined(ISAAC_VITA_LUA_GC_PROFILE)
    vita_lua_gc_profile *p = &s_vita_lua_gc_profile;
    int outer = p->depth == 0U;
    uint64_t started = 0U;

    if (outer) {
        started = vita_lua_gc_now_us();
        p->finalizers_call = 0U;
    }
    if (p->depth != UINT32_MAX)
        ++p->depth;
#else
    (void)requested;
#endif

#if defined(ISAAC_VITA_LUA_GCCOLLECT_CLAMP)
    if (what == LUA_GCCOLLECT && vita_lua_gccollect_site_pinned(site)) {
        result = vita_lua_gccollect_clamp(state, site);
        clamped = 1;
    } else
#endif
    {
        result = lua_gc(state, what, data);
    }

#if defined(ISAAC_VITA_LUA_GC_PROFILE)
    if (p->depth)
        --p->depth;
    if (outer)
        vita_lua_gc_profile_account(state, site, what, requested, data,
                                    vita_lua_gc_elapsed_us(started),
                                    clamped);
#else
    (void)clamped;
#endif
    return result;
}
