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
 * alone; the remaining lua_gc sites are GCCOUNT/GCCOUNTB queries (0x26a0e6/
 * 0x26a0fc/0x40e5b0/0x4022ec/0x40b70b/0x40b718) and the paced GCSTEP of
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
    !defined(ISAAC_VITA_LUA_GC_PROFILE) && \
    !defined(ISAAC_VITA_LUA_GC_IDLE_STEP)
#error "host_vita_lua_gc.c is compiled only for its opt-in features"
#endif
#if defined(ISAAC_VITA_LUA_GC_STATE_PROFILE) && \
    !defined(ISAAC_VITA_LUA_GC_PROFILE)
#error "ISAAC_VITA_LUA_GC_STATE_PROFILE requires ISAAC_VITA_LUA_GC_PROFILE"
#endif
#if defined(ISAAC_VITA_LUA_GC_STATE_PROFILE)
#include <stddef.h>
#endif
#if defined(ISAAC_VITA_LUA_GC_IDLE_STEP)
/* the scheduler's idle-slack query, and the collector state of the step
 * about to run (lgc.h GCS*, lstate.h G(L)->gcstate; read only) */
#include "kage_vita_fullspeed_scheduler.h"
#include "host_vita_lua_gc_idle_rule.h"
#include "lfunc.h"
#include "lgc.h"
#include "lobject.h"
#include "lstate.h"
#endif

/* recomp/vita/platform.h; declared here so the host oracle can stub it. */
void isaac_vita_log(const char *format, ...);

/* LuaEngine::Update's paced GCSTEP (0x4023c0 call [IAT lua_gc]). */
#define VITA_LUA_GC_UPDATE_STEP_RETURN_RVA 0x004023c6U

static uint64_t vita_lua_gc_now_us(void)
{
#if defined(VITA_LUA_GC_ORACLE_CLOCK)
    /* host oracles only (test_vita_lua_gc_state_idle.py) */
    extern uint64_t vita_lua_gc_oracle_now_us(void);
    return vita_lua_gc_oracle_now_us();
#elif defined(__vita__)
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
#if defined(ISAAC_VITA_LUA_GC_IDLE_STEP)
/* time of the carried step the LUA_GCCOUNT import ran in front of this
 * Update step (vita_lua_gc_idle_settle): charged to that step, so the
 * gc window's sum stays the collector time of the Update site */
static uint32_t s_vita_lua_gc_idle_settled_us;
#endif

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

#if defined(ISAAC_VITA_LUA_GC_STATE_PROFILE)
static void vita_lua_gc_state_note_finalizer(void);
#endif

void isaac_vita_lua_gc_profile_note_callback(uint32_t target)
{
    vita_lua_gc_profile *p = &s_vita_lua_gc_profile;
#if defined(ISAAC_VITA_LUA_GC_STATE_PROFILE)
    /* also outside lua_gc: an allocation-driven step's finalizers */
    if (vita_lua_gc_target_is_finalizer(target))
        vita_lua_gc_state_note_finalizer();
#endif
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

#if defined(ISAAC_VITA_LUA_GC_STATE_PROFILE)
/* --- per-gcstate split (ISAAC_VITA_LUA_GC_STATE_PROFILE) ------------------
 * The derived lgc.c (lua53_gc_finalizer_pace.py --state-profile) calls the
 * three hooks below from luaC_step: begin after the gcrunning check, mark
 * whenever the class of work changes, end before the return.  A mark charges
 * the time since the previous mark to the previous class, so the classes of
 * one luaC_step sum to its begin..end time exactly (the interval before the
 * first mark goes to that first class).  Mark ids are lgc.h:39-46 gcstate
 * values for a singlestep entered in that state (GCSpropagate 0, GCSatomic
 * 1, GCSswpallgc 2, GCSswpfinobj 3, GCSswptobefnz 4, GCSswpend 5, GCScallfin
 * 6, GCSpause 7), 8..11 for the cuts inside atomic() and 12 for luaC_step's
 * tail runafewfinalizers (lua53_gc_finalizer_pace.py _STATE_EDITS).
 *
 * Origin of a luaC_step: `upd` inside LuaEngine::Update's paced lua_gc
 * (return word 0x4023c6), `other` inside any other lua_gc import, `alloc`
 * outside lua_gc -- an allocation-driven step (luaC_checkGC in the VM or
 * the API), which the gc window line cannot see at all.  luaC_fullgc does
 * not go through luaC_step and is not split (collect(...,us) has its time).
 * Two cases are NOT separated (both rare, both visible): an emergency
 * luaC_fullgc on an allocation failure inside an open luaC_step charges
 * its atomic marks to that step's classes; and a finalizer that calls
 * collectgarbage("step") nests a luaC_step, whose begin finds the outer
 * step open and counts it in `dropped` (so does a Lua error that unwinds
 * a step).  A window with dropped != 0 has an incomplete split.
 * The hooks read nothing but their arguments and write only this unit's
 * statics: the collector's decisions are those of the same build without
 * the marks (test_vita_lua_gc_state_idle.py compares the two). */
enum {
    VITA_GCS_PAUSE, VITA_GCS_PROP, VITA_GCS_ATOMIC, VITA_GCS_SWEEP,
    VITA_GCS_SWPEND, VITA_GCS_CALLFIN, VITA_GCS_TAILFIN, VITA_GCS_CLASSES
};
enum {
    VITA_GCA_ROOTS, VITA_GCA_AGAIN, VITA_GCA_WEAK, VITA_GCA_FNZ,
    VITA_GCA_TAIL, VITA_GCA_SUBS
};
enum { VITA_GCO_UPD, VITA_GCO_ALLOC, VITA_GCO_OTHER, VITA_GCO_ORIGINS };
#define VITA_GCS_MARKS 13U
#define VITA_GCS_NO_MARK 255U

static const uint8_t s_vita_gcs_class[VITA_GCS_MARKS] = {
    VITA_GCS_PROP, VITA_GCS_ATOMIC, VITA_GCS_SWEEP, VITA_GCS_SWEEP,
    VITA_GCS_SWEEP, VITA_GCS_SWPEND, VITA_GCS_CALLFIN, VITA_GCS_PAUSE,
    VITA_GCS_ATOMIC, VITA_GCS_ATOMIC, VITA_GCS_ATOMIC, VITA_GCS_ATOMIC,
    VITA_GCS_TAILFIN
};
/* atomic() sub-phase of a mark id; VITA_GCA_SUBS = not inside atomic */
static const uint8_t s_vita_gcs_sub[VITA_GCS_MARKS] = {
    VITA_GCA_SUBS, VITA_GCA_ROOTS, VITA_GCA_SUBS, VITA_GCA_SUBS,
    VITA_GCA_SUBS, VITA_GCA_SUBS, VITA_GCA_SUBS, VITA_GCA_SUBS,
    VITA_GCA_AGAIN, VITA_GCA_WEAK, VITA_GCA_FNZ, VITA_GCA_TAIL,
    VITA_GCA_SUBS
};
static const char *const s_vita_gcs_origin_name[VITA_GCO_ORIGINS] = {
    "upd", "alloc", "other"
};

typedef struct vita_lua_gc_state_step {
    uint32_t us;                           /* begin..end */
    uint32_t class_us[VITA_GCS_CLASSES];
    uint32_t sub_us[VITA_GCA_SUBS];
    uint32_t mask;                         /* classes this step entered */
    uint32_t finalizers;                   /* __gc stubs entered */
    uint32_t roots_kb;                     /* atomic: traversed from roots */
    uint32_t again_kb;                     /* atomic: 'grayagain' traversed */
    uint32_t estimate_kb;                  /* GCestimate at begin */
    uint32_t total_kb;                     /* gettotalbytes at begin */
    int32_t debt_in_kb;
    int32_t debt_out_kb;
    int8_t gcstate_in;
    int8_t gcstate_out;
    uint8_t origin;
} vita_lua_gc_state_step;

typedef struct vita_lua_gc_state {
    /* the luaC_step in flight */
    uint8_t open;
    uint8_t mark;
    uint8_t update_site;                   /* inside the 0x4023c6 lua_gc */
    uint64_t mark_us;
    uint64_t begin_us;
    vita_lua_gc_state_step step;
    /* window */
    uint32_t origin_n[VITA_GCO_ORIGINS];
    uint32_t origin_us[VITA_GCO_ORIGINS];
    uint32_t origin_max[VITA_GCO_ORIGINS];
    uint32_t class_n[VITA_GCS_CLASSES];    /* steps that entered the class */
    uint32_t class_us[VITA_GCS_CLASSES];
    uint32_t class_max[VITA_GCS_CLASSES];  /* worst single step, this class */
    uint32_t sub_us[VITA_GCA_SUBS];
    uint32_t atomic_n[VITA_GCO_ORIGINS];
    uint32_t atomic_max_us;
    uint32_t atomic_roots_kb;              /* of the atomic_max_us step */
    uint32_t atomic_again_kb;
    uint32_t alloc_finalizers;             /* __gc run by alloc-origin steps */
    uint32_t dropped;                      /* begin without end (longjmp) */
    vita_lua_gc_state_step worst;
    uint32_t banner;
} vita_lua_gc_state;

static vita_lua_gc_state s_vita_lua_gc_state;

static uint32_t vita_lua_gc_state_kb(size_t bytes)
{
    return (uint32_t)(bytes >> 10);
}

static void vita_lua_gc_state_charge(vita_lua_gc_state *s, uint64_t now)
{
    uint64_t elapsed = now > s->mark_us ? now - s->mark_us : 0U;
    uint32_t us = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
    uint32_t sub;

    s->mark_us = now;
    if (s->mark >= VITA_GCS_MARKS)
        return;
    s->step.class_us[s_vita_gcs_class[s->mark]] = vita_lua_gc_add_saturating(
        s->step.class_us[s_vita_gcs_class[s->mark]], us);
    sub = s_vita_gcs_sub[s->mark];
    if (sub < VITA_GCA_SUBS)
        s->step.sub_us[sub] =
            vita_lua_gc_add_saturating(s->step.sub_us[sub], us);
}

void isaac_lua_gc_state_begin(ptrdiff_t debt, size_t estimate, size_t total,
                              int gcstate);
void isaac_lua_gc_state_mark(int cls, size_t info);
void isaac_lua_gc_state_end(ptrdiff_t debt, int gcstate);

void isaac_lua_gc_state_begin(ptrdiff_t debt, size_t estimate, size_t total,
                              int gcstate)
{
    vita_lua_gc_state *s = &s_vita_lua_gc_state;

    if (s->open)
        s->dropped = vita_lua_gc_add_saturating(s->dropped, 1U);
    memset(&s->step, 0, sizeof s->step);
    s->open = 1U;
    s->mark = VITA_GCS_NO_MARK;
    s->step.origin = !s_vita_lua_gc_profile.depth ? VITA_GCO_ALLOC :
        s->update_site ? VITA_GCO_UPD : VITA_GCO_OTHER;
    s->step.debt_in_kb = (int32_t)(debt / 1024);
    s->step.estimate_kb = vita_lua_gc_state_kb(estimate);
    s->step.total_kb = vita_lua_gc_state_kb(total);
    s->step.gcstate_in = (int8_t)gcstate;
    s->begin_us = s->mark_us = vita_lua_gc_now_us();
}

void isaac_lua_gc_state_mark(int cls, size_t info)
{
    vita_lua_gc_state *s = &s_vita_lua_gc_state;

    /* atomic() also runs under luaC_fullgc, outside any luaC_step */
    if (!s->open || cls < 0 || (uint32_t)cls >= VITA_GCS_MARKS)
        return;
    if (s->mark != VITA_GCS_NO_MARK)
        vita_lua_gc_state_charge(s, vita_lua_gc_now_us());
    s->mark = (uint8_t)cls;
    s->step.mask |= 1U << s_vita_gcs_class[cls];
    if (cls == 8)
        s->step.roots_kb = vita_lua_gc_state_kb(info);
    else if (cls == 9)
        s->step.again_kb = vita_lua_gc_state_kb(info);
}

void isaac_lua_gc_state_end(ptrdiff_t debt, int gcstate)
{
    vita_lua_gc_state *s = &s_vita_lua_gc_state;
    vita_lua_gc_state_step *t = &s->step;
    uint64_t now;
    uint32_t i;

    if (!s->open)
        return;
    now = vita_lua_gc_now_us();
    t->us = now > s->begin_us ? (now - s->begin_us > UINT32_MAX ?
        UINT32_MAX : (uint32_t)(now - s->begin_us)) : 0U;
    vita_lua_gc_state_charge(s, now);
    s->open = 0U;
    t->debt_out_kb = (int32_t)(debt / 1024);
    t->gcstate_out = (int8_t)gcstate;

    s->origin_n[t->origin] =
        vita_lua_gc_add_saturating(s->origin_n[t->origin], 1U);
    s->origin_us[t->origin] =
        vita_lua_gc_add_saturating(s->origin_us[t->origin], t->us);
    if (t->us > s->origin_max[t->origin])
        s->origin_max[t->origin] = t->us;
    for (i = 0U; i < VITA_GCS_CLASSES; ++i) {
        if (!(t->mask & (1U << i)))
            continue;
        s->class_n[i] = vita_lua_gc_add_saturating(s->class_n[i], 1U);
        s->class_us[i] =
            vita_lua_gc_add_saturating(s->class_us[i], t->class_us[i]);
        if (t->class_us[i] > s->class_max[i])
            s->class_max[i] = t->class_us[i];
    }
    for (i = 0U; i < VITA_GCA_SUBS; ++i)
        s->sub_us[i] = vita_lua_gc_add_saturating(s->sub_us[i], t->sub_us[i]);
    if (t->mask & (1U << VITA_GCS_ATOMIC)) {
        s->atomic_n[t->origin] =
            vita_lua_gc_add_saturating(s->atomic_n[t->origin], 1U);
        if (t->class_us[VITA_GCS_ATOMIC] >= s->atomic_max_us) {
            s->atomic_max_us = t->class_us[VITA_GCS_ATOMIC];
            s->atomic_roots_kb = t->roots_kb;
            s->atomic_again_kb = t->again_kb;
        }
    }
    if (t->origin == VITA_GCO_ALLOC)
        s->alloc_finalizers =
            vita_lua_gc_add_saturating(s->alloc_finalizers, t->finalizers);
    if (t->us >= s->worst.us)
        s->worst = *t;
}

static void vita_lua_gc_state_note_finalizer(void)
{
    vita_lua_gc_state *s = &s_vita_lua_gc_state;
    if (s->open)
        s->step.finalizers =
            vita_lua_gc_add_saturating(s->step.finalizers, 1U);
}

static void vita_lua_gc_state_emit(unsigned window)
{
    vita_lua_gc_state *s = &s_vita_lua_gc_state;
    const vita_lua_gc_state_step *w = &s->worst;
    uint32_t banner = s->banner;
    uint8_t update_site = s->update_site;

    if (!banner)
        isaac_vita_log("[isaac-lua] gc state profile: on=1 marks=%u "
                       "classes=%u window=%u update steps",
                       (unsigned)VITA_GCS_MARKS, (unsigned)VITA_GCS_CLASSES,
                       (unsigned)VITA_LUA_GC_WINDOW);
    isaac_vita_log("[isaac-lua] gc state: win=%u step(n,us,max) "
                   "upd=%u,%u,%u alloc=%u,%u,%u other=%u,%u,%u "
                   "atomic(upd,alloc,other,max_us,roots_kb,again_kb)="
                   "%u,%u,%u,%u,%u,%u fin_alloc=%u dropped=%u",
                   window,
                   (unsigned)s->origin_n[VITA_GCO_UPD],
                   (unsigned)s->origin_us[VITA_GCO_UPD],
                   (unsigned)s->origin_max[VITA_GCO_UPD],
                   (unsigned)s->origin_n[VITA_GCO_ALLOC],
                   (unsigned)s->origin_us[VITA_GCO_ALLOC],
                   (unsigned)s->origin_max[VITA_GCO_ALLOC],
                   (unsigned)s->origin_n[VITA_GCO_OTHER],
                   (unsigned)s->origin_us[VITA_GCO_OTHER],
                   (unsigned)s->origin_max[VITA_GCO_OTHER],
                   (unsigned)s->atomic_n[VITA_GCO_UPD],
                   (unsigned)s->atomic_n[VITA_GCO_ALLOC],
                   (unsigned)s->atomic_n[VITA_GCO_OTHER],
                   (unsigned)s->atomic_max_us,
                   (unsigned)s->atomic_roots_kb,
                   (unsigned)s->atomic_again_kb,
                   (unsigned)s->alloc_finalizers, (unsigned)s->dropped);
    isaac_vita_log("[isaac-lua] gc cls: win=%u (n,us,max) pause=%u,%u,%u "
                   "prop=%u,%u,%u atomic=%u,%u,%u sweep=%u,%u,%u "
                   "swpend=%u,%u,%u callfin=%u,%u,%u tailfin=%u,%u,%u",
                   window,
                   (unsigned)s->class_n[VITA_GCS_PAUSE],
                   (unsigned)s->class_us[VITA_GCS_PAUSE],
                   (unsigned)s->class_max[VITA_GCS_PAUSE],
                   (unsigned)s->class_n[VITA_GCS_PROP],
                   (unsigned)s->class_us[VITA_GCS_PROP],
                   (unsigned)s->class_max[VITA_GCS_PROP],
                   (unsigned)s->class_n[VITA_GCS_ATOMIC],
                   (unsigned)s->class_us[VITA_GCS_ATOMIC],
                   (unsigned)s->class_max[VITA_GCS_ATOMIC],
                   (unsigned)s->class_n[VITA_GCS_SWEEP],
                   (unsigned)s->class_us[VITA_GCS_SWEEP],
                   (unsigned)s->class_max[VITA_GCS_SWEEP],
                   (unsigned)s->class_n[VITA_GCS_SWPEND],
                   (unsigned)s->class_us[VITA_GCS_SWPEND],
                   (unsigned)s->class_max[VITA_GCS_SWPEND],
                   (unsigned)s->class_n[VITA_GCS_CALLFIN],
                   (unsigned)s->class_us[VITA_GCS_CALLFIN],
                   (unsigned)s->class_max[VITA_GCS_CALLFIN],
                   (unsigned)s->class_n[VITA_GCS_TAILFIN],
                   (unsigned)s->class_us[VITA_GCS_TAILFIN],
                   (unsigned)s->class_max[VITA_GCS_TAILFIN]);
    isaac_vita_log("[isaac-lua] gc asub: win=%u "
                   "us(roots,again,weak,fnz,tail)=%u,%u,%u,%u,%u",
                   window,
                   (unsigned)s->sub_us[VITA_GCA_ROOTS],
                   (unsigned)s->sub_us[VITA_GCA_AGAIN],
                   (unsigned)s->sub_us[VITA_GCA_WEAK],
                   (unsigned)s->sub_us[VITA_GCA_FNZ],
                   (unsigned)s->sub_us[VITA_GCA_TAIL]);
    /* the window's longest luaC_step; cls_us and asub_us in the order of
     * the gc cls and gc asub lines */
    isaac_vita_log("[isaac-lua] gc worst: win=%u us=%u origin=%s "
                   "gcstate=%d>%d cls_us=%u,%u,%u,%u,%u,%u,%u "
                   "asub_us=%u,%u,%u,%u,%u fin=%u debt_kb=%d>%d "
                   "est_kb=%u tot_kb=%u roots_kb=%u again_kb=%u",
                   window, (unsigned)w->us,
                   s_vita_gcs_origin_name[w->origin],
                   (int)w->gcstate_in, (int)w->gcstate_out,
                   (unsigned)w->class_us[VITA_GCS_PAUSE],
                   (unsigned)w->class_us[VITA_GCS_PROP],
                   (unsigned)w->class_us[VITA_GCS_ATOMIC],
                   (unsigned)w->class_us[VITA_GCS_SWEEP],
                   (unsigned)w->class_us[VITA_GCS_SWPEND],
                   (unsigned)w->class_us[VITA_GCS_CALLFIN],
                   (unsigned)w->class_us[VITA_GCS_TAILFIN],
                   (unsigned)w->sub_us[VITA_GCA_ROOTS],
                   (unsigned)w->sub_us[VITA_GCA_AGAIN],
                   (unsigned)w->sub_us[VITA_GCA_WEAK],
                   (unsigned)w->sub_us[VITA_GCA_FNZ],
                   (unsigned)w->sub_us[VITA_GCA_TAIL],
                   (unsigned)w->finalizers,
                   (int)w->debt_in_kb, (int)w->debt_out_kb,
                   (unsigned)w->estimate_kb, (unsigned)w->total_kb,
                   (unsigned)w->roots_kb, (unsigned)w->again_kb);
    /* Window reset; no luaC_step is in flight here (the window closes after
     * the Update-site lua_gc returned). */
    memset(s, 0, sizeof *s);
    s->banner = 1U;
    s->update_site = update_site;
}
#endif

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
#if defined(ISAAC_VITA_LUA_GC_STATE_PROFILE)
    vita_lua_gc_state_emit((unsigned)p->windows);
#endif
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
#if defined(ISAAC_VITA_LUA_GC_IDLE_STEP)
    us = vita_lua_gc_add_saturating(us, s_vita_lua_gc_idle_settled_us);
    s_vita_lua_gc_idle_settled_us = 0U;
#endif
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

#if defined(ISAAC_VITA_LUA_GC_IDLE_STEP)
/* --- paced step in the loop's idle slack (ISAAC_VITA_LUA_GC_IDLE_STEP) ----
 * LuaEngine::Update (sub_004022d0) is the last guest work of a main-loop
 * iteration, so the next thing the thread does is the scheduler's loop head
 * (kage_vita_fullspeed_scheduler.c).  The function makes two lua_gc calls:
 *
 *   4022e6  call [IAT lua_gc]  (L, LUA_GCCOUNT, 0)   -> ret 0x4022ec
 *           ... the game's own step controller: a ring of the last ten
 *           count deltas at [edi+0x28], k at [edi+0x24] (all deltas >= 0 and
 *           their sum > 0: k = min(k + sum/10, 10000) and a 600-call
 *           countdown at [edi+0x54] restarts; otherwise, once the countdown
 *           is 0, k -= max(k/100, 10); floor 50) ...
 *   4023c0  call [IAT lua_gc]  (L, LUA_GCSTEP, k)    -> ret 0x4023c6
 *
 * At the GCSTEP import the unit asks the scheduler what the loop head leads
 * to.  On BUSY (this tick ran Game::Update, the head will not sleep, the
 * non-full tick with the interpolated render is next) the step is carried
 * when host_vita_lua_gc_idle_rule.h allows it: only a step that cannot run
 * atomic() at this tail -- the collector is sweeping or calling finalizers,
 * or it is propagating and the gray objects already queued cost more than
 * the step's budget.  A step that is or may reach the atomic, a step that
 * restarts the cycle and a call that would run no luaC_step run now, as in
 * the build without the knob.  On FREE / LATE / UNKNOWN the step runs now.
 *
 * A carried step runs at the START of the next LuaEngine::Update GC
 * sequence: in the LUA_GCCOUNT import at 0x4022ec, before the count is
 * taken.  The game's controller therefore reads the heap after the carried
 * step, as it does without the knob (the collector's work between two
 * counts is the same steps; only allocation-driven luaC_steps of the
 * interpolated render see a collector one step behind).  Should that import
 * not arrive, the GCSTEP import runs the carried step first.
 *
 * Steps are never split or merged: the finalizer pacer charges per step
 * (one step sweeps ISAAC_VITA_LUA_GC_SWEEP_STEP objects and runs
 * ISAAC_VITA_LUA_GC_FINALIZER_STEP finalizers whatever its budget), so two
 * k/2 steps are not one k step.  At most VITA_LUA_GC_IDLE_OWED_MAX step is
 * carried, so the collector is never more than one Update step behind the
 * build without the knob.  The carried step is dropped when the lua_State
 * changes (a new LuaEngine).
 *
 * What the rule bounds: the work moved behind the interpolated render is
 * one non-atomic step (on the device, 2026-11-12, clamp 64 KB: <= 2.4 ms).  The
 * state is read when the game issues the step; allocation-driven steps of
 * the render in between can advance the collector, so a carried step can
 * still be the one that runs the atomic -- then the atomic runs at the tail
 * where the build without the knob runs it as well (that tail's own step),
 * and the extra work there is the one small step.  `catomic` counts it.
 *
 * Same context as today's call: the same function of the guest, the same
 * CPU, the same lua_State, so the __gc finalizers a carried step runs enter
 * the guest exactly as the game's own step does.  A Lua error raised by a
 * finalizer of the carried step leaves through the LUA_GCCOUNT import, i.e.
 * out of sub_004022d0 before it issues this tick's step -- as an error in
 * the build without the knob leaves through its GCSTEP import; no requested
 * k is lost by this unit.  The GCSTEP import returns 0 for a carried step;
 * the game does not read it (0x4023c6 add esp,0xc ... ret, and the caller's
 * next instruction at 0x48be0d overwrites EAX). */
#define VITA_LUA_GC_UPDATE_COUNT_RETURN_RVA 0x004022ecU
#define VITA_LUA_GC_IDLE_OWED_MAX 1U
#define VITA_LUA_GC_IDLE_WINDOW 120U
/* gray objects read to prove a propagate step short of the atomic */
#define VITA_LUA_GC_IDLE_WALK_MAX 512U
/* lgc.c:52 `#define STEPMULADJ 200` (not in a header;
 * test_vita_lua_gc_state_idle.py pins the line of the pristine and of the
 * derived lgc.c against this value) */
#define VITA_LUA_GC_IDLE_STEPMULADJ 200
enum {
    VITA_GCI_IDLE,     /* FREE tail, the step fitted the slack left */
    VITA_GCI_OVER,     /* FREE tail, the step ran past the slack */
    VITA_GCI_LATE,     /* LATE tail */
    VITA_GCI_BUSY,     /* BUSY tail: not carried, or carried into one */
    VITA_GCI_UNKNOWN,  /* scheduler not in charge: as without the knob */
    VITA_GCI_PLACES
};

typedef struct vita_lua_gc_idle {
    lua_State *state;
    int owed_kb[VITA_LUA_GC_IDLE_OWED_MAX];
    uint32_t owed;
    uint32_t banner;
    uint32_t windows;
    uint32_t tail_slack_counted; /* this tail's slack is in slack_us */
    /* window */
    uint32_t calls;
    uint32_t kinds[4];
    uint32_t deferred;
    uint32_t ran[VITA_GCI_PLACES];
    uint32_t atomic[VITA_GCI_PLACES];
    uint32_t place_us[VITA_GCI_PLACES];
    uint32_t over_us;            /* time the OVER steps ran past the slack */
    uint32_t slack_us;           /* slack offered by the FREE tails */
    uint32_t owed_kb_max;
    uint32_t requested_kb;
    uint32_t ran_kb;
    uint32_t owed_in_kb;         /* carried into this window */
    uint32_t dropped_kb;
    uint32_t max_us;
    uint32_t dropped;
    uint32_t busy[VITA_GCI_PHASES];   /* BUSY tails by the step's phase */
    uint32_t carry[VITA_GCI_PHASES];  /* ... of which carried */
    uint32_t walks;              /* propagate steps whose gray list was read */
    uint32_t walk_objects;
    uint32_t walk_max;
    uint32_t settle_count;       /* carried steps run at the GCCOUNT import */
    uint32_t settle_step;        /* ... at the GCSTEP import (no count seen) */
    uint32_t carried_atomic;     /* carried steps that ran atomic() */
} vita_lua_gc_idle;

static vita_lua_gc_idle s_vita_lua_gc_idle;

static uint32_t vita_lua_gc_idle_add(uint32_t value, uint32_t add)
{
    return value > UINT32_MAX - add ? UINT32_MAX : value + add;
}

static uint32_t vita_lua_gc_idle_kb(int kb)
{
    return kb > 0 ? (uint32_t)kb : 0U;
}

/* Propagate: do the gray objects queued right now cost more than the step
 * can pay?  luaC_step repeats singlestep while debt > -GCSTEPSIZE
 * (lgc.c:1133-1136); a propagate singlestep takes ONE object off g->gray
 * and returns what its traversal counted (lgc.c:1050-1057, propagatemark
 * 560-599); GCSatomic is entered only when the list is empty.  Every object
 * queued now must be traversed before that, so before the last of them the
 * loop has paid at least the sum of the others.  If that sum reaches
 * budget + GCSTEPSIZE the loop has stopped with the list non-empty: the
 * step ends in GCSpropagate.  The sizes are lower bounds of what the
 * traversals return: tables exactly (lgc.c:469-470), closures exactly
 * (lgc.c:505, 526), a proto at least sizeof(Proto) (lgc.c:492), a thread at
 * least 1 (lgc.c:533).  Read only; at most VITA_LUA_GC_IDLE_WALK_MAX links
 * are followed, and anything unexpected is "not proven". */
static int vita_lua_gc_idle_prop_bound(global_State *g, l_mem budget,
                                       uint32_t *objects)
{
    GCObject *o = g->gray;
    lu_mem need = (lu_mem)budget + (lu_mem)GCSTEPSIZE;
    lu_mem sum = 0U;
    lu_mem largest = 0U;
    uint32_t n = 0U;

    while (o != NULL && n < VITA_LUA_GC_IDLE_WALK_MAX) {
        lu_mem size;

        switch (o->tt) {
        case LUA_TTABLE: {
            Table *h = gco2t(o);
            size = sizeof(Table) + sizeof(TValue) * h->sizearray +
                sizeof(Node) * cast(size_t, sizenode(h));
            o = h->gclist;
            break;
        }
        case LUA_TLCL: {
            LClosure *cl = gco2lcl(o);
            size = (lu_mem)sizeLclosure(cl->nupvalues);
            o = cl->gclist;
            break;
        }
        case LUA_TCCL: {
            CClosure *cl = gco2ccl(o);
            size = (lu_mem)sizeCclosure(cl->nupvalues);
            o = cl->gclist;
            break;
        }
        case LUA_TTHREAD:
            size = 1U;
            o = gco2th(o)->gclist;
            break;
        case LUA_TPROTO:
            size = sizeof(Proto);
            o = gco2p(o)->gclist;
            break;
        default:
            *objects = n;
            return 0;
        }
        ++n;
        sum += size;
        if (size > largest)
            largest = size;
        if (sum - largest >= need) {
            *objects = n;
            return 1;
        }
    }
    *objects = n;
    return 0;
}

/* The phase (host_vita_lua_gc_idle_rule.h) of the step lua_gc(L,
 * LUA_GCSTEP, kb) would run right now. */
static int vita_lua_gc_idle_phase(lua_State *state, int kb)
{
    vita_lua_gc_idle *q = &s_vita_lua_gc_idle;
    global_State *g = G(state);
    l_mem debt = 0;                    /* kb == 0: lapi.c:1072, lgc.c:1116 */

    if (kb != 0) {
        debt = (l_mem)kb * 1024 + g->GCdebt;           /* lapi.c:1076 */
        if (debt <= 0)                                  /* lgc.h:110 */
            return VITA_GCI_PHASE_NOOP;
        debt = debt / VITA_LUA_GC_IDLE_STEPMULADJ + 1;  /* lgc.c:1117 */
        debt = debt < MAX_LMEM / g->gcstepmul ?         /* lgc.c:1118 */
            debt * g->gcstepmul : MAX_LMEM;
    }
    switch (g->gcstate) {
    case GCSpause:
        return VITA_GCI_PHASE_PAUSE;
    case GCSpropagate: {
        uint32_t objects = 0U;
        int bound = vita_lua_gc_idle_prop_bound(g, debt, &objects);

        q->walks = vita_lua_gc_idle_add(q->walks, 1U);
        q->walk_objects = vita_lua_gc_idle_add(q->walk_objects, objects);
        if (objects > q->walk_max)
            q->walk_max = objects;
        return bound ? VITA_GCI_PHASE_PROP_BOUND : VITA_GCI_PHASE_PROP;
    }
    case GCSswpallgc:
    case GCSswpfinobj:
    case GCSswptobefnz:
    case GCSswpend:
        return VITA_GCI_PHASE_SWEEP;
    case GCScallfin:
        return VITA_GCI_PHASE_CALLFIN;
    default:                           /* GCSatomic, or a value not known */
        return VITA_GCI_PHASE_ATOMIC;
    }
}

static int vita_lua_gc_idle_run(lua_State *state, int kb, int kind,
                                uint32_t *slack_left, int carried)
{
    vita_lua_gc_idle *q = &s_vita_lua_gc_idle;
    uint64_t started = vita_lua_gc_now_us();
    lu_byte white = G(state)->currentwhite;
    int result = lua_gc(state, LUA_GCSTEP, kb);
    uint32_t us = vita_lua_gc_elapsed_us(started);
    /* atomic() flips the current white (lgc.c:1020) and nothing else in a
     * luaC_step does; a step runs at most one atomic (its loop ends at
     * GCSpause), and so does an emergency luaC_fullgc inside it. */
    int atomic = ((white ^ G(state)->currentwhite) & WHITEBITS) != 0;
    uint32_t place;

    if (kind == KAGE_VITA_GC_IDLE_FREE) {
        if (us <= *slack_left) {
            place = VITA_GCI_IDLE;
            *slack_left -= us;
        } else {
            place = VITA_GCI_OVER;
            q->over_us = vita_lua_gc_idle_add(q->over_us, us - *slack_left);
            *slack_left = 0U;
        }
    } else if (kind == KAGE_VITA_GC_IDLE_LATE) {
        place = VITA_GCI_LATE;
    } else if (kind == KAGE_VITA_GC_IDLE_BUSY) {
        place = VITA_GCI_BUSY;
    } else {
        place = VITA_GCI_UNKNOWN;
    }
    q->ran[place] = vita_lua_gc_idle_add(q->ran[place], 1U);
    q->place_us[place] = vita_lua_gc_idle_add(q->place_us[place], us);
    if (atomic) {
        q->atomic[place] = vita_lua_gc_idle_add(q->atomic[place], 1U);
        if (carried)
            q->carried_atomic = vita_lua_gc_idle_add(q->carried_atomic, 1U);
    }
    q->ran_kb = vita_lua_gc_idle_add(q->ran_kb, vita_lua_gc_idle_kb(kb));
    if (us > q->max_us)
        q->max_us = us;
    return result;
}

static void vita_lua_gc_idle_emit(void)
{
    vita_lua_gc_idle *q = &s_vita_lua_gc_idle;
    uint32_t owed_out_kb = q->owed ? vita_lua_gc_idle_kb(q->owed_kb[0]) : 0U;

    if (q->windows != UINT32_MAX)
        ++q->windows;
    isaac_vita_log("[isaac-lua] gc idle: win=%u calls=%u "
                   "kind(unk,free,busy,late)=%u,%u,%u,%u deferred=%u "
                   "ran(idle,over,late,busy,unk)=%u,%u,%u,%u,%u "
                   "atomic(idle,over,late,busy,unk)=%u,%u,%u,%u,%u "
                   "dropped=%u",
                   (unsigned)q->windows, (unsigned)q->calls,
                   (unsigned)q->kinds[KAGE_VITA_GC_IDLE_UNKNOWN],
                   (unsigned)q->kinds[KAGE_VITA_GC_IDLE_FREE],
                   (unsigned)q->kinds[KAGE_VITA_GC_IDLE_BUSY],
                   (unsigned)q->kinds[KAGE_VITA_GC_IDLE_LATE],
                   (unsigned)q->deferred,
                   (unsigned)q->ran[VITA_GCI_IDLE],
                   (unsigned)q->ran[VITA_GCI_OVER],
                   (unsigned)q->ran[VITA_GCI_LATE],
                   (unsigned)q->ran[VITA_GCI_BUSY],
                   (unsigned)q->ran[VITA_GCI_UNKNOWN],
                   (unsigned)q->atomic[VITA_GCI_IDLE],
                   (unsigned)q->atomic[VITA_GCI_OVER],
                   (unsigned)q->atomic[VITA_GCI_LATE],
                   (unsigned)q->atomic[VITA_GCI_BUSY],
                   (unsigned)q->atomic[VITA_GCI_UNKNOWN],
                   (unsigned)q->dropped);
    /* kb: req + owed_kb.in == ran + owed_kb.out + owed_kb.drop (a step
     * requested by the window's last call may still be owed at its close) */
    isaac_vita_log("[isaac-lua] gc idle us: win=%u "
                   "us(idle,over,late,busy,unk)=%u,%u,%u,%u,%u "
                   "over_us=%u slack_us=%u owed_kb_max=%u kb(req,ran)=%u,%u "
                   "owed_kb(in,out,drop)=%u,%u,%u max_us=%u",
                   (unsigned)q->windows,
                   (unsigned)q->place_us[VITA_GCI_IDLE],
                   (unsigned)q->place_us[VITA_GCI_OVER],
                   (unsigned)q->place_us[VITA_GCI_LATE],
                   (unsigned)q->place_us[VITA_GCI_BUSY],
                   (unsigned)q->place_us[VITA_GCI_UNKNOWN],
                   (unsigned)q->over_us, (unsigned)q->slack_us,
                   (unsigned)q->owed_kb_max,
                   (unsigned)q->requested_kb, (unsigned)q->ran_kb,
                   (unsigned)q->owed_in_kb, (unsigned)owed_out_kb,
                   (unsigned)q->dropped_kb, (unsigned)q->max_us);
    /* why: the BUSY tails by the phase of the step issued there, and the
     * ones carried (the rule admits propb, sweep and callfin only) */
    isaac_vita_log("[isaac-lua] gc idle why: win=%u "
                   "busy(noop,pause,prop,propb,atomic,sweep,callfin)="
                   "%u,%u,%u,%u,%u,%u,%u carry(propb,sweep,callfin)=%u,%u,%u "
                   "walk(n,objs,max)=%u,%u,%u settle(count,step)=%u,%u "
                   "catomic=%u",
                   (unsigned)q->windows,
                   (unsigned)q->busy[VITA_GCI_PHASE_NOOP],
                   (unsigned)q->busy[VITA_GCI_PHASE_PAUSE],
                   (unsigned)q->busy[VITA_GCI_PHASE_PROP],
                   (unsigned)q->busy[VITA_GCI_PHASE_PROP_BOUND],
                   (unsigned)q->busy[VITA_GCI_PHASE_ATOMIC],
                   (unsigned)q->busy[VITA_GCI_PHASE_SWEEP],
                   (unsigned)q->busy[VITA_GCI_PHASE_CALLFIN],
                   (unsigned)q->carry[VITA_GCI_PHASE_PROP_BOUND],
                   (unsigned)q->carry[VITA_GCI_PHASE_SWEEP],
                   (unsigned)q->carry[VITA_GCI_PHASE_CALLFIN],
                   (unsigned)q->walks, (unsigned)q->walk_objects,
                   (unsigned)q->walk_max,
                   (unsigned)q->settle_count, (unsigned)q->settle_step,
                   (unsigned)q->carried_atomic);
    q->calls = 0U;
    memset(q->kinds, 0, sizeof q->kinds);
    q->deferred = 0U;
    memset(q->ran, 0, sizeof q->ran);
    memset(q->atomic, 0, sizeof q->atomic);
    memset(q->place_us, 0, sizeof q->place_us);
    q->over_us = 0U;
    q->slack_us = 0U;
    q->owed_kb_max = 0U;
    q->requested_kb = 0U;
    q->ran_kb = 0U;
    q->owed_in_kb = owed_out_kb;
    q->dropped_kb = 0U;
    q->max_us = 0U;
    q->dropped = 0U;
    memset(q->busy, 0, sizeof q->busy);
    memset(q->carry, 0, sizeof q->carry);
    q->walks = 0U;
    q->walk_objects = 0U;
    q->walk_max = 0U;
    q->settle_count = 0U;
    q->settle_step = 0U;
    q->carried_atomic = 0U;
}

static int vita_lua_gc_idle_query(uint32_t *slack_us)
{
    vita_lua_gc_idle *q = &s_vita_lua_gc_idle;
    int kind = kage_vita_fullspeed_scheduler_gc_idle_query(slack_us);

    if (kind < KAGE_VITA_GC_IDLE_UNKNOWN || kind > KAGE_VITA_GC_IDLE_LATE)
        kind = KAGE_VITA_GC_IDLE_UNKNOWN;
    /* the slack a tail offers is counted once, at its first query */
    if (kind == KAGE_VITA_GC_IDLE_FREE && !q->tail_slack_counted) {
        q->slack_us = vita_lua_gc_idle_add(q->slack_us, *slack_us);
        q->tail_slack_counted = 1U;
    }
    return kind;
}

/* Run the carried step, if one is owed to this lua_State.  `at_count`: from
 * the LUA_GCCOUNT import at 0x4022ec (the normal place), else from the
 * GCSTEP import. */
static void vita_lua_gc_idle_settle(lua_State *state, int at_count)
{
    vita_lua_gc_idle *q = &s_vita_lua_gc_idle;
    uint32_t slack_us = 0U;
    uint64_t started;
    int kind;
#if defined(ISAAC_VITA_LUA_GC_STATE_PROFILE)
    uint8_t update_site = s_vita_lua_gc_state.update_site;
#endif

    if (!q->owed)
        return;
    if (q->state != state) {
        uint32_t i;

        for (i = 0U; i < q->owed; ++i)
            q->dropped_kb = vita_lua_gc_idle_add(
                q->dropped_kb, vita_lua_gc_idle_kb(q->owed_kb[i]));
        q->dropped = vita_lua_gc_idle_add(q->dropped, q->owed);
        q->owed = 0U;
        return;
    }
    kind = vita_lua_gc_idle_query(&slack_us);
    started = vita_lua_gc_now_us();
#if defined(ISAAC_VITA_LUA_GC_STATE_PROFILE)
    s_vita_lua_gc_state.update_site = 1U;    /* origin `upd` */
#endif
    /* Each carried step is taken off the ledger before it runs: a Lua
     * error thrown by a finalizer longjmps past this frame and must not
     * replay it. */
    while (q->owed) {
        int owed_kb = q->owed_kb[0];
        uint32_t i;

        for (i = 1U; i < q->owed; ++i)
            q->owed_kb[i - 1U] = q->owed_kb[i];
        --q->owed;
        if (at_count)
            q->settle_count = vita_lua_gc_idle_add(q->settle_count, 1U);
        else
            q->settle_step = vita_lua_gc_idle_add(q->settle_step, 1U);
        (void)vita_lua_gc_idle_run(state, owed_kb, kind, &slack_us, 1);
    }
#if defined(ISAAC_VITA_LUA_GC_STATE_PROFILE)
    s_vita_lua_gc_state.update_site = update_site;
#endif
#if defined(ISAAC_VITA_LUA_GC_PROFILE)
    /* the gc window charges it to the Update step that follows */
    s_vita_lua_gc_idle_settled_us = vita_lua_gc_add_saturating(
        s_vita_lua_gc_idle_settled_us, vita_lua_gc_elapsed_us(started));
#else
    (void)started;
#endif
}

/* LuaEngine::Update's paced step: run it, or carry it one tail. */
static int vita_lua_gc_idle_step(lua_State *state, int kb)
{
    vita_lua_gc_idle *q = &s_vita_lua_gc_idle;
    uint32_t slack_us = 0U;
    int result = 0;
    int phase = VITA_GCI_PHASE_NOOP;
    int busy;
    int kind;

    if (!q->banner) {
        q->banner = 1U;
        isaac_vita_log("[isaac-lua] gc idle step: on=1 owed_max=%u "
                       "window=%u site=%08x count_site=%08x walk_max=%u "
                       "carry=propb+sweep+callfin",
                       (unsigned)VITA_LUA_GC_IDLE_OWED_MAX,
                       (unsigned)VITA_LUA_GC_IDLE_WINDOW,
                       (unsigned)VITA_LUA_GC_UPDATE_STEP_RETURN_RVA,
                       (unsigned)VITA_LUA_GC_UPDATE_COUNT_RETURN_RVA,
                       (unsigned)VITA_LUA_GC_IDLE_WALK_MAX);
    }
    /* normally nothing is owed here: the GCCOUNT import ran it */
    vita_lua_gc_idle_settle(state, 0);
    q->state = state;
    kind = vita_lua_gc_idle_query(&slack_us);
    busy = kind == KAGE_VITA_GC_IDLE_BUSY;
    q->kinds[kind] = vita_lua_gc_idle_add(q->kinds[kind], 1U);
    q->requested_kb =
        vita_lua_gc_idle_add(q->requested_kb, vita_lua_gc_idle_kb(kb));
    if (busy) {
        phase = vita_lua_gc_idle_phase(state, kb);
        q->busy[phase] = vita_lua_gc_idle_add(q->busy[phase], 1U);
    }
    if (vita_lua_gc_idle_carry(busy, q->owed, VITA_LUA_GC_IDLE_OWED_MAX,
                               phase)) {
        q->owed_kb[q->owed++] = kb;
        q->deferred = vita_lua_gc_idle_add(q->deferred, 1U);
        q->carry[phase] = vita_lua_gc_idle_add(q->carry[phase], 1U);
        if (kb > 0 && (uint32_t)kb > q->owed_kb_max)
            q->owed_kb_max = (uint32_t)kb;
    } else {
        result = vita_lua_gc_idle_run(state, kb, kind, &slack_us, 0);
    }
    q->tail_slack_counted = 0U;
    if (++q->calls == VITA_LUA_GC_IDLE_WINDOW)
        vita_lua_gc_idle_emit();
    return result;
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
#if defined(ISAAC_VITA_LUA_GC_STATE_PROFILE)
        s_vita_lua_gc_state.update_site = what == LUA_GCSTEP &&
            site == VITA_LUA_GC_UPDATE_STEP_RETURN_RVA;
#endif
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
#if defined(ISAAC_VITA_LUA_GC_IDLE_STEP)
    if (what == LUA_GCSTEP && site == VITA_LUA_GC_UPDATE_STEP_RETURN_RVA) {
        result = vita_lua_gc_idle_step(state, data);
    } else
#endif
    {
#if defined(ISAAC_VITA_LUA_GC_IDLE_STEP)
        /* the carried step runs before LuaEngine::Update reads the heap */
        if (what == LUA_GCCOUNT &&
                site == VITA_LUA_GC_UPDATE_COUNT_RETURN_RVA)
            vita_lua_gc_idle_settle(state, 1);
#endif
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
