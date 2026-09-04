#ifndef KAGE_VITA_STALL_PROBE_H
#define KAGE_VITA_STALL_PROBE_H

#include <stdint.h>

/* Native breadcrumbs for a guest main-thread stall.  These are deliberately
 * outside generated translation units: enabling the diagnostic must not
 * invalidate or slow the 169-file translated corpus at build time. */
enum kage_vita_stall_phase {
    KAGE_VITA_STALL_INACTIVE = 0,
    KAGE_VITA_STALL_LOOP,
    KAGE_VITA_STALL_UPDATE,
    KAGE_VITA_STALL_RENDER,
    KAGE_VITA_STALL_POST_RENDER,
    KAGE_VITA_STALL_PRESENT,
    KAGE_VITA_STALL_POST_PRESENT
};

enum kage_vita_stall_dispatch_kind {
    KAGE_VITA_STALL_DISPATCH_INDIRECT = 1,
    KAGE_VITA_STALL_DISPATCH_IMPORT = 2
};

enum kage_vita_stall_sync_edge {
    KAGE_VITA_STALL_SYNC_FREAD_POWER = 1,
    KAGE_VITA_STALL_SYNC_LOADING_SWAP_ENTER,
    KAGE_VITA_STALL_SYNC_LOADING_SWAP_RETURN,
    KAGE_VITA_STALL_SYNC_LOADING_NOTE_RETURN
};

typedef struct kage_vita_stall_snapshot {
    uint32_t active;
    uint32_t phase;
    uint32_t loop_count;
    uint32_t update_count;
    uint32_t render_count;
    uint32_t render_return_count;
    uint32_t present_count;
    uint32_t guest_site;
    uint32_t dispatch_calls;
    uint32_t dispatch_events;
    uint32_t logical_fread_calls;
    uint32_t loading_swap_count;
    uint32_t report_count;
    uint32_t power_tick_calls;
    uint32_t power_tick_result;
} kage_vita_stall_snapshot;

typedef struct kage_vita_stall_dispatch_event {
    uint32_t sequence;
    uint32_t calls;
    uint32_t kind;
    uint32_t target_rva;
    uint32_t return_rva;
} kage_vita_stall_dispatch_event;

/* Start/stop own a joinable Vita thread.  Failure is diagnostic-only: start
 * returns zero and logs once, but must never make the game fail to initialize. */
int  kage_vita_stall_probe_start(void);
void kage_vita_stall_probe_stop(void);

void kage_vita_stall_note_loop(void);
void kage_vita_stall_note_update(void);
void kage_vita_stall_note_render(void);
void kage_vita_stall_note_render_return(void);
void kage_vita_stall_note_present_enter(uint32_t completed_count);
void kage_vita_stall_note_present_return(uint32_t completed_count);
void kage_vita_stall_note_guest_site(uint32_t site_rva);
void kage_vita_stall_note_dispatch(uint32_t kind, uint32_t target_rva,
                                   uint32_t return_rva);
void kage_vita_stall_note_logical_fread(uint32_t completed_calls);
void kage_vita_stall_note_loading_swap(uint32_t completed_swaps);
void kage_vita_stall_trace_sync(uint32_t edge, uint32_t completed_calls);

void kage_vita_stall_get_snapshot(kage_vita_stall_snapshot *out);
unsigned kage_vita_stall_get_dispatch_events(
    kage_vita_stall_dispatch_event *out, unsigned capacity);

/* Owners use these wrappers rather than the functions directly.  The release
 * default leaves ISAAC_VITA_STALL_PROBE undefined, so even argument evaluation
 * (including the guest return-address load) disappears at preprocessing time. */
#ifdef ISAAC_VITA_STALL_PROBE
# define KAGE_VITA_STALL_START() \
    ((void)kage_vita_stall_probe_start())
# define KAGE_VITA_STALL_STOP() \
    kage_vita_stall_probe_stop()
# define KAGE_VITA_STALL_NOTE_LOOP() \
    kage_vita_stall_note_loop()
# define KAGE_VITA_STALL_NOTE_UPDATE() \
    kage_vita_stall_note_update()
# define KAGE_VITA_STALL_NOTE_RENDER() \
    kage_vita_stall_note_render()
# define KAGE_VITA_STALL_NOTE_RENDER_RETURN() \
    kage_vita_stall_note_render_return()
# define KAGE_VITA_STALL_NOTE_PRESENT_ENTER(count) \
    kage_vita_stall_note_present_enter(count)
# define KAGE_VITA_STALL_NOTE_PRESENT_RETURN(count) \
    kage_vita_stall_note_present_return(count)
# define KAGE_VITA_STALL_NOTE_GUEST_SITE(site) \
    kage_vita_stall_note_guest_site(site)
# define KAGE_VITA_STALL_NOTE_DISPATCH(kind, target, return_rva) \
    kage_vita_stall_note_dispatch(kind, target, return_rva)
# define KAGE_VITA_STALL_NOTE_LOGICAL_FREAD(count) \
    kage_vita_stall_note_logical_fread(count)
# define KAGE_VITA_STALL_NOTE_LOADING_SWAP(count) \
    kage_vita_stall_note_loading_swap(count)
# define KAGE_VITA_STALL_TRACE_SYNC(edge, count) \
    kage_vita_stall_trace_sync((edge), (count))
#else
# define KAGE_VITA_STALL_START()                       ((void)0)
# define KAGE_VITA_STALL_STOP()                        ((void)0)
# define KAGE_VITA_STALL_NOTE_LOOP()                   ((void)0)
# define KAGE_VITA_STALL_NOTE_UPDATE()                 ((void)0)
# define KAGE_VITA_STALL_NOTE_RENDER()                 ((void)0)
# define KAGE_VITA_STALL_NOTE_RENDER_RETURN()          ((void)0)
# define KAGE_VITA_STALL_NOTE_PRESENT_ENTER(count)     ((void)0)
# define KAGE_VITA_STALL_NOTE_PRESENT_RETURN(count)    ((void)0)
# define KAGE_VITA_STALL_NOTE_GUEST_SITE(site)         ((void)0)
# define KAGE_VITA_STALL_NOTE_DISPATCH(kind, target, return_rva) ((void)0)
# define KAGE_VITA_STALL_NOTE_LOGICAL_FREAD(count)      ((void)0)
# define KAGE_VITA_STALL_NOTE_LOADING_SWAP(count)       ((void)0)
# define KAGE_VITA_STALL_TRACE_SYNC(edge, count)         ((void)0)
#endif

#ifdef ISAAC_KAGE_VITA_STALL_ORACLE
/* Deterministic seam used by the host/softfp oracle; production calls the
 * same poller from its native watchdog thread. */
void kage_vita_stall_oracle_poll(uint64_t now_us);
#endif

#endif
