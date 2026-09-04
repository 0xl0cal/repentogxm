#ifndef KAGE_VITA_PHASE_PROFILE_H
#define KAGE_VITA_PHASE_PROFILE_H

#include <stdint.h>

#if defined(ISAAC_VITA_PILL_BLOOM_BYPASS) && \
    defined(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES)
#error "Bloom bypass and half-resolution modes are mutually exclusive"
#endif
#if defined(ISAAC_VITA_POOP_FX_SINGLE_CLOUD) && \
    !defined(ISAAC_VITA_POOP_FX_PROFILE)
#error "PoopFx single-cloud mode requires its proof profile"
#endif

/* The report interval is deliberately identical to the established physical
 * present heartbeat.  Two bounded records (timings and counters) describe the
 * 120 completed outer loops before the emitting loop head.  The opt-in guest
 * cache, GL redundancy cache, typed GL-state cache, vitaGL draw policy,
 * selective Bloom bypass, half-resolution Bloom mode, laser census and
 * fullspeed scheduler append bounded g, o, y, v/q/f, k, b, h, l and s/r records,
 * respectively.  The texture-churn diagnostic adds x after the base counters;
 * PoopFx attribution adds p after h and before the laser records.  The GL
 * time profile (ISAAC_VITA_GL_TIME_PROFILE) adds gt directly after c.  The
 * dispatch table (ISAAC_VITA_DISPATCH_TABLE) adds d and i directly after g;
 * the import-kinds census (ISAAC_VITA_PROFILE_IMPORT_KINDS) appends ik and ih
 * last.  The fully enabled order is
 * t,c,a,gt,x,g,d,i,o,y,v,q,f,k,b,h,p,l,m,n,s,r,ik,ih.  The s
 * record's
 * 16-hex-digit `at`
 * field is the exact
 * report-loop-head process time;
 * subtracting consecutive values measures the wall time of the later record's
 * 120 Updates.  ISAAC_VITA_PHASE_PROFILE_OTHER adds oth=p50/p95/max to t:
 * the loop time outside svc/upd/rnd/lim (render return -> limiter entry
 * plus limiter exit -> next service entry). */
#define KAGE_VITA_PHASE_PROFILE_WINDOW 120u

void kage_vita_phase_profile_reset(void);
void kage_vita_phase_profile_note_loop_head(uint32_t outer_loop_count);
void kage_vita_phase_profile_note_service_entry(void);
void kage_vita_phase_profile_note_update_entry(void);
void kage_vita_phase_profile_note_render_entry(void);
void kage_vita_phase_profile_note_render_return(void);
void kage_vita_phase_profile_note_render_skipped(void);
void kage_vita_phase_profile_note_present_enter(void);
void kage_vita_phase_profile_note_present_return(void);
void kage_vita_phase_profile_note_limiter_entry(void);
void kage_vita_phase_profile_note_limiter_exit(void);
#if defined(ISAAC_VITA_PILL_BLOOM_BYPASS)
void kage_vita_phase_profile_note_pill_bloom_capture_skip(void);
void kage_vita_phase_profile_note_pill_bloom_composite_skip(void);
#endif
#if defined(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES)
/* receipt: 1 = untouched initial 480x270, 2 = halved 960x540 recreation;
 * every other value records a rejected/unexpected creation. */
void kage_vita_phase_profile_note_bloom_half_create(uint32_t receipt);
void kage_vita_phase_profile_note_bloom_half_capture(void);
void kage_vita_phase_profile_note_bloom_half_composite(void);
#endif
#if defined(ISAAC_VITA_POOP_FX_PROFILE)
void kage_vita_phase_profile_note_poop_fx_add(uint32_t return_rva);
void kage_vita_phase_profile_note_poop_fx_render(
    uint32_t countdown, uint32_t cloud_count);
void kage_vita_phase_profile_note_poop_fx_cap(uint32_t skipped_clouds);
#endif
#if defined(ISAAC_VITA_LASER_PROFILE)
void kage_vita_phase_profile_laser_begin(
    uint32_t token, uint32_t source_type, uint32_t variant,
    uint32_t subtype, uint32_t sample, uint32_t shadow);
void kage_vita_phase_profile_laser_end(uint32_t token, uint32_t shadow);
#endif

#if defined(ISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE)
void kage_vita_phase_profile_oracle_emit_max_records(void);
#endif

#if defined(ISAAC_VITA_PHASE_PROFILE)
# define KAGE_VITA_PHASE_PROFILE_RESET() \
    kage_vita_phase_profile_reset()
# define KAGE_VITA_PHASE_PROFILE_LOOP_HEAD(count) \
    kage_vita_phase_profile_note_loop_head((count))
# define KAGE_VITA_PHASE_PROFILE_SERVICE_ENTRY() \
    kage_vita_phase_profile_note_service_entry()
# define KAGE_VITA_PHASE_PROFILE_UPDATE_ENTRY() \
    kage_vita_phase_profile_note_update_entry()
# define KAGE_VITA_PHASE_PROFILE_RENDER_ENTRY() \
    kage_vita_phase_profile_note_render_entry()
# define KAGE_VITA_PHASE_PROFILE_RENDER_RETURN() \
    kage_vita_phase_profile_note_render_return()
# define KAGE_VITA_PHASE_PROFILE_RENDER_SKIPPED() \
    kage_vita_phase_profile_note_render_skipped()
# define KAGE_VITA_PHASE_PROFILE_PRESENT_ENTER() \
    kage_vita_phase_profile_note_present_enter()
# define KAGE_VITA_PHASE_PROFILE_PRESENT_RETURN() \
    kage_vita_phase_profile_note_present_return()
# define KAGE_VITA_PHASE_PROFILE_LIMITER_ENTRY() \
    kage_vita_phase_profile_note_limiter_entry()
# define KAGE_VITA_PHASE_PROFILE_LIMITER_EXIT() \
    kage_vita_phase_profile_note_limiter_exit()
#else
# define KAGE_VITA_PHASE_PROFILE_RESET() ((void)0)
# define KAGE_VITA_PHASE_PROFILE_LOOP_HEAD(count) ((void)0)
# define KAGE_VITA_PHASE_PROFILE_SERVICE_ENTRY() ((void)0)
# define KAGE_VITA_PHASE_PROFILE_UPDATE_ENTRY() ((void)0)
# define KAGE_VITA_PHASE_PROFILE_RENDER_ENTRY() ((void)0)
# define KAGE_VITA_PHASE_PROFILE_RENDER_RETURN() ((void)0)
# define KAGE_VITA_PHASE_PROFILE_RENDER_SKIPPED() ((void)0)
# define KAGE_VITA_PHASE_PROFILE_PRESENT_ENTER() ((void)0)
# define KAGE_VITA_PHASE_PROFILE_PRESENT_RETURN() ((void)0)
# define KAGE_VITA_PHASE_PROFILE_LIMITER_ENTRY() ((void)0)
# define KAGE_VITA_PHASE_PROFILE_LIMITER_EXIT() ((void)0)
#endif

#endif
