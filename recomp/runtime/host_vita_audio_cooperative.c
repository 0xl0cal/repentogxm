#include "host_vita_audio_cooperative.h"

#ifndef ISAAC_VITA_AUDIO
#define ISAAC_VITA_AUDIO 0
#endif

#if ISAAC_VITA_AUDIO
#include "host_vita_audio.h"
#include "platform.h"
#include "vita_host_services.h"

#if defined(ISAAC_VITA_AUDIO_STREAM_RECEIPT)
#include <string.h>

typedef struct audio_stream_receipt_item {
    uint32_t object;
    uint32_t source;
    uint16_t queue_states;
    uint8_t stopped;
    uint8_t loop;
    uint8_t eof;
    uint8_t queue_empty;
} audio_stream_receipt_item;

typedef struct audio_stream_receipt_snapshot {
    audio_stream_receipt_item
        items[ISAAC_VITA_AUDIO_STREAM_SOURCE_HEADROOM];
    uint32_t ogg_count;
    uint32_t queue_empty_count;
    uint32_t stopped_count;
    uint32_t eof_count;
    uint8_t valid;
    uint8_t overflow;
    uint8_t invalid_queue_state;
} audio_stream_receipt_snapshot;

enum audio_stream_receipt_edge {
    AUDIO_STREAM_EDGE_NONE = 0,
    AUDIO_STREAM_EDGE_QUEUE_EMPTY,
    AUDIO_STREAM_EDGE_EMPTY_REPEAT,
    AUDIO_STREAM_EDGE_REFILL,
    AUDIO_STREAM_EDGE_STOPPED,
    AUDIO_STREAM_EDGE_REMOVED,
    AUDIO_STREAM_EDGE_INVALID,
    AUDIO_STREAM_EDGE_COUNT
};

static uint32_t s_audio_stream_receipt_sequence;
static uint32_t s_audio_stream_receipt_successes;
static uint32_t s_audio_stream_receipt_faults;
static uint32_t s_audio_stream_receipt_edges[AUDIO_STREAM_EDGE_COUNT];
static uint32_t s_audio_stream_receipt_max_us;

static uint32_t audio_stream_receipt_increment(uint32_t *value)
{
    if (*value != UINT32_MAX)
        ++*value;
    return *value;
}

static int audio_stream_receipt_power_of_two(uint32_t value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

static uint32_t audio_stream_receipt_elapsed(uint64_t begin, uint64_t end)
{
    uint64_t elapsed;

    if (end < begin)
        return 0U;
    elapsed = end - begin;
    return elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
}

static void audio_stream_receipt_snapshot_load(
    uint32_t manager, audio_stream_receipt_snapshot *snapshot)
{
    uint32_t begin;
    uint32_t end;
    uint32_t at;

    memset(snapshot, 0, sizeof *snapshot);
    begin = ld32(manager + ISAAC_VITA_AUDIO_MANAGER_ACTIVE_BEGIN_OFFSET);
    end = ld32(manager + ISAAC_VITA_AUDIO_MANAGER_ACTIVE_END_OFFSET);
    if (begin == end) {
        snapshot->valid = 1U;
        return;
    }
    if (begin == 0U || end < begin || ((begin | end) & 3U) != 0U ||
            end - begin > ISAAC_VITA_AUDIO_DEVICE_SOURCE_COUNT * 4U)
        return;

    snapshot->valid = 1U;
    for (at = begin; at != end; at += 4U) {
        audio_stream_receipt_item item;
        uint32_t index;
        uint32_t object = ld32(at);

        if (object == 0U || (object & 3U) != 0U) {
            snapshot->valid = 0U;
            return;
        }
        if (ld32(object) !=
                GUEST_IMAGE_BASE + ISAAC_VITA_AUDIO_OGG_VTABLE_RVA)
            continue;

        memset(&item, 0, sizeof item);
        item.object = object;
        item.source = ld32(object + ISAAC_VITA_AUDIO_OGG_SOURCE_OFFSET);
        item.stopped = ld8(object + ISAAC_VITA_AUDIO_OGG_STOPPED_OFFSET);
        item.loop = ld8(object + ISAAC_VITA_AUDIO_OGG_LOOP_OFFSET);
        item.eof = ld8(object + ISAAC_VITA_AUDIO_OGG_EOF_OFFSET);
        item.queue_empty = 1U;
        for (index = 0U; index < ISAAC_VITA_AUDIO_OGG_QUEUE_COUNT;
                ++index) {
            uint32_t state = ld32(
                object + ISAAC_VITA_AUDIO_OGG_QUEUE_OFFSET +
                index * ISAAC_VITA_AUDIO_OGG_QUEUE_STRIDE);

            if (state != 0U)
                item.queue_empty = 0U;
            if (state > 3U)
                snapshot->invalid_queue_state = 1U;
            item.queue_states |= (uint16_t)((state & 0x0fU) <<
                                             (index * 4U));
        }

        if (snapshot->ogg_count <
                ISAAC_VITA_AUDIO_STREAM_SOURCE_HEADROOM)
            snapshot->items[snapshot->ogg_count] = item;
        else
            snapshot->overflow = 1U;
        ++snapshot->ogg_count;
        if (item.queue_empty && !item.eof)
            ++snapshot->queue_empty_count;
        if (item.stopped)
            ++snapshot->stopped_count;
        if (item.eof)
            ++snapshot->eof_count;
    }
}

static const audio_stream_receipt_item *audio_stream_receipt_find(
    const audio_stream_receipt_snapshot *snapshot, uint32_t object)
{
    uint32_t count = snapshot->ogg_count;
    uint32_t index;

    if (count > ISAAC_VITA_AUDIO_STREAM_SOURCE_HEADROOM)
        count = ISAAC_VITA_AUDIO_STREAM_SOURCE_HEADROOM;
    for (index = 0U; index < count; ++index) {
        if (snapshot->items[index].object == object)
            return &snapshot->items[index];
    }
    return NULL;
}

static const char *audio_stream_receipt_edge_name(
    enum audio_stream_receipt_edge edge)
{
    switch (edge) {
    case AUDIO_STREAM_EDGE_QUEUE_EMPTY: return "queue-empty";
    case AUDIO_STREAM_EDGE_EMPTY_REPEAT: return "empty-repeat";
    case AUDIO_STREAM_EDGE_REFILL: return "refill";
    case AUDIO_STREAM_EDGE_STOPPED: return "stopped";
    case AUDIO_STREAM_EDGE_REMOVED: return "removed";
    case AUDIO_STREAM_EDGE_INVALID: return "invalid";
    default: return "none";
    }
}

static enum audio_stream_receipt_edge audio_stream_receipt_classify(
    const audio_stream_receipt_snapshot *before,
    const audio_stream_receipt_snapshot *after,
    audio_stream_receipt_item *affected_before,
    audio_stream_receipt_item *affected_after)
{
    enum audio_stream_receipt_edge result = AUDIO_STREAM_EDGE_NONE;
    uint32_t count = before->ogg_count;
    uint32_t index;

    memset(affected_before, 0, sizeof *affected_before);
    memset(affected_after, 0, sizeof *affected_after);
    if (!before->valid || !after->valid || before->overflow ||
            after->overflow || before->invalid_queue_state ||
            after->invalid_queue_state)
        return AUDIO_STREAM_EDGE_INVALID;
    if (count > ISAAC_VITA_AUDIO_STREAM_SOURCE_HEADROOM)
        count = ISAAC_VITA_AUDIO_STREAM_SOURCE_HEADROOM;

    for (index = 0U; index < count; ++index) {
        const audio_stream_receipt_item *old = &before->items[index];
        const audio_stream_receipt_item *current =
            audio_stream_receipt_find(after, old->object);
        enum audio_stream_receipt_edge edge = AUDIO_STREAM_EDGE_NONE;

        if (!current) {
            edge = AUDIO_STREAM_EDGE_REMOVED;
        } else if (!old->queue_empty && current->queue_empty &&
                   !current->eof) {
            edge = AUDIO_STREAM_EDGE_QUEUE_EMPTY;
        } else if (old->queue_empty && !old->eof &&
                   current->queue_empty && !current->eof) {
            edge = AUDIO_STREAM_EDGE_EMPTY_REPEAT;
        } else if (old->queue_empty && !old->eof &&
                   !current->queue_empty) {
            edge = AUDIO_STREAM_EDGE_REFILL;
        } else if (!old->stopped && current->stopped) {
            edge = AUDIO_STREAM_EDGE_STOPPED;
        }

        /* Keep the strongest observation when several OGG actors change in
         * one Manager::Update.  Empty-without-EOF is the only state which can
         * directly explain a live stream running out of queued PCM. */
        if (edge != AUDIO_STREAM_EDGE_NONE &&
                (result == AUDIO_STREAM_EDGE_NONE || edge < result)) {
            result = edge;
            *affected_before = *old;
            if (current)
                *affected_after = *current;
        }
    }
    return result;
}

static void audio_stream_receipt_record(
    const char *route, const char *result, uint64_t begin, uint64_t end,
    const audio_stream_receipt_snapshot *before,
    const audio_stream_receipt_snapshot *after)
{
    audio_stream_receipt_item affected_before;
    audio_stream_receipt_item affected_after;
    enum audio_stream_receipt_edge edge = AUDIO_STREAM_EDGE_NONE;
    uint32_t edge_count = 0U;
    uint32_t elapsed = audio_stream_receipt_elapsed(begin, end);
    uint32_t sequence =
        audio_stream_receipt_increment(&s_audio_stream_receipt_sequence);
    int success = strcmp(result, "return") == 0;
    int emit;

    if (success) {
        audio_stream_receipt_increment(&s_audio_stream_receipt_successes);
        edge = audio_stream_receipt_classify(
            before, after, &affected_before, &affected_after);
    } else {
        audio_stream_receipt_increment(&s_audio_stream_receipt_faults);
        memset(&affected_before, 0, sizeof affected_before);
        memset(&affected_after, 0, sizeof affected_after);
    }
    if (elapsed > s_audio_stream_receipt_max_us)
        s_audio_stream_receipt_max_us = elapsed;
    if (edge != AUDIO_STREAM_EDGE_NONE) {
        edge_count = audio_stream_receipt_increment(
            &s_audio_stream_receipt_edges[edge]);
    }

    emit = !success || audio_stream_receipt_power_of_two(sequence) ||
           (edge != AUDIO_STREAM_EDGE_NONE &&
            audio_stream_receipt_power_of_two(edge_count));
    if (!emit)
        return;

    isaac_vita_log(
        "KAGE VITA AUDIO STREAM RECEIPT: n=%u route=%s result=%s "
        "us=%u max_us=%u ok=%u fault=%u valid=%u/%u ogg=%u/%u "
        "empty=%u/%u stopped=%u/%u eof=%u/%u edge=%s edge_n=%u "
        "object=%08x source=%u states=%04x/%04x",
        (unsigned)sequence, route, result, (unsigned)elapsed,
        (unsigned)s_audio_stream_receipt_max_us,
        (unsigned)s_audio_stream_receipt_successes,
        (unsigned)s_audio_stream_receipt_faults,
        (unsigned)before->valid, (unsigned)after->valid,
        (unsigned)before->ogg_count, (unsigned)after->ogg_count,
        (unsigned)before->queue_empty_count,
        (unsigned)after->queue_empty_count,
        (unsigned)before->stopped_count, (unsigned)after->stopped_count,
        (unsigned)before->eof_count, (unsigned)after->eof_count,
        audio_stream_receipt_edge_name(edge), (unsigned)edge_count,
        (unsigned)affected_before.object,
        (unsigned)(affected_after.source ? affected_after.source :
                                             affected_before.source),
        (unsigned)affected_before.queue_states,
        (unsigned)affected_after.queue_states);
}
#endif

static int s_audio_cooperative_poll_active;
static int s_audio_cooperative_poll_timed;
static int s_audio_cooperative_poll_logged;
static uint64_t s_audio_cooperative_poll_time;

static void audio_cooperative_poll_interval(
    CPU *__restrict c, uint64_t minimum_interval_us, const char *route)
{
    CPU saved;
    uint32_t manager;
    uint32_t preserved_low_water;
    uint64_t now;
#if defined(ISAAC_VITA_AUDIO_STREAM_RECEIPT)
    audio_stream_receipt_snapshot receipt_before;
    audio_stream_receipt_snapshot receipt_after;
    uint64_t receipt_begin;
#else
    (void)route;
#endif

    if (!c || c->fault || c->stop_kind != GUEST_RUN_RETURNED ||
            s_audio_cooperative_poll_active ||
            !isaac_vita_audio_manager_is_active())
        return;

    manager = GUEST_IMAGE_BASE + ISAAC_VITA_AUDIO_MANAGER_RVA;
    if (ld8(manager + ISAAC_VITA_AUDIO_MANAGER_RUNNING_OFFSET) == 0U ||
            ld8(manager + ISAAC_VITA_AUDIO_MANAGER_SUSPENDED_OFFSET) != 0U)
        return;

    now = isaac_vita_get_process_time();
    if (s_audio_cooperative_poll_timed) {
        if (now < s_audio_cooperative_poll_time) {
            /* The platform counter is monotonic.  A backwards sample cannot
             * safely establish elapsed time, so restart the throttle and
             * leave guest state untouched. */
            s_audio_cooperative_poll_time = now;
            return;
        }
        if (now - s_audio_cooperative_poll_time < minimum_interval_us)
            return;
    }
    /* This is an asynchronous safe point from the guest's perspective.  The
     * nested Update may use every architectural register and flag, so retain
     * the entire CPU rather than guessing a calling convention. */
    saved = *c;
    s_audio_cooperative_poll_active = 1;
#if defined(ISAAC_VITA_AUDIO_STREAM_RECEIPT)
    audio_stream_receipt_snapshot_load(manager, &receipt_before);
    receipt_begin = now;
#endif
    gpush(c, ISAAC_VITA_AUDIO_COOPERATIVE_RETURN);
    if (!c->fault) {
        c->ecx = manager;
        guest_call(c,
                   GUEST_IMAGE_BASE + ISAAC_VITA_AUDIO_MANAGER_UPDATE_RVA);
    }
    s_audio_cooperative_poll_active = 0;
    if (c->fault) {
#if defined(ISAAC_VITA_AUDIO_STREAM_RECEIPT)
        memset(&receipt_after, 0, sizeof receipt_after);
        audio_stream_receipt_record(
            route, "guest-fault", receipt_begin,
            isaac_vita_get_process_time(), &receipt_before, &receipt_after);
#endif
        return;
    }
    if (c->esp != saved.esp) {
        guest_fault(c, GUEST_IMAGE_BASE +
                         ISAAC_VITA_AUDIO_MANAGER_UPDATE_RVA,
                    "cooperative audio Update did not restore ESP");
#if defined(ISAAC_VITA_AUDIO_STREAM_RECEIPT)
        memset(&receipt_after, 0, sizeof receipt_after);
        audio_stream_receipt_record(
            route, "esp-fault", receipt_begin,
            isaac_vita_get_process_time(), &receipt_before, &receipt_after);
#endif
        return;
    }
    if (c->jump_sites != saved.jump_sites) {
        guest_fault(c, GUEST_IMAGE_BASE +
                         ISAAC_VITA_AUDIO_MANAGER_UPDATE_RVA,
                    "cooperative audio Update changed jump-site ownership");
#if defined(ISAAC_VITA_AUDIO_STREAM_RECEIPT)
        memset(&receipt_after, 0, sizeof receipt_after);
        audio_stream_receipt_record(
            route, "jump-fault", receipt_begin,
            isaac_vita_get_process_time(), &receipt_before, &receipt_after);
#endif
        return;
    }

    preserved_low_water = c->stack_low_water;
    if (preserved_low_water < saved.stack_floor ||
            preserved_low_water > saved.stack_low_water)
        preserved_low_water = saved.stack_low_water;
    *c = saved;
    c->stack_low_water = preserved_low_water;
    /* Match the removed worker's Update(); Sleep(5) order: the throttle
     * starts after the nested Update has completed, not before it. */
    s_audio_cooperative_poll_time = isaac_vita_get_process_time();
#if defined(ISAAC_VITA_AUDIO_STREAM_RECEIPT)
    audio_stream_receipt_snapshot_load(manager, &receipt_after);
    audio_stream_receipt_record(
        route, "return", receipt_begin, s_audio_cooperative_poll_time,
        &receipt_before, &receipt_after);
#endif
    s_audio_cooperative_poll_timed = 1;
    if (!s_audio_cooperative_poll_logged) {
        s_audio_cooperative_poll_logged = 1;
        isaac_vita_log(
            "KAGE VITA AUDIO COOPERATIVE: active interval_us=5000");
    }
}

void isaac_vita_audio_cooperative_poll(CPU *__restrict c)
{
    audio_cooperative_poll_interval(
        c, ISAAC_VITA_AUDIO_COOPERATIVE_INTERVAL_US, "room");
}

void isaac_vita_audio_cooperative_save_poll(CPU *__restrict c)
{
    audio_cooperative_poll_interval(
        c, ISAAC_VITA_AUDIO_SAVE_COOPERATIVE_INTERVAL_US, "save");
}

#else

void isaac_vita_audio_cooperative_poll(CPU *__restrict c)
{
    (void)c;
}

void isaac_vita_audio_cooperative_save_poll(CPU *__restrict c)
{
    (void)c;
}

#endif
