#ifndef KAGE_VITA_GUEST_SAMPLER_H
#define KAGE_VITA_GUEST_SAMPLER_H

#include <stdint.h>

/* Guest-code sampling profiler (ISAAC_VITA_GUEST_SAMPLER, device only).
 *
 * A second-core SceKernel thread reads the game thread's guest ESP every
 * millisecond, walks the guest stack upward for the innermost validated x86
 * return marker (the translated code pushes bare return RVAs at every call
 * site) and attributes the sample to translated functions, bucketed by the
 * phase-profile phase the game thread is in.  Every 120-loop window the
 * profile reporter prints the per-phase top functions right after ph120.t.
 * Phase indices are also the bucket order in every printed record. */
enum kage_vita_guest_sampler_phase {
    KAGE_VITA_GUEST_SAMPLER_SVC = 0,
    KAGE_VITA_GUEST_SAMPLER_UPD,
    KAGE_VITA_GUEST_SAMPLER_RND,
    KAGE_VITA_GUEST_SAMPLER_SWP,
    KAGE_VITA_GUEST_SAMPLER_LIM,
    KAGE_VITA_GUEST_SAMPLER_OTH,
    /* The fullspeed scheduler's Game::Update pacing sleep
     * (kage_vita_fullspeed_pace_game_update -> sceKernelDelayThread).  It runs
     * inside the upd phase before the Game::Update marker exists, so without
     * its own bucket it lands on Manager::Update as 14-21 % of "upd self"
     * (research-sampler-hotlists finding 5).  Three key bits hold 8 phases. */
    KAGE_VITA_GUEST_SAMPLER_PACE,
    KAGE_VITA_GUEST_SAMPLER_PHASE_COUNT
};

#if defined(ISAAC_VITA_GUEST_SAMPLER)
struct CPU;

/* Written by the game thread at every phase seam, read by the sampler. */
extern volatile uint32_t g_kage_vita_guest_sampler_phase;
/* guest.c (compiled with ISAAC_VITA_GUEST_SAMPLER): callee of the innermost
 * *live* indirect dispatch on the game thread, or 0 when none is live.
 * Contract (see kage_vita_guest_sampler.c "ext"): every route that executes an
 * indirect `call r/m32` publishes its target on entry and restores the
 * enclosing value when the callee returns -- guest_call (wrapper in guest.c),
 * guest_try_direct_sync_import_call (the Enter/LeaveCriticalSection route
 * that bypasses guest_call) and guest_import_call (ISAAC_VITA_IMPORT_DIRECT,
 * the generated call/jmp [IAT] route, around its family endpoint).  Generated
 * direct edges
 * (guest_direct_translated_target) do not publish; their callee is named by
 * the enclosing indirect target, one level up.
 *
 * Non-local exits.  The restore is a plain store after the callee returns, so
 * a longjmp over the wrapper skips it:
 *  - Lua errors.  host_vita_lua.c (also compiled with the macro) saves the
 *    word in each translated-callback frame and restores it on both unwind
 *    routes: the frame's own setjmp (vita_lua_call_guest) and the recovery
 *    of frames abandoned by a raise that a native lua_pcall caught
 *    (vita_lua_recover_callbacks).  Until that restore runs the word names
 *    the escaped callee; the window is the Lua error path itself.
 *  - guest_fault / guest_exit never return to translated code (longjmp to
 *    the run scope or abort), so a fault inside a published callee leaves
 *    the word naming that callee while the run is being torn down.  No
 *    sample taken after that point belongs to a live frame; nothing to
 *    restore.
 * Sampler builds only; production objects contain no reference to the word. */
extern volatile uint32_t g_kage_guest_last_indirect_target;

/* Bind the production runner's stack-owned CPU and start the sampler thread.
 * Logs one startup line (thread id, affinity, period, table sizes). */
void kage_vita_guest_sampler_start(struct CPU *c);

/* Called by the phase-profile reporter once per 120-loop window, directly
 * after the ph120.t record.  Swaps the aggregation buffer, prints the bounded
 * ph120.hot/ph120.hotc/ph120.hs records and clears the retired buffer. */
void kage_vita_guest_sampler_report(
    const char *build_id, uint32_t window, uint32_t last_outer_loop);

# define KAGE_VITA_GUEST_SAMPLER_PHASE(phase) \
    (g_kage_vita_guest_sampler_phase = (uint32_t)(phase))
# define KAGE_VITA_GUEST_SAMPLER_REPORT(build_id, window, loops) \
    kage_vita_guest_sampler_report((build_id), (window), (loops))
#else
# define KAGE_VITA_GUEST_SAMPLER_PHASE(phase) ((void)0)
# define KAGE_VITA_GUEST_SAMPLER_REPORT(build_id, window, loops) ((void)0)
#endif

#endif
