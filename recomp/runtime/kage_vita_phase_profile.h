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
 * time profile (ISAAC_VITA_GL_TIME_PROFILE) adds gt and gx directly after c
 * (in GL_TIME_SDK_SPARSE mode gt is the four mode=sdk32 scope rows scene,
 * canonical, clear, queue, plus scope=attrib when the attrib fast path is ON),
 * the GL wrapper time (ISAAC_VITA_GL_WRAPPER_TIME) inserts gw between them,
 * and the FBO valid region (ISAAC_VITA_STOCK_FBO_VALID_REGION) appends
 * vr(s,p,u,a,e,v,c) to gx and adds its size census vz after it.  The
 * dispatch table (ISAAC_VITA_DISPATCH_TABLE) adds d and i directly after g;
 * the import-kinds census (ISAAC_VITA_PROFILE_IMPORT_KINDS) appends ik and
 * ih; the heap receipt (ISAAC_VITA_HEAP_CENSUS) appends mem. The optional
 * counter-only ISAAC_VITA_LASER_HALO_PROFILE appends ha, using a private
 * cumulative snapshot only at outer-loop boundaries, never laser seams. The
 * ABI2 ha extension observes ordinary all-white PLAIN requests only when
 * staging fusion is enabled; white_on=0/legacy records are not measured zeros.
 * The scheduler's w (previous window's emission cost) follows r. The diagnostic
 * ColorOffset fragment-shader probe (ISAAC_VITA_COLOROFFSET_FS_PROBE) adds kp
 * directly after k. The GL fill census ISAAC_VITA_GL_FILL_CENSUS adds fa and
 * fp directly after f, or in f's place when the vitaGL draw policy is off. The
 * fully enabled order is
 * t,c,a,gt,gw,gd,gu,gx,vz,x,xd,g,d,i,o,y,v,q,f,fa,fp,e,k,kp,b,h,p,l,m,n,s,r,w,ik,ih,mem,ha,rl.
 * ROOM_LOG_MARKERS adds rl last, followed by at most eight separate roomlog
 * events. Those timestamps mark successful logger formatting, not room arrival
 * or durable fwrite. Queue/reset loss and saturation are explicit; no forced
 * tail flush is performed.
 * The s
 * record's 16-hex-digit `at` field is the exact report-loop-head process
 * time; subtracting consecutive values measures the wall time of the later
 * record's 120 Updates.  ISAAC_VITA_PHASE_PROFILE_OTHER adds oth=p50/p95/max
 * to t: the loop time outside svc/upd/rnd/lim (render return -> limiter
 * entry plus limiter exit -> next service entry). The t record also includes
 * gap(n,min,50,95,max), in microseconds between consecutive valid CPU Present
 * returns. It includes skipped-render loops and synchronous report overhead,
 * carries the previous endpoint across window boundaries, and resets on a
 * backend reset or invalid Present pair. n is normally 119 in the first full
 * window and 120 thereafter. This is NOT physical display/vblank timing.
 * FRAME_SPIKE_PROFILE inserts three sp records after t (whole/update/render
 * winners with correlated same-loop phases; swap remains nested in render).
 * Optional room diagnostics follow x/xd: fh, png/pngf, io/ioo/ioc, sfx/sfxn and
 * nr/free/nrm/nrs. Their take validity/baseline and inclusive-scope contracts
 * are documented in kage_vita_room_profile.h and the producer headers. */
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
#if defined(ISAAC_VITA_ROOM_LOG_MARKERS) && ISAAC_VITA_ROOM_LOG_MARKERS
/* Authenticated, completed logger formatting only. Raw signed-%d words, not
 * unique room IDs. One clock on the owner; bounded deferred output at report. */
void kage_vita_phase_profile_room_log(uint32_t first, uint32_t second);
#endif
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
#if defined(ISAAC_VITA_ROOM_LOG_MARKERS) && ISAAC_VITA_ROOM_LOG_MARKERS
void kage_vita_phase_profile_oracle_room_seed(uint32_t value);
#endif
#endif

#if defined(ISAAC_VITA_HEAP_CENSUS)
/* ISAAC_VITA_HEAP_CENSUS: one extra ph120.mem record outside the window
 * cadence, tagged why= ("badalloc" from the entry owner's bad_alloc
 * diagnostic, "exit" from its terminal tail).  Declared under the census
 * define, not ISAAC_VITA_PHASE_PROFILE, because entry_vita.c is not in the
 * phase-profile define scope.  No-op before the first profiled loop head
 * (boot failures reach the terminal tail before KAGE init); each why= is
 * claimed once per process; mallinfo is skipped when the guest heap is
 * terminal. */
void kage_vita_phase_profile_heap_census_final(const char *why);

/* newlib mallinfo() reduced to the five ph120.mem fields.  A private struct
 * so the host oracle compiles without <malloc.h> (Windows clang has no
 * struct mallinfo; glibc's lacks Vita's field widths). */
typedef struct kage_vita_heap_mallinfo {
    uint32_t arena;
    uint32_t ordblks;
    uint32_t uordblks;
    uint32_t fordblks;
    uint32_t keepcost;
} kage_vita_heap_mallinfo;

# if defined(ISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE)
/* Oracle stand-ins for the two reads the reporter makes outside every lock:
 * newlib mallinfo() and vitaGL's RAM pool query (vglMemFree/vglMemTotal). */
void kage_vita_phase_profile_oracle_mallinfo(kage_vita_heap_mallinfo *out);
void kage_vita_phase_profile_oracle_vgl_ram(
    uint32_t *free_bytes, uint32_t *total_bytes);
# endif
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
