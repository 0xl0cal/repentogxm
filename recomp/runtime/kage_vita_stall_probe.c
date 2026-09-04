/* Low-overhead, lifecycle-owned watchdog for a live Isaac guest stall.
 *
 * The current runtime has one x86 dispatcher.  guest_call therefore keeps a
 * writer-local call counter and publishes it with a cheap relaxed 32-bit
 * store.  The more expensive ring commit happens only when the target/return
 * pair changes.  The watchdog reads all shared words atomically and never
 * dereferences guest memory, so it remains safe while the main guest thread
 * is spinning or stopped inside a native seam. */
#include "kage_vita_stall_probe.h"
#include "kage_vita_preloop_phase.h"

#include <stdint.h>
#include <string.h>

#ifndef ISAAC_VITA_STALL_BUILD_ID
# define ISAAC_VITA_STALL_BUILD_ID "stall:unstamped"
#endif

#ifdef ISAAC_KAGE_VITA_STALL_ORACLE
typedef int SceUID;
typedef uint32_t SceSize;
typedef uint32_t SceUInt;
typedef int (*SceKernelThreadEntry)(SceSize args, void *argp);
typedef struct SceKernelThreadInfo {
    SceSize size;
    int currentPriority;
} SceKernelThreadInfo;
extern SceUID sceKernelCreateThread(
    const char *, SceKernelThreadEntry, int, SceSize, SceUInt, int,
    const void *);
extern int sceKernelDeleteThread(SceUID);
extern int sceKernelStartThread(SceUID, SceSize, void *);
extern int sceKernelWaitThreadEnd(SceUID, int *, SceUInt *);
extern int sceKernelDelayThread(SceUInt);
extern int sceKernelGetThreadId(void);
extern int sceKernelGetThreadInfo(SceUID, SceKernelThreadInfo *);
extern uint64_t sceKernelGetProcessTimeWide(void);
extern int sceKernelPowerTick(int);
# define SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND 1
#else
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#endif

void isaac_vita_log(const char *format, ...);

#define KAGE_VITA_STALL_RING_COUNT       32u
#define KAGE_VITA_STALL_LOG_EVENT_COUNT  16u
#define KAGE_VITA_STALL_POLL_US          250000u
#define KAGE_VITA_STALL_FIRST_REPORT_US  5000000u
#define KAGE_VITA_STALL_SECOND_REPORT_US 10000000u
#define KAGE_VITA_STALL_REPEAT_REPORT_US 10000000u
#define KAGE_VITA_PRELOOP_FIRST_REPORT_US   UINT64_C(30000000)
#define KAGE_VITA_PRELOOP_SECOND_REPORT_US  UINT64_C(60000000)
#define KAGE_VITA_PRELOOP_REPEAT_REPORT_US UINT64_C(120000000)
#define KAGE_VITA_PRELOOP_LOG_EVENT_COUNT  4u
#define KAGE_VITA_STALL_STACK_SIZE       0x4000u
#define KAGE_VITA_STALL_DEFAULT_PRIORITY 0x10000100

#define KAGE_VITA_STALL_MARKER_LOOP           (1u << 0)
#define KAGE_VITA_STALL_MARKER_UPDATE         (1u << 1)
#define KAGE_VITA_STALL_MARKER_RENDER         (1u << 2)
#define KAGE_VITA_STALL_MARKER_RENDER_RETURN  (1u << 3)
#define KAGE_VITA_STALL_MARKER_PRESENT_ENTER  (1u << 4)
#define KAGE_VITA_STALL_MARKER_PRESENT_RETURN (1u << 5)

#define LOAD32(pointer) \
    __atomic_load_n((pointer), __ATOMIC_ACQUIRE)
#define LOAD32_RELAXED(pointer) \
    __atomic_load_n((pointer), __ATOMIC_RELAXED)
#define STORE32(pointer, value) \
    __atomic_store_n((pointer), (value), __ATOMIC_RELEASE)
#define STORE32_RELAXED(pointer, value) \
    __atomic_store_n((pointer), (value), __ATOMIC_RELAXED)

typedef struct kage_vita_stall_ring_slot {
    uint32_t sequence;
    uint32_t calls;
    uint32_t kind;
    uint32_t target_rva;
    uint32_t return_rva;
} kage_vita_stall_ring_slot;

typedef struct kage_vita_stall_state {
    uint32_t active;
    uint32_t stop;
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
    kage_vita_stall_ring_slot ring[KAGE_VITA_STALL_RING_COUNT];
} kage_vita_stall_state;

typedef struct kage_vita_stall_dispatch_writer {
    uint32_t calls;
    uint32_t events;
    uint32_t last_kind;
    uint32_t last_target;
    uint32_t last_return;
} kage_vita_stall_dispatch_writer;

/* Kept global and named so a debugger/minidump can find the same state that
 * the watchdog prints.  Readers must still use the atomic accessors below. */
kage_vita_stall_state g_kage_vita_stall_state;

static SceUID s_watchdog_thread = -1;
/* One object keeps the hot single-writer fields in one data section.  With
 * -fdata-sections this avoids four extra address materializations in every
 * guest_call breadcrumb. */
static kage_vita_stall_dispatch_writer s_dispatch_writer;
static uint32_t s_tracked_present;
static uint64_t s_next_report_us;
static uint32_t s_pre_present_armed;
static uint32_t s_first_marker_mask;
static uint32_t s_preloop_last_dispatch_calls;
static uint32_t s_preloop_last_fread_calls;
static uint32_t s_preloop_last_loading_swaps;
static kage_vita_preloop_phase_snapshot s_preloop_last_phase;
static uint64_t s_last_progress_us;
static uint32_t s_sync_sequence;
static uint32_t s_sync_dispatch_remaining;

static uint32_t kage_vita_stall_increment(uint32_t *value)
{
    return __atomic_add_fetch(value, 1u, __ATOMIC_RELAXED);
}

static int kage_vita_stall_power_tick(void)
{
    int result = sceKernelPowerTick(
        SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);

    STORE32_RELAXED(&g_kage_vita_stall_state.power_tick_result,
                    (uint32_t)result);
    (void)kage_vita_stall_increment(
        &g_kage_vita_stall_state.power_tick_calls);
    return result;
}

static const char *kage_vita_stall_phase_name(uint32_t phase)
{
    switch (phase) {
    case KAGE_VITA_STALL_LOOP:         return "loop";
    case KAGE_VITA_STALL_UPDATE:       return "update";
    case KAGE_VITA_STALL_RENDER:       return "render";
    case KAGE_VITA_STALL_POST_RENDER:  return "post-render";
    case KAGE_VITA_STALL_PRESENT:      return "present";
    case KAGE_VITA_STALL_POST_PRESENT: return "post-present";
    default:                           return "inactive";
    }
}

static const char *kage_vita_stall_kind_name(uint32_t kind)
{
    return kind == KAGE_VITA_STALL_DISPATCH_IMPORT ? "import" : "indirect";
}

static char kage_vita_stall_kind_code(uint32_t kind)
{
    if (kind == KAGE_VITA_STALL_DISPATCH_IMPORT)
        return 'I';
    if (kind == KAGE_VITA_STALL_DISPATCH_INDIRECT)
        return 'D';
    return '-';
}

static void kage_vita_stall_reset_state(void)
{
    kage_vita_preloop_phase_reset();
    memset(&g_kage_vita_stall_state, 0, sizeof g_kage_vita_stall_state);
    memset(&s_dispatch_writer, 0, sizeof s_dispatch_writer);
    s_tracked_present = 0u;
    s_next_report_us = KAGE_VITA_PRELOOP_FIRST_REPORT_US;
    s_pre_present_armed = 0u;
    s_first_marker_mask = 0u;
    s_preloop_last_dispatch_calls = 0u;
    s_preloop_last_fread_calls = 0u;
    s_preloop_last_loading_swaps = 0u;
    memset(&s_preloop_last_phase, 0, sizeof s_preloop_last_phase);
    s_last_progress_us = sceKernelGetProcessTimeWide();
    s_sync_sequence = 0u;
    s_sync_dispatch_remaining = 0u;
}

void kage_vita_stall_get_snapshot(kage_vita_stall_snapshot *out)
{
    if (!out)
        return;
    out->active = LOAD32(&g_kage_vita_stall_state.active);
    out->phase = LOAD32(&g_kage_vita_stall_state.phase);
    out->loop_count = LOAD32(&g_kage_vita_stall_state.loop_count);
    out->update_count = LOAD32(&g_kage_vita_stall_state.update_count);
    out->render_count = LOAD32(&g_kage_vita_stall_state.render_count);
    out->render_return_count =
        LOAD32(&g_kage_vita_stall_state.render_return_count);
    out->present_count = LOAD32(&g_kage_vita_stall_state.present_count);
    out->guest_site = LOAD32(&g_kage_vita_stall_state.guest_site);
    out->dispatch_calls = LOAD32(&g_kage_vita_stall_state.dispatch_calls);
    out->dispatch_events = LOAD32(&g_kage_vita_stall_state.dispatch_events);
    out->logical_fread_calls =
        LOAD32(&g_kage_vita_stall_state.logical_fread_calls);
    out->loading_swap_count =
        LOAD32(&g_kage_vita_stall_state.loading_swap_count);
    out->report_count = LOAD32(&g_kage_vita_stall_state.report_count);
    out->power_tick_calls =
        LOAD32(&g_kage_vita_stall_state.power_tick_calls);
    out->power_tick_result =
        LOAD32(&g_kage_vita_stall_state.power_tick_result);
}

unsigned kage_vita_stall_get_dispatch_events(
    kage_vita_stall_dispatch_event *out, unsigned capacity)
{
    uint32_t newest;
    uint32_t wanted;
    uint32_t first;
    unsigned written = 0u;
    uint32_t expected;

    if (!out || !capacity)
        return 0u;
    newest = LOAD32(&g_kage_vita_stall_state.dispatch_events);
    wanted = newest < KAGE_VITA_STALL_RING_COUNT
        ? newest : KAGE_VITA_STALL_RING_COUNT;
    if (wanted > capacity)
        wanted = capacity;
    first = newest - wanted + 1u;
    for (expected = first; expected != newest + 1u; ++expected) {
        kage_vita_stall_ring_slot *slot =
            &g_kage_vita_stall_state.ring[
                (expected - 1u) % KAGE_VITA_STALL_RING_COUNT];
        uint32_t before = LOAD32(&slot->sequence);
        kage_vita_stall_dispatch_event event;
        uint32_t after;

        if (before != expected)
            continue;
        event.sequence = before;
        event.calls = LOAD32(&slot->calls);
        event.kind = LOAD32(&slot->kind);
        event.target_rva = LOAD32(&slot->target_rva);
        event.return_rva = LOAD32(&slot->return_rva);
        after = LOAD32(&slot->sequence);
        if (after != before)
            continue;
        out[written++] = event;
    }
    return written;
}

static const char *kage_vita_stall_sync_edge_name(uint32_t edge)
{
    switch (edge) {
    case KAGE_VITA_STALL_SYNC_FREAD_POWER:          return "fread-power";
    case KAGE_VITA_STALL_SYNC_LOADING_SWAP_ENTER:  return "swap-enter";
    case KAGE_VITA_STALL_SYNC_LOADING_SWAP_RETURN: return "swap-return";
    case KAGE_VITA_STALL_SYNC_LOADING_NOTE_RETURN: return "note-return";
    default:                                        return "unknown";
    }
}

static int kage_vita_stall_sync_enabled(uint32_t edge,
                                        uint32_t completed_calls)
{
    if (completed_calls == 0u)
        return edge == KAGE_VITA_STALL_SYNC_LOADING_SWAP_ENTER ||
               edge == KAGE_VITA_STALL_SYNC_LOADING_SWAP_RETURN;
    if (completed_calls < 8192u ||
        (completed_calls & (completed_calls - 1u)) != 0u)
        return 0;
    return edge >= KAGE_VITA_STALL_SYNC_FREAD_POWER &&
           edge <= KAGE_VITA_STALL_SYNC_LOADING_NOTE_RETURN;
}

void kage_vita_stall_trace_sync(uint32_t edge, uint32_t completed_calls)
{
    kage_vita_stall_snapshot snapshot;
    kage_vita_preloop_phase_snapshot phase;
    kage_vita_stall_dispatch_event events[4];
    unsigned count;
    uint32_t sequence;

    if (!LOAD32(&g_kage_vita_stall_state.active) ||
        !kage_vita_stall_sync_enabled(edge, completed_calls))
        return;
    memset(events, 0, sizeof events);
    kage_vita_stall_get_snapshot(&snapshot);
    kage_vita_preloop_phase_get_snapshot(&phase);
    count = kage_vita_stall_get_dispatch_events(events, 4u);
    sequence = ++s_sync_sequence;
    if (edge == KAGE_VITA_STALL_SYNC_FREAD_POWER)
        s_sync_dispatch_remaining = 4u;
    isaac_vita_log(
        "KAGE VITA SYNC: bid40=%.40s q=%u edge=%s fread=%u swaps=%u "
        "phase=%02x/%08x/%u/%u disp=%u/%u/%u pwr=%u/%08x "
        "tail=%c:%08x:%08x:%u,%c:%08x:%08x:%u,"
        "%c:%08x:%08x:%u,%c:%08x:%08x:%u",
        ISAAC_VITA_STALL_BUILD_ID, sequence,
        kage_vita_stall_sync_edge_name(edge), completed_calls,
        snapshot.loading_swap_count, phase.active_mask,
        phase.packed_depths, phase.total_enter_count,
        phase.total_exit_count, snapshot.dispatch_calls,
        snapshot.dispatch_events, count, snapshot.power_tick_calls,
        snapshot.power_tick_result,
        kage_vita_stall_kind_code(events[0].kind),
        events[0].target_rva, events[0].return_rva, events[0].calls,
        kage_vita_stall_kind_code(events[1].kind),
        events[1].target_rva, events[1].return_rva, events[1].calls,
        kage_vita_stall_kind_code(events[2].kind),
        events[2].target_rva, events[2].return_rva, events[2].calls,
        kage_vita_stall_kind_code(events[3].kind),
        events[3].target_rva, events[3].return_rva, events[3].calls);
}

static void kage_vita_stall_log_first_marker(
    uint32_t bit, const char *marker)
{
    kage_vita_stall_snapshot snapshot;
    uint32_t previous;

    previous = __atomic_fetch_or(
        &s_first_marker_mask, bit, __ATOMIC_RELAXED);
    if (previous & bit)
        return;
    kage_vita_stall_get_snapshot(&snapshot);
    isaac_vita_log(
        "KAGE VITA STAGE: build=%s marker=%s phase=%s "
        "loop=%u update=%u render=%u return=%u present=%u",
        ISAAC_VITA_STALL_BUILD_ID, marker,
        kage_vita_stall_phase_name(snapshot.phase), snapshot.loop_count,
        snapshot.update_count, snapshot.render_count,
        snapshot.render_return_count, snapshot.present_count);
}

static void kage_vita_stall_report(
    uint64_t stalled_us, const char *window)
{
    kage_vita_stall_snapshot snapshot;
    kage_vita_stall_dispatch_event events[KAGE_VITA_STALL_LOG_EVENT_COUNT];
    unsigned count;
    unsigned i;
    uint32_t report;

    report = kage_vita_stall_increment(&g_kage_vita_stall_state.report_count);
    kage_vita_stall_get_snapshot(&snapshot);
    isaac_vita_log(
        "KAGE VITA STALL: build=%s window=%s report=%u "
        "stalled_ms=%u phase=%s "
        "loop=%u update=%u render=%u return=%u present=%u "
        "guest_site=0x%08x dispatch_calls=%u dispatch_events=%u",
        ISAAC_VITA_STALL_BUILD_ID, window, report,
        (unsigned)(stalled_us / 1000u),
        kage_vita_stall_phase_name(snapshot.phase), snapshot.loop_count,
        snapshot.update_count, snapshot.render_count,
        snapshot.render_return_count, snapshot.present_count,
        snapshot.guest_site, snapshot.dispatch_calls,
        snapshot.dispatch_events);

    count = kage_vita_stall_get_dispatch_events(
        events, KAGE_VITA_STALL_LOG_EVENT_COUNT);
    for (i = 0u; i < count; ++i) {
        isaac_vita_log(
            "KAGE VITA STALL DISPATCH: build=%s report=%u seq=%u "
            "calls=%u kind=%s target=0x%08x return=0x%08x",
            ISAAC_VITA_STALL_BUILD_ID, report, events[i].sequence,
            events[i].calls,
            kage_vita_stall_kind_name(events[i].kind),
            events[i].target_rva, events[i].return_rva);
    }
}

/* The pre-loop heartbeat must remain useful when the guest has stopped
 * calling every instrumented seam.  It therefore reads only atomically
 * published words and the lock-free dispatch ring: no guest pointer, loading
 * lock, CRT FILE lock, archive dump, or heap walk is reachable from here. */
static void kage_vita_stall_report_preloop(uint64_t elapsed_us)
{
    kage_vita_stall_snapshot snapshot;
    kage_vita_preloop_phase_snapshot phase;
    kage_vita_stall_dispatch_event
        events[KAGE_VITA_PRELOOP_LOG_EVENT_COUNT];
    uint32_t dispatch_delta;
    uint32_t fread_delta;
    uint32_t swap_delta;
    uint32_t phase_enter_delta;
    uint32_t phase_exit_delta;
    uint32_t phase_enter_mask = 0u;
    uint32_t phase_exit_mask = 0u;
    uint32_t report;
    unsigned count;
    unsigned slot;

    memset(events, 0, sizeof events);
    report = kage_vita_stall_increment(
        &g_kage_vita_stall_state.report_count);
    kage_vita_stall_get_snapshot(&snapshot);
    kage_vita_preloop_phase_get_snapshot(&phase);
    count = kage_vita_stall_get_dispatch_events(
        events, KAGE_VITA_PRELOOP_LOG_EVENT_COUNT);
    dispatch_delta = snapshot.dispatch_calls -
        s_preloop_last_dispatch_calls;
    fread_delta = snapshot.logical_fread_calls -
        s_preloop_last_fread_calls;
    swap_delta = snapshot.loading_swap_count -
        s_preloop_last_loading_swaps;
    phase_enter_delta = phase.total_enter_count -
        s_preloop_last_phase.total_enter_count;
    phase_exit_delta = phase.total_exit_count -
        s_preloop_last_phase.total_exit_count;
    for (slot = 0u; slot < KAGE_VITA_PRELOOP_PHASE_COUNT; ++slot) {
        if (phase.slots[slot].enter_count !=
            s_preloop_last_phase.slots[slot].enter_count)
            phase_enter_mask |= 1u << slot;
        if (phase.slots[slot].exit_count !=
            s_preloop_last_phase.slots[slot].exit_count)
            phase_exit_mask |= 1u << slot;
    }
    s_preloop_last_dispatch_calls = snapshot.dispatch_calls;
    s_preloop_last_fread_calls = snapshot.logical_fread_calls;
    s_preloop_last_loading_swaps = snapshot.loading_swap_count;
    s_preloop_last_phase = phase;

    isaac_vita_log(
        "KAGE VITA PRELOOP: bid40=%.40s report=%u elapsed_ms=%u "
        "dispatch_total/delta/events=%u/%u/%u "
        "fread_total/delta=%u/%u loading_swaps_total/delta=%u/%u "
        "phase(a/d/ei/eo)=%02x/%08x/%u:%02x/%u:%02x",
        ISAAC_VITA_STALL_BUILD_ID, report,
        (unsigned)(elapsed_us / UINT64_C(1000)),
        snapshot.dispatch_calls, dispatch_delta, snapshot.dispatch_events,
        snapshot.logical_fread_calls, fread_delta,
        snapshot.loading_swap_count, swap_delta,
        phase.active_mask, phase.packed_depths,
        phase_enter_delta, phase_enter_mask,
        phase_exit_delta, phase_exit_mask);
    isaac_vita_log(
        "KAGE VITA PRELOOP TAIL: bid40=%.40s report=%u count=%u "
        "entries=%c:%08x:%08x:%u,%c:%08x:%08x:%u,"
        "%c:%08x:%08x:%u,%c:%08x:%08x:%u",
        ISAAC_VITA_STALL_BUILD_ID, report, count,
        kage_vita_stall_kind_code(events[0].kind),
        events[0].target_rva, events[0].return_rva, events[0].calls,
        kage_vita_stall_kind_code(events[1].kind),
        events[1].target_rva, events[1].return_rva, events[1].calls,
        kage_vita_stall_kind_code(events[2].kind),
        events[2].target_rva, events[2].return_rva, events[2].calls,
        kage_vita_stall_kind_code(events[3].kind),
        events[3].target_rva, events[3].return_rva, events[3].calls);
}

static void kage_vita_stall_poll(uint64_t now_us)
{
    uint32_t loop;
    uint32_t present;
    uint64_t stalled_us;

    if (!LOAD32(&g_kage_vita_stall_state.active))
        return;
    present = LOAD32(&g_kage_vita_stall_state.present_count);
    if (!present) {
        loop = LOAD32(&g_kage_vita_stall_state.loop_count);
        if (!loop) {
            /* Unlike the five-second stall detector, this is an independent
             * low-frequency heartbeat.  Deltas distinguish slow continuing
             * startup from a runaway reached after the last fread/dispatch. */
            stalled_us = now_us >= s_last_progress_us
                ? now_us - s_last_progress_us : 0u;
            if (stalled_us < s_next_report_us)
                return;
            kage_vita_stall_report_preloop(stalled_us);
            if (stalled_us >= KAGE_VITA_PRELOOP_REPEAT_REPORT_US) {
                s_next_report_us =
                    (stalled_us / KAGE_VITA_PRELOOP_REPEAT_REPORT_US + 1u) *
                    KAGE_VITA_PRELOOP_REPEAT_REPORT_US;
            } else if (s_next_report_us ==
                       KAGE_VITA_PRELOOP_FIRST_REPORT_US) {
                s_next_report_us = KAGE_VITA_PRELOOP_SECOND_REPORT_US;
            } else {
                s_next_report_us = KAGE_VITA_PRELOOP_REPEAT_REPORT_US;
            }
            return;
        }
        if (!s_pre_present_armed) {
            s_pre_present_armed = 1u;
            s_last_progress_us = now_us;
            s_next_report_us = KAGE_VITA_STALL_FIRST_REPORT_US;
            return;
        }
        stalled_us = now_us >= s_last_progress_us
            ? now_us - s_last_progress_us : 0u;
        if (!s_next_report_us || stalled_us < s_next_report_us)
            return;
        kage_vita_stall_report(stalled_us, "pre-present");
        if (s_next_report_us == KAGE_VITA_STALL_FIRST_REPORT_US)
            s_next_report_us = KAGE_VITA_STALL_SECOND_REPORT_US;
        else
            s_next_report_us = 0u; /* Exactly 5 s and 10 s; never flood. */
        return;
    }
    if (present != s_tracked_present) {
        s_tracked_present = present;
        s_pre_present_armed = 0u;
        s_last_progress_us = now_us;
        s_next_report_us = KAGE_VITA_STALL_FIRST_REPORT_US;
        return;
    }
    stalled_us = now_us >= s_last_progress_us
        ? now_us - s_last_progress_us : 0u;
    if (stalled_us < s_next_report_us)
        return;
    kage_vita_stall_report(stalled_us, "post-present");
    if (s_next_report_us == KAGE_VITA_STALL_FIRST_REPORT_US)
        s_next_report_us = KAGE_VITA_STALL_SECOND_REPORT_US;
    else
        s_next_report_us += KAGE_VITA_STALL_REPEAT_REPORT_US;
}

#ifdef ISAAC_KAGE_VITA_STALL_ORACLE
void kage_vita_stall_oracle_poll(uint64_t now_us)
{
    (void)kage_vita_stall_power_tick();
    kage_vita_stall_poll(now_us);
}
#endif

static int kage_vita_stall_watchdog(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    while (!LOAD32(&g_kage_vita_stall_state.stop)) {
        (void)kage_vita_stall_power_tick();
        (void)sceKernelDelayThread(KAGE_VITA_STALL_POLL_US);
        if (!LOAD32(&g_kage_vita_stall_state.stop))
            kage_vita_stall_poll(sceKernelGetProcessTimeWide());
    }
    return 0;
}

int kage_vita_stall_probe_start(void)
{
    SceKernelThreadInfo info;
    SceUID owner;
    SceUID thread;
    int priority = KAGE_VITA_STALL_DEFAULT_PRIORITY;
    int power_tick_result;
    int result;

    if (s_watchdog_thread >= 0)
        return 1;
    kage_vita_stall_reset_state();
    owner = sceKernelGetThreadId();
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    if (sceKernelGetThreadInfo(owner, &info) >= 0 && info.currentPriority)
        priority = info.currentPriority;

    thread = sceKernelCreateThread(
        "isaac_stall_watch", kage_vita_stall_watchdog, priority,
        KAGE_VITA_STALL_STACK_SIZE, 0u, 0, NULL);
    if (thread < 0) {
        isaac_vita_log(
            "KAGE VITA STALL WATCHDOG: build=%s state=create-failed "
            "result=0x%08x priority=0x%08x",
            ISAAC_VITA_STALL_BUILD_ID,
            (unsigned)thread, (unsigned)priority);
        return 0;
    }
    STORE32(&g_kage_vita_stall_state.active, 1u);
    power_tick_result = kage_vita_stall_power_tick();
    result = sceKernelStartThread(thread, 0u, NULL);
    if (result < 0) {
        STORE32(&g_kage_vita_stall_state.active, 0u);
        (void)sceKernelDeleteThread(thread);
        isaac_vita_log(
            "KAGE VITA STALL WATCHDOG: build=%s state=start-failed "
            "result=0x%08x thread=0x%08x",
            ISAAC_VITA_STALL_BUILD_ID,
            (unsigned)result, (unsigned)thread);
        return 0;
    }
    s_watchdog_thread = thread;
    isaac_vita_log(
        "KAGE VITA STALL WATCHDOG: build=%s state=start thread=0x%08x "
        "priority=0x%08x power_tick=0x%08x stall_threshold_ms=5000 "
        "preloop_first_ms=30000 preloop_repeat_ms=120000 ring=%u",
        ISAAC_VITA_STALL_BUILD_ID, (unsigned)thread, (unsigned)priority,
        (unsigned)power_tick_result,
        (unsigned)KAGE_VITA_STALL_RING_COUNT);
    return 1;
}

void kage_vita_stall_probe_stop(void)
{
    SceUID thread = s_watchdog_thread;
    int wait_result;
    int delete_result;

    STORE32(&g_kage_vita_stall_state.active, 0u);
    STORE32(&g_kage_vita_stall_state.stop, 1u);
    if (thread < 0) {
        STORE32(&g_kage_vita_stall_state.phase,
                KAGE_VITA_STALL_INACTIVE);
        return;
    }
    wait_result = sceKernelWaitThreadEnd(thread, NULL, NULL);
    delete_result = sceKernelDeleteThread(thread);
    s_watchdog_thread = -1;
    STORE32(&g_kage_vita_stall_state.phase, KAGE_VITA_STALL_INACTIVE);
    isaac_vita_log(
        "KAGE VITA STALL WATCHDOG: build=%s state=stop thread=0x%08x "
        "wait=0x%08x delete=0x%08x",
        ISAAC_VITA_STALL_BUILD_ID, (unsigned)thread,
        (unsigned)wait_result, (unsigned)delete_result);
}

void kage_vita_stall_note_loop(void)
{
    if (!LOAD32(&g_kage_vita_stall_state.active))
        return;
    (void)kage_vita_stall_increment(&g_kage_vita_stall_state.loop_count);
    STORE32(&g_kage_vita_stall_state.phase, KAGE_VITA_STALL_LOOP);
    STORE32(&g_kage_vita_stall_state.guest_site, 0u);
    kage_vita_stall_log_first_marker(
        KAGE_VITA_STALL_MARKER_LOOP, "LOOP");
}

void kage_vita_stall_note_update(void)
{
    if (!LOAD32(&g_kage_vita_stall_state.active))
        return;
    (void)kage_vita_stall_increment(&g_kage_vita_stall_state.update_count);
    STORE32(&g_kage_vita_stall_state.phase, KAGE_VITA_STALL_UPDATE);
    /* Exact Manager::Update entry RVA.  Later generated seam breadcrumbs
     * replace this with the last site crossed before a deeper call. */
    STORE32(&g_kage_vita_stall_state.guest_site, 0x004b0010u);
    kage_vita_stall_log_first_marker(
        KAGE_VITA_STALL_MARKER_UPDATE, "UPDATE");
}

void kage_vita_stall_note_render(void)
{
    if (!LOAD32(&g_kage_vita_stall_state.active))
        return;
    (void)kage_vita_stall_increment(&g_kage_vita_stall_state.render_count);
    STORE32(&g_kage_vita_stall_state.phase, KAGE_VITA_STALL_RENDER);
    STORE32(&g_kage_vita_stall_state.guest_site, 0u);
    kage_vita_stall_log_first_marker(
        KAGE_VITA_STALL_MARKER_RENDER, "RENDER");
}

void kage_vita_stall_note_render_return(void)
{
    if (!LOAD32(&g_kage_vita_stall_state.active))
        return;
    (void)kage_vita_stall_increment(
        &g_kage_vita_stall_state.render_return_count);
    STORE32(&g_kage_vita_stall_state.phase, KAGE_VITA_STALL_POST_RENDER);
    kage_vita_stall_log_first_marker(
        KAGE_VITA_STALL_MARKER_RENDER_RETURN, "RETURN");
}

void kage_vita_stall_note_present_enter(uint32_t completed_count)
{
    if (!LOAD32(&g_kage_vita_stall_state.active))
        return;
    STORE32(&g_kage_vita_stall_state.present_count, completed_count);
    STORE32(&g_kage_vita_stall_state.phase, KAGE_VITA_STALL_PRESENT);
    kage_vita_stall_log_first_marker(
        KAGE_VITA_STALL_MARKER_PRESENT_ENTER, "PRESENT-enter");
}

void kage_vita_stall_note_present_return(uint32_t completed_count)
{
    if (!LOAD32(&g_kage_vita_stall_state.active))
        return;
    STORE32(&g_kage_vita_stall_state.present_count, completed_count);
    STORE32(&g_kage_vita_stall_state.phase, KAGE_VITA_STALL_POST_PRESENT);
    kage_vita_stall_log_first_marker(
        KAGE_VITA_STALL_MARKER_PRESENT_RETURN, "PRESENT-return");
}

void kage_vita_stall_note_guest_site(uint32_t site_rva)
{
    if (LOAD32_RELAXED(&g_kage_vita_stall_state.active))
        STORE32(&g_kage_vita_stall_state.guest_site, site_rva);
}

void kage_vita_stall_note_logical_fread(uint32_t completed_calls)
{
    if (LOAD32_RELAXED(&g_kage_vita_stall_state.active))
        STORE32(&g_kage_vita_stall_state.logical_fread_calls,
                completed_calls);
}

void kage_vita_stall_note_loading_swap(uint32_t completed_swaps)
{
    if (LOAD32_RELAXED(&g_kage_vita_stall_state.active))
        STORE32(&g_kage_vita_stall_state.loading_swap_count,
                completed_swaps);
}

void kage_vita_stall_note_dispatch(uint32_t kind, uint32_t target_rva,
                                   uint32_t return_rva)
{
    kage_vita_stall_ring_slot *slot;
    uint32_t calls;
    uint32_t sequence;

    if (!LOAD32_RELAXED(&g_kage_vita_stall_state.active))
        return;
    calls = ++s_dispatch_writer.calls;
    STORE32_RELAXED(&g_kage_vita_stall_state.dispatch_calls, calls);
    if (kind != s_dispatch_writer.last_kind ||
        target_rva != s_dispatch_writer.last_target ||
        return_rva != s_dispatch_writer.last_return) {
        s_dispatch_writer.last_kind = kind;
        s_dispatch_writer.last_target = target_rva;
        s_dispatch_writer.last_return = return_rva;
        sequence = ++s_dispatch_writer.events;
        if (!sequence)
            sequence = ++s_dispatch_writer.events;
        slot = &g_kage_vita_stall_state.ring[
            (sequence - 1u) % KAGE_VITA_STALL_RING_COUNT];
        STORE32_RELAXED(&slot->sequence, 0u);
        STORE32_RELAXED(&slot->calls, calls);
        STORE32_RELAXED(&slot->kind, kind);
        STORE32_RELAXED(&slot->target_rva, target_rva);
        STORE32_RELAXED(&slot->return_rva, return_rva);
        STORE32(&slot->sequence, sequence);
        /* The slot's release stamp publishes its payload.  The global cursor
         * is only a cheap range hint; keeping this store relaxed avoids a
         * second DMB on each changed indirect target. */
        STORE32_RELAXED(&g_kage_vita_stall_state.dispatch_events, sequence);
    }
    if (s_sync_dispatch_remaining) {
        uint32_t ordinal = 5u - s_sync_dispatch_remaining;
        --s_sync_dispatch_remaining;
        isaac_vita_log(
            "KAGE VITA SYNC DISPATCH: bid40=%.40s n=%u calls=%u "
            "kind=%c target=%08x return=%08x",
            ISAAC_VITA_STALL_BUILD_ID, ordinal, calls,
            kage_vita_stall_kind_code(kind), target_rva, return_rva);
    }
}
