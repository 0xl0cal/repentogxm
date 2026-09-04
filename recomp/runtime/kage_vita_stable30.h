#ifndef KAGE_VITA_STABLE30_H
#define KAGE_VITA_STABLE30_H

#include <stdint.h>

/* Deadlines use thirds of a microsecond so 1/30 second is exact. */
#define KAGE_VITA_STABLE30_PERIOD_UNITS 100000u

typedef struct KageVitaStable30Snapshot {
    uint32_t loop_heads;
    uint32_t full_phases;
    uint32_t nonfull_phases;
    uint32_t interpolation_renders_skipped;
    uint32_t limiter_bypasses;
    uint32_t limiter_fallbacks;
    uint32_t wait_calls;
    uint32_t waited_us;
    uint32_t delay_failures;
    uint32_t clock_resets;
} KageVitaStable30Snapshot;

void kage_vita_stable30_reset(void);
void kage_vita_stable30_deactivate(void);
void kage_vita_stable30_note_loop_head(void);

/* These bracket the one existing Vita platform Present.  Only a matching
 * full phase is paced; the deadline is rebased after a successful Present. */
void kage_vita_stable30_before_present(void);
void kage_vita_stable30_after_present(int succeeded);

/* Called after Manager::Update has loaded Manager+0x4a264.  A true result
 * bypasses only the interpolation flag and enters the native 4a26c/4a26d
 * guard chain at 0x004b0043. */
int kage_vita_stable30_manager_entry(uint32_t manager_counter);

/* Called once at Manager::Render's exact parity test.  A true result takes
 * the existing no-op epilogue at 0x004b1089 for a matching non-full phase.
 * A matching full/odd phase instead arms exactly one platform Present. */
int kage_vita_stable30_plan_render(uint32_t manager_counter);

/* Called before the original per-wrapper 60-Hz limiter.  Only the cheap
 * non-full phase is paced to the preceding full phase's 1/30 deadline and
 * then bypasses that limiter.  Full phases retain the original limiter. */
int kage_vita_stable30_finish_tick(void);

void kage_vita_stable30_snapshot(KageVitaStable30Snapshot *snapshot);

#if defined(ISAAC_VITA_STABLE_30_PRESENTATION)
# define KAGE_VITA_STABLE30_RESET() kage_vita_stable30_reset()
# define KAGE_VITA_STABLE30_DEACTIVATE() kage_vita_stable30_deactivate()
# define KAGE_VITA_STABLE30_LOOP_HEAD() kage_vita_stable30_note_loop_head()
# define KAGE_VITA_STABLE30_BEFORE_PRESENT() \
    kage_vita_stable30_before_present()
# define KAGE_VITA_STABLE30_AFTER_PRESENT(succeeded) \
    kage_vita_stable30_after_present((succeeded))
#else
# define KAGE_VITA_STABLE30_RESET() ((void)0)
# define KAGE_VITA_STABLE30_DEACTIVATE() ((void)0)
# define KAGE_VITA_STABLE30_LOOP_HEAD() ((void)0)
# define KAGE_VITA_STABLE30_BEFORE_PRESENT() ((void)0)
# define KAGE_VITA_STABLE30_AFTER_PRESENT(succeeded) ((void)(succeeded))
#endif

#endif
