#ifndef KAGE_VITA_FULLSPEED_SCHEDULER_H
#define KAGE_VITA_FULLSPEED_SCHEDULER_H

#include <stdint.h>

/* The clock is integer microseconds.  Keeping deadlines in thirds of a
 * microsecond represents 1/60 second exactly:
 * 16,666 2/3 us == 50,000 units. */
#define KAGE_VITA_FULLSPEED_TICK_UNITS 50000u
/* 33,333 1/3 us: hard minimum spacing between Game::Update calls. */
#define KAGE_VITA_FULLSPEED_GAME_TICK_UNITS 100000u
#define KAGE_VITA_FULLSPEED_MAX_CATCHUP_TICKS 4u
#define KAGE_VITA_FULLSPEED_MAX_DUE_TICKS \
    (KAGE_VITA_FULLSPEED_MAX_CATCHUP_TICKS + 1u)
/* Once a backlog is already large enough that time must be discarded, retain
 * only one catch-up wrapper after the current one.  Ordinary 27--55 ms render
 * recovery remains governed by MAX_CATCHUP_TICKS above. */
#define KAGE_VITA_FULLSPEED_STALE_CATCHUP_TICKS 1u
#define KAGE_VITA_FULLSPEED_STALE_DUE_TICKS \
    (KAGE_VITA_FULLSPEED_STALE_CATCHUP_TICKS + 1u)
#define KAGE_VITA_FULLSPEED_MAX_FULL_RENDER_SKIPS 4u
#define KAGE_VITA_FULLSPEED_GAME_UPDATE_NORMAL_RETURN_RVA 0x002CE74Fu
#define KAGE_VITA_FULLSPEED_GAME_UPDATE_ZERO_FRAME_RETURN_RVA 0x002CE167u
#define KAGE_VITA_FULLSPEED_GAME_UPDATE_ZERO_FRAME_2CE121_RVA 0x002CE121u
#define KAGE_VITA_FULLSPEED_GAME_UPDATE_ZERO_FRAME_2CE362_RVA 0x002CE362u
/* Two more of the eight machine-pinned early RETs.  Device evidence (bid
 * perf:bundle35-raster720-v1 at 157.659 s, perf:wf-int720-v2 at 394.796 s):
 * both returned with Game+0x1a30dc unchanged (21346>21346, 5042>5042) while
 * the player left a run for the menu, and the fail-open then disabled the
 * 60-Hz cadence for the rest of the session.  Accepted only when the build
 * defines ISAAC_VITA_FULLSPEED_EXIT_ZERO_FRAME_SITES. */
#define KAGE_VITA_FULLSPEED_GAME_UPDATE_ZERO_FRAME_2CE099_RVA 0x002CE099u
#define KAGE_VITA_FULLSPEED_GAME_UPDATE_ZERO_FRAME_2CE0EA_RVA 0x002CE0EAu

typedef struct KageVitaFullspeedSnapshot {
    uint32_t wrapper_ticks;
    uint32_t service_calls;
    uint32_t manager_dispatch_calls;
    uint32_t manager_entries;
    uint32_t game_updates;
    uint32_t render_calls;
    uint32_t render_bodies;
    uint32_t presents;
    uint32_t full_phases;
    uint32_t nonfull_phases;
    uint32_t interpolation_phases;
    uint32_t render_skips;
    uint32_t late_full_skips;
    uint32_t nonfull_skips;
    uint32_t forced_renders;
    uint32_t duplicate_tick_violations;
    uint32_t parity_violations;
    uint32_t game_frame_violations;
    uint32_t game_update_last_return_site;
    uint32_t game_update_last_frame_before;
    uint32_t game_update_last_frame_after;
    uint32_t game_frame_violation_return_site;
    uint32_t game_frame_violation_before;
    uint32_t game_frame_violation_after;
    uint32_t manager_counter_rebase_events;
    uint32_t manager_counter_rebases;
    uint32_t manager_counter_rebase_violations;
    uint32_t game_pointer_publish_events;
    uint32_t game_pointer_rebinds;
    uint32_t game_pointer_violations;
    uint32_t sequence_violations;
    uint32_t runtime_disables;
    uint32_t clock_resets;
    uint32_t dropped_ticks;
    uint32_t dropped_us;
    uint32_t wait_calls;
    uint32_t waited_us;
    uint32_t debt_us;
    uint32_t maximum_debt_us;
    /* Subset of wait_calls/waited_us spent in the Game::Update pacing sleep
     * (kage_vita_fullspeed_pace_game_update), which executes inside the
     * profiler's upd phase.  ph120.ik pace(c,us) prints the window delta so
     * upd wall time can be split into CPU and sleep.  Accumulated only when
     * the scheduler is compiled with ISAAC_VITA_PROFILE_IMPORT_KINDS (the
     * census build that prints ph120.ik); zero in every other build.  The
     * fields stay unconditional so every includer shares one layout. */
    uint32_t pace_wait_calls;
    uint32_t pace_waited_us;
} KageVitaFullspeedSnapshot;

void kage_vita_fullspeed_scheduler_reset(void);
void kage_vita_fullspeed_scheduler_deactivate(void);
void kage_vita_fullspeed_scheduler_note_loop_head(void);
void kage_vita_fullspeed_scheduler_note_service(void);
void kage_vita_fullspeed_scheduler_note_manager_dispatch(void);
void kage_vita_fullspeed_scheduler_note_manager_entry(
    uint32_t manager_pointer, uint32_t manager_counter,
    uint32_t interpolation_enabled, uint32_t game_pointer,
    uint32_t game_frame);
void kage_vita_fullspeed_scheduler_note_manager_counter_rebase(
    uint32_t manager_pointer, uint32_t old_manager_counter,
    uint32_t new_manager_counter);
void kage_vita_fullspeed_scheduler_note_game_pointer_publish(
    uint32_t manager_pointer, uint32_t manager_counter,
    uint32_t old_game_pointer, uint32_t new_game_pointer);
void kage_vita_fullspeed_scheduler_note_game_update_begin(
    uint32_t manager_counter, uint32_t game_pointer, uint32_t game_frame);
void kage_vita_fullspeed_scheduler_note_game_update_early_return(
    uint32_t return_site);
void kage_vita_fullspeed_scheduler_note_game_update_end(
    uint32_t game_pointer, uint32_t game_frame);
int kage_vita_fullspeed_scheduler_plan_render(
    uint32_t manager_pointer, uint32_t manager_counter,
    uint32_t interpolation_enabled, uint32_t game_pointer,
    uint32_t game_frame);
void kage_vita_fullspeed_scheduler_note_render_entry(void);
void kage_vita_fullspeed_scheduler_note_render_body(
    uint32_t manager_pointer, uint32_t manager_counter,
    uint32_t interpolation_enabled);
void kage_vita_fullspeed_scheduler_note_render_return(void);
void kage_vita_fullspeed_scheduler_note_present(void);
int kage_vita_fullspeed_scheduler_bypass_limiter(void);
void kage_vita_fullspeed_scheduler_snapshot(
    KageVitaFullspeedSnapshot *snapshot);

#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
# define KAGE_VITA_FULLSPEED_RESET() \
    kage_vita_fullspeed_scheduler_reset()
# define KAGE_VITA_FULLSPEED_DEACTIVATE() \
    kage_vita_fullspeed_scheduler_deactivate()
# define KAGE_VITA_FULLSPEED_LOOP_HEAD() \
    kage_vita_fullspeed_scheduler_note_loop_head()
# define KAGE_VITA_FULLSPEED_NOTE_SERVICE() \
    kage_vita_fullspeed_scheduler_note_service()
# define KAGE_VITA_FULLSPEED_NOTE_MANAGER_DISPATCH() \
    kage_vita_fullspeed_scheduler_note_manager_dispatch()
# define KAGE_VITA_FULLSPEED_NOTE_RENDER_ENTRY() \
    kage_vita_fullspeed_scheduler_note_render_entry()
# define KAGE_VITA_FULLSPEED_NOTE_RENDER_RETURN() \
    kage_vita_fullspeed_scheduler_note_render_return()
# define KAGE_VITA_FULLSPEED_NOTE_PRESENT() \
    kage_vita_fullspeed_scheduler_note_present()
#else
# define KAGE_VITA_FULLSPEED_RESET() ((void)0)
# define KAGE_VITA_FULLSPEED_DEACTIVATE() ((void)0)
# define KAGE_VITA_FULLSPEED_LOOP_HEAD() ((void)0)
# define KAGE_VITA_FULLSPEED_NOTE_SERVICE() ((void)0)
# define KAGE_VITA_FULLSPEED_NOTE_MANAGER_DISPATCH() ((void)0)
# define KAGE_VITA_FULLSPEED_NOTE_RENDER_ENTRY() ((void)0)
# define KAGE_VITA_FULLSPEED_NOTE_RENDER_RETURN() ((void)0)
# define KAGE_VITA_FULLSPEED_NOTE_PRESENT() ((void)0)
#endif

#endif
