#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kage_vita_preloop_phase.h"
#include "kage_vita_stall_probe.h"

typedef int SceUID;
typedef uint32_t SceSize;
typedef uint32_t SceUInt;
typedef int (*SceKernelThreadEntry)(SceSize args, void *argp);
typedef struct SceKernelThreadInfo {
    SceSize size;
    int currentPriority;
} SceKernelThreadInfo;

#define ORACLE_LOG_CAPACITY 96u
#define ORACLE_LOG_LENGTH   512u

static uint64_t s_now;
static int s_create_result = 7;
static int s_start_result;
static int s_current_priority = 0x10000100;
static int s_created_priority;
static uint32_t s_created_stack;
static SceKernelThreadEntry s_created_entry;
static unsigned s_create_calls;
static unsigned s_wait_calls;
static unsigned s_delete_calls;
static unsigned s_power_tick_calls;
static int s_power_tick_result;
static unsigned s_log_count;
static char s_logs[ORACLE_LOG_CAPACITY][ORACLE_LOG_LENGTH];
static int s_log_lengths[ORACLE_LOG_CAPACITY];
static kage_vita_preloop_phase_snapshot s_phase_snapshot;
static unsigned s_phase_reset_calls;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Vita stall probe oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static int oracle_log_contains(const char *needle)
{
    unsigned i;
    for (i = 0u; i < s_log_count; ++i) {
        if (strstr(s_logs[i], needle))
            return 1;
    }
    return 0;
}

static unsigned oracle_log_count_contains(const char *needle)
{
    unsigned count = 0u;
    unsigned i;

    for (i = 0u; i < s_log_count; ++i) {
        if (strstr(s_logs[i], needle))
            ++count;
    }
    return count;
}

SceUID sceKernelCreateThread(
    const char *name, SceKernelThreadEntry entry, int priority,
    SceSize stack_size, SceUInt attributes, int affinity, const void *options)
{
    CHECK(name && strcmp(name, "isaac_stall_watch") == 0);
    CHECK(entry != NULL);
    CHECK(attributes == 0u);
    CHECK(affinity == 0);
    CHECK(options == NULL);
    s_created_entry = entry;
    s_created_priority = priority;
    s_created_stack = stack_size;
    ++s_create_calls;
    return s_create_result;
}

int sceKernelDeleteThread(SceUID thread)
{
    CHECK(thread == 7);
    ++s_delete_calls;
    return 0;
}

int sceKernelStartThread(SceUID thread, SceSize args, void *argp)
{
    CHECK(thread == 7);
    CHECK(args == 0u);
    CHECK(argp == NULL);
    return s_start_result;
}

int sceKernelWaitThreadEnd(SceUID thread, int *status, SceUInt *timeout)
{
    CHECK(thread == 7);
    CHECK(status == NULL);
    CHECK(timeout == NULL);
    ++s_wait_calls;
    return 0;
}

int sceKernelDelayThread(SceUInt delay)
{
    s_now += delay;
    return 0;
}

int sceKernelGetThreadId(void)
{
    return 3;
}

int sceKernelGetThreadInfo(SceUID thread, SceKernelThreadInfo *info)
{
    CHECK(thread == 3);
    CHECK(info && info->size == sizeof *info);
    info->currentPriority = s_current_priority;
    return 0;
}

uint64_t sceKernelGetProcessTimeWide(void)
{
    return s_now;
}

int sceKernelPowerTick(int type)
{
    CHECK(type == 1);
    ++s_power_tick_calls;
    return s_power_tick_result;
}

void kage_vita_preloop_phase_reset(void)
{
    memset(&s_phase_snapshot, 0, sizeof s_phase_snapshot);
    ++s_phase_reset_calls;
}

void kage_vita_preloop_phase_get_snapshot(
    kage_vita_preloop_phase_snapshot *out)
{
    if (out)
        *out = s_phase_snapshot;
}

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;

    if (s_log_count >= ORACLE_LOG_CAPACITY)
        return;
    va_start(arguments, format);
    s_log_lengths[s_log_count] = vsnprintf(
        s_logs[s_log_count], ORACLE_LOG_LENGTH, format, arguments);
    va_end(arguments);
    ++s_log_count;
}

int main(void)
{
    kage_vita_stall_snapshot snapshot;
    kage_vita_stall_dispatch_event events[4];
    unsigned before_logs;
    unsigned count;
    unsigned i;

    CHECK(kage_vita_stall_probe_start() == 1);
    CHECK(s_phase_reset_calls == 1u);
    CHECK(kage_vita_stall_probe_start() == 1);
    CHECK(s_phase_reset_calls == 1u);
    CHECK(s_create_calls == 1u);
    CHECK(s_created_entry != NULL);
    CHECK(s_created_priority == s_current_priority);
    CHECK(s_created_stack == 0x4000u);
    CHECK(s_power_tick_calls == 1u);
    CHECK(oracle_log_contains(
        "state=start thread=0x00000007"));
    CHECK(oracle_log_contains("power_tick=0x00000000"));

    /* Pre-loop heartbeat has its own intentionally slow 30-second deadline. */
    before_logs = s_log_count;
    kage_vita_stall_oracle_poll(29999000u);
    CHECK(s_log_count == before_logs);
    CHECK(s_power_tick_calls == 2u);

    kage_vita_stall_note_loop();
    kage_vita_stall_note_update();
    kage_vita_stall_note_guest_site(0x004b01fbu);
    kage_vita_stall_note_dispatch(
        KAGE_VITA_STALL_DISPATCH_IMPORT, 0x00680010u, 0x004b0200u);
    for (i = 0u; i < 10u; ++i) {
        kage_vita_stall_note_dispatch(
            KAGE_VITA_STALL_DISPATCH_IMPORT, 0x00680010u, 0x004b0200u);
    }
    kage_vita_stall_note_dispatch(
        KAGE_VITA_STALL_DISPATCH_INDIRECT, 0x002cdcf0u, 0x004b0316u);
    kage_vita_stall_get_snapshot(&snapshot);
    CHECK(snapshot.active == 1u);
    CHECK(snapshot.phase == KAGE_VITA_STALL_UPDATE);
    CHECK(snapshot.loop_count == 1u && snapshot.update_count == 1u);
    CHECK(snapshot.guest_site == 0x004b01fbu);
    CHECK(snapshot.dispatch_calls == 12u);
    CHECK(snapshot.dispatch_events == 2u);
    count = kage_vita_stall_get_dispatch_events(events, 4u);
    CHECK(count == 2u);
    CHECK(events[0].kind == KAGE_VITA_STALL_DISPATCH_IMPORT);
    CHECK(events[0].calls == 1u && events[0].target_rva == 0x00680010u);
    CHECK(events[1].kind == KAGE_VITA_STALL_DISPATCH_INDIRECT);
    CHECK(events[1].calls == 12u && events[1].target_rva == 0x002cdcf0u);
    CHECK(oracle_log_count_contains("marker=LOOP") == 1u);
    CHECK(oracle_log_count_contains("marker=UPDATE") == 1u);

    /* Loading before the generated loop is intentionally unarmed.  The first
     * observed LOOP arms a separate, bounded first-present window. */
    before_logs = s_log_count;
    kage_vita_stall_oracle_poll(60000000u);
    kage_vita_stall_oracle_poll(64999000u);
    CHECK(s_log_count == before_logs);
    kage_vita_stall_oracle_poll(65001000u);
    kage_vita_stall_get_snapshot(&snapshot);
    CHECK(snapshot.report_count == 1u);
    CHECK(oracle_log_contains(
        "window=pre-present report=1 stalled_ms=5001 phase=update"));
    CHECK(oracle_log_contains("build="));
    CHECK(oracle_log_contains("guest_site=0x004b01fb"));
    CHECK(oracle_log_contains(
        "kind=indirect target=0x002cdcf0 return=0x004b0316"));

    before_logs = s_log_count;
    kage_vita_stall_oracle_poll(69999000u);
    CHECK(s_log_count == before_logs);
    kage_vita_stall_oracle_poll(70001000u);
    kage_vita_stall_get_snapshot(&snapshot);
    CHECK(snapshot.report_count == 2u);
    CHECK(oracle_log_contains(
        "window=pre-present report=2 stalled_ms=10001 phase=update"));

    /* Pre-present is deliberately two snapshots, not an unbounded periodic
     * writer.  Post-present keeps the existing periodic watchdog behavior. */
    before_logs = s_log_count;
    kage_vita_stall_oracle_poll(120000000u);
    CHECK(s_log_count == before_logs);
    CHECK(snapshot.report_count == 2u);

    /* Establish one completed frame, then park in the next Update. */
    kage_vita_stall_note_render();
    kage_vita_stall_note_render_return();
    kage_vita_stall_note_present_enter(0u);
    kage_vita_stall_note_present_return(1u);
    kage_vita_stall_oracle_poll(120000000u);
    kage_vita_stall_note_loop();
    kage_vita_stall_note_update();
    kage_vita_stall_note_guest_site(0x004b01fbu);
    before_logs = s_log_count;
    kage_vita_stall_oracle_poll(124999000u);
    CHECK(s_log_count == before_logs);
    kage_vita_stall_oracle_poll(125001000u);
    kage_vita_stall_get_snapshot(&snapshot);
    CHECK(snapshot.report_count == 3u);
    CHECK(oracle_log_contains(
        "window=post-present report=3 stalled_ms=5001 phase=update"));
    CHECK(oracle_log_contains("guest_site=0x004b01fb"));
    CHECK(oracle_log_contains(
        "kind=indirect target=0x002cdcf0 return=0x004b0316"));

    before_logs = s_log_count;
    kage_vita_stall_oracle_poll(129999000u);
    CHECK(s_log_count == before_logs);
    kage_vita_stall_oracle_poll(130001000u);
    kage_vita_stall_get_snapshot(&snapshot);
    CHECK(snapshot.report_count == 4u);
    CHECK(oracle_log_contains(
        "window=post-present report=4 stalled_ms=10001 phase=update"));

    /* A completed present resets the deadline instead of emitting a stale
     * report inherited from the prior frozen window. */
    kage_vita_stall_note_present_enter(1u);
    kage_vita_stall_note_present_return(2u);
    kage_vita_stall_oracle_poll(131000000u);
    before_logs = s_log_count;
    kage_vita_stall_oracle_poll(135999000u);
    CHECK(s_log_count == before_logs);
    CHECK(oracle_log_count_contains("marker=LOOP") == 1u);
    CHECK(oracle_log_count_contains("marker=UPDATE") == 1u);
    CHECK(oracle_log_count_contains("marker=RENDER") == 1u);
    CHECK(oracle_log_count_contains("marker=RETURN") == 1u);
    CHECK(oracle_log_count_contains("marker=PRESENT-enter") == 1u);
    CHECK(oracle_log_count_contains("marker=PRESENT-return") == 1u);

    /* More than one ring's worth of changed targets returns the newest tail
     * in chronological order without exposing an in-progress slot. */
    for (i = 0u; i < 40u; ++i) {
        kage_vita_stall_note_dispatch(
            KAGE_VITA_STALL_DISPATCH_INDIRECT,
            0x00100000u + i, 0x00200000u + i);
    }
    count = kage_vita_stall_get_dispatch_events(events, 4u);
    CHECK(count == 4u);
    CHECK(events[0].sequence == 39u &&
          events[0].target_rva == 0x00100024u);
    CHECK(events[3].sequence == 42u &&
          events[3].target_rva == 0x00100027u);

    /* Synchronous power-of-two checkpoints bracket the risky loading swap,
     * preserve a four-event tail, and arm exactly four following dispatch
     * records even when the target tuple does not change. */
    before_logs = s_log_count;
    kage_vita_stall_trace_sync(
        KAGE_VITA_STALL_SYNC_FREAD_POWER, 8191u);
    CHECK(s_log_count == before_logs);
    kage_vita_stall_trace_sync(
        KAGE_VITA_STALL_SYNC_FREAD_POWER, 8192u);
    CHECK(s_log_count == before_logs + 1u);
    CHECK(oracle_log_contains("edge=fread-power fread=8192"));
    CHECK(oracle_log_contains("tail=D:00100024:00200024:"));
    before_logs = s_log_count;
    for (i = 0u; i < 5u; ++i) {
        kage_vita_stall_note_dispatch(
            KAGE_VITA_STALL_DISPATCH_IMPORT,
            0x00606494u, 0x0059c383u);
    }
    CHECK(s_log_count == before_logs + 4u);
    CHECK(oracle_log_count_contains("KAGE VITA SYNC DISPATCH:") == 4u);
    kage_vita_stall_trace_sync(
        KAGE_VITA_STALL_SYNC_LOADING_SWAP_ENTER, 8192u);
    kage_vita_stall_trace_sync(
        KAGE_VITA_STALL_SYNC_LOADING_SWAP_RETURN, 8192u);
    kage_vita_stall_trace_sync(
        KAGE_VITA_STALL_SYNC_LOADING_NOTE_RETURN, 8192u);
    kage_vita_stall_trace_sync(
        KAGE_VITA_STALL_SYNC_LOADING_SWAP_ENTER, 0u);
    kage_vita_stall_trace_sync(
        KAGE_VITA_STALL_SYNC_LOADING_SWAP_RETURN, 0u);
    CHECK(oracle_log_contains("edge=swap-enter fread=8192"));
    CHECK(oracle_log_contains("edge=swap-return fread=8192"));
    CHECK(oracle_log_contains("edge=note-return fread=8192"));
    CHECK(oracle_log_contains("edge=swap-enter fread=0"));
    CHECK(oracle_log_contains("edge=swap-return fread=0"));

    kage_vita_stall_probe_stop();
    kage_vita_stall_probe_stop();
    CHECK(s_wait_calls == 1u && s_delete_calls == 1u);
    kage_vita_stall_get_snapshot(&snapshot);
    CHECK(snapshot.active == 0u);
    CHECK(snapshot.phase == KAGE_VITA_STALL_INACTIVE);
    CHECK(oracle_log_contains("state=stop thread=0x00000007"));

    /* A second isolated epoch exercises the independent zero-LOOP heartbeat.
     * It must report continuing and stopped progress without touching a CRT
     * or loading lock, retain the newest four dispatch transitions, and skip
     * 180 seconds after adopting the 120-second long-run cadence. */
    memset(&s_phase_snapshot, 0xa5, sizeof s_phase_snapshot);
    CHECK(kage_vita_stall_probe_start() == 1);
    CHECK(s_phase_reset_calls == 2u);
    CHECK(s_phase_snapshot.active_mask == 0u);
    CHECK(s_phase_snapshot.total_enter_count == 0u);
    CHECK(s_phase_snapshot.slots[
        KAGE_VITA_PRELOOP_INFLATE_BLOCKS].max_depth == 0u);
    memset(&s_phase_snapshot, 0, sizeof s_phase_snapshot);
    s_phase_snapshot.active_mask =
        1u << KAGE_VITA_PRELOOP_PLATFORM_INIT;
    s_phase_snapshot.packed_depths = 0x00000010u;
    s_phase_snapshot.total_enter_count = 2u;
    s_phase_snapshot.total_exit_count = 1u;
    s_phase_snapshot.slots[KAGE_VITA_PRELOOP_GLFW_ERROR].enter_count = 1u;
    s_phase_snapshot.slots[KAGE_VITA_PRELOOP_GLFW_ERROR].exit_count = 1u;
    s_phase_snapshot.slots[KAGE_VITA_PRELOOP_PLATFORM_INIT].enter_count = 1u;
    s_phase_snapshot.slots[KAGE_VITA_PRELOOP_PLATFORM_INIT].current_depth = 1u;
    kage_vita_stall_note_logical_fread(512u);
    kage_vita_stall_note_loading_swap(1u);
    for (i = 0u; i < 4u; ++i) {
        kage_vita_stall_note_dispatch(
            i & 1u ? KAGE_VITA_STALL_DISPATCH_INDIRECT
                   : KAGE_VITA_STALL_DISPATCH_IMPORT,
            0xfffffff0u + i, 0xeeeefff0u + i);
    }
    before_logs = s_log_count;
    kage_vita_stall_oracle_poll(29999000u);
    CHECK(s_log_count == before_logs);
    kage_vita_stall_oracle_poll(30001000u);
    kage_vita_stall_get_snapshot(&snapshot);
    CHECK(snapshot.report_count == 1u);
    CHECK(snapshot.logical_fread_calls == 512u);
    CHECK(snapshot.loading_swap_count == 1u);
    CHECK(s_log_count == before_logs + 2u);
    CHECK(oracle_log_contains(
        "report=1 elapsed_ms=30001 dispatch_total/delta/events=4/4/4"));
    CHECK(oracle_log_contains(
        "fread_total/delta=512/512 loading_swaps_total/delta=1/1"));
    CHECK(oracle_log_contains(
        "phase(a/d/ei/eo)=02/00000010/2:03/1:01"));
    CHECK(oracle_log_contains(
        "report=1 count=4 entries=I:fffffff0:eeeefff0:1,"));

    for (i = 0u; i < 5u; ++i) {
        kage_vita_stall_note_dispatch(
            KAGE_VITA_STALL_DISPATCH_INDIRECT,
            0xfffffff3u, 0xeeeefff3u);
    }
    kage_vita_stall_note_logical_fread(1536u);
    kage_vita_stall_note_loading_swap(4u);
    s_phase_snapshot.active_mask =
        (1u << KAGE_VITA_PRELOOP_ISAAC_STARTUP) |
        (1u << KAGE_VITA_PRELOOP_LOAD_ARCHIVE);
    s_phase_snapshot.packed_depths = 0x00001100u;
    s_phase_snapshot.total_enter_count = 4u;
    s_phase_snapshot.total_exit_count = 2u;
    s_phase_snapshot.slots[KAGE_VITA_PRELOOP_PLATFORM_INIT].exit_count = 1u;
    s_phase_snapshot.slots[KAGE_VITA_PRELOOP_PLATFORM_INIT].current_depth = 0u;
    s_phase_snapshot.slots[KAGE_VITA_PRELOOP_ISAAC_STARTUP].enter_count = 1u;
    s_phase_snapshot.slots[KAGE_VITA_PRELOOP_ISAAC_STARTUP].current_depth = 1u;
    s_phase_snapshot.slots[KAGE_VITA_PRELOOP_LOAD_ARCHIVE].enter_count = 1u;
    s_phase_snapshot.slots[KAGE_VITA_PRELOOP_LOAD_ARCHIVE].current_depth = 1u;
    before_logs = s_log_count;
    kage_vita_stall_oracle_poll(59999000u);
    CHECK(s_log_count == before_logs);
    kage_vita_stall_oracle_poll(60001000u);
    CHECK(oracle_log_contains(
        "report=2 elapsed_ms=60001 dispatch_total/delta/events=9/5/4"));
    CHECK(oracle_log_contains(
        "fread_total/delta=1536/1024 loading_swaps_total/delta=4/3"));
    CHECK(oracle_log_contains(
        "phase(a/d/ei/eo)=0c/00001100/2:0c/1:02"));

    before_logs = s_log_count;
    kage_vita_stall_oracle_poll(119999000u);
    CHECK(s_log_count == before_logs);
    kage_vita_stall_oracle_poll(120001000u);
    kage_vita_stall_get_snapshot(&snapshot);
    CHECK(snapshot.report_count == 3u);
    CHECK(oracle_log_contains(
        "report=3 elapsed_ms=120001 dispatch_total/delta/events=9/0/4"));
    CHECK(oracle_log_contains(
        "phase(a/d/ei/eo)=0c/00001100/0:00/0:00"));
    before_logs = s_log_count;
    kage_vita_stall_oracle_poll(180001000u);
    CHECK(s_log_count == before_logs);
    kage_vita_stall_oracle_poll(240001000u);
    kage_vita_stall_get_snapshot(&snapshot);
    CHECK(snapshot.report_count == 4u);
    CHECK(s_log_count == before_logs + 2u);

    for (i = 0u; i < s_log_count; ++i) {
        if (strstr(s_logs[i], "KAGE VITA PRELOOP") ||
            strstr(s_logs[i], "KAGE VITA SYNC")) {
            CHECK(s_log_lengths[i] > 0);
            CHECK(s_log_lengths[i] < 384);
        }
    }
    kage_vita_stall_probe_stop();
    CHECK(s_wait_calls == 2u && s_delete_calls == 2u);

    /* Both create and start failure remain fail-open for gameplay. */
    s_start_result = -9;
    CHECK(kage_vita_stall_probe_start() == 0);
    kage_vita_stall_get_snapshot(&snapshot);
    CHECK(snapshot.active == 0u);
    CHECK(s_delete_calls == 3u);
    CHECK(oracle_log_contains(
        "state=start-failed result=0xfffffff7"));
    s_start_result = 0;
    s_create_result = -5;
    CHECK(kage_vita_stall_probe_start() == 0);
    kage_vita_stall_get_snapshot(&snapshot);
    CHECK(snapshot.active == 0u);
    CHECK(oracle_log_contains(
        "state=create-failed result=0xfffffffb"));

    puts("Vita guest-stall watchdog oracle: PASS");
    return 0;
}
