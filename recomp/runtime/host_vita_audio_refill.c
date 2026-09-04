/* Stream refill pacing and receipts for the frozen OpenAL boundary.
 *
 * Measured rationale (Bundle29 phase A vs B, ph120.t): the whole audio cost
 * on the frame thread is bursty.  svc p50 rises only 10 -> 44 us with audio
 * ON, but svc p95/max is 77-155 / 145-183 ms because every layered music
 * stream decodes one 64 KiB slot (0.372 s of PCM) through translated
 * stb_vorbis in the same StreamSource::Update, as soon as OpenAL reports the
 * slot processed.  The decode itself is guest code and cannot move off the
 * owner thread without a second guest CPU.  What the host does own is the
 * answer to AL_BUFFERS_PROCESSED, and StreamSource::Update decodes at most one
 * slot per stream per Update.  Showing the guest at most one processed buffer
 * per ISAAC_VITA_AUDIO_REFILL_SPACING_US therefore spreads N layer decodes
 * over N spaced Updates instead of one N-times-longer stall.  A count that is
 * lower than the real one is a state OpenAL itself could have reported a
 * moment earlier: processed buffers stay processed until unqueued, so no
 * alSourceUnqueueBuffers of a shown count can fail.  The headroom valve
 * answers truthfully whenever a source is down to its last unprocessed
 * buffer, so deferral can never starve a stream (>= 0.37 s remains).
 *
 * This file never reads guest memory and issues no OpenAL call; the import
 * adapters in host_vita_audio.c pass the values they already read. */
#include <stdint.h>
#include <string.h>

#include "host_vita_audio_refill.h"

int32_t isaac_vita_audio_refill_pace_decide(
    isaac_vita_audio_refill_pacer *pacer, int32_t processed, int32_t queued,
    uint64_t now_us, uint64_t spacing_us)
{
    int32_t headroom;

    if (pacer->queries != UINT32_MAX)
        ++pacer->queries;
    if (processed <= 0)
        return processed;
    if (pacer->positive != UINT32_MAX)
        ++pacer->positive;

    headroom = queued - processed;
    if (headroom <= ISAAC_VITA_AUDIO_REFILL_MIN_HEADROOM) {
        /* Down to the playing buffer: the guest must refill now.  This also
         * covers end-of-stream drains where processed == queued. */
        if (pacer->forced != UINT32_MAX)
            ++pacer->forced;
        pacer->last_grant_us = now_us;
        pacer->armed = 1U;
        return processed;
    }
    if (pacer->armed && now_us >= pacer->last_grant_us &&
            now_us - pacer->last_grant_us < spacing_us) {
        if (pacer->deferred != UINT32_MAX)
            ++pacer->deferred;
        return 0;
    }
    /* A backwards clock sample simply grants; it cannot defer forever. */
    pacer->last_grant_us = now_us;
    pacer->armed = 1U;
    if (pacer->granted != UINT32_MAX)
        ++pacer->granted;
    if (processed > 1) {
        /* One decode per grant: the remaining processed names are reported by
         * a later, spaced query.  The guest's free-slot flags persist across
         * Updates, so the partial unqueue is bookkept exactly. */
        if (pacer->capped != UINT32_MAX)
            ++pacer->capped;
        return 1;
    }
    return 1;
}

#if defined(ISAAC_VITA_AUDIO_REFILL_HOOKS)

#include "vita_host_services.h"

void isaac_vita_log(const char *format, ...);

#if defined(ISAAC_VITA_AUDIO_WORKER) && defined(__vita__)
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

/* The isage OpenAL Soft 1.19.1 Vita backend reads this weak symbol when it
 * creates "OpenAL Vita playback thread" (Alc/backends/vita.c,
 * ALCvitaPlayback_start) and otherwise starts the mixer with affinity
 * DEFAULT, i.e. free to preempt the translated owner thread on its own core
 * at a higher priority every UpdateSize period.  Pin the mixer to the third
 * user core; the owner thread is moved to the other two below. */
unsigned int _oal_thread_affinity = SCE_KERNEL_CPU_MASK_USER_2;
#define AUDIO_REFILL_OWNER_MASK \
    (SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_1)
#endif

#define AUDIO_REFILL_SOURCE_TABLE 128U
#define AUDIO_REFILL_LIVE_WINDOW_US UINT64_C(1500000)
/* An unqueue that is not followed by a decode in the same Update (end of a
 * non-looping track) must not be attributed to the next stream's refill. */
#define AUDIO_REFILL_PENDING_LIMIT_US UINT64_C(1000000)

typedef struct audio_refill_state {
    isaac_vita_audio_refill_pacer pacer;
    /* The refill sequence for one stream is unqueue -> Decode -> alBufferData
     * -> alSourceQueueBuffers inside one Update, so one pending record
     * captures the translated decode time exactly. */
    uint64_t pending_unqueue_us;
    uint32_t pending_source;
    uint64_t last_refill_us[AUDIO_REFILL_SOURCE_TABLE];
    uint32_t unqueues;
    uint32_t refills;
    uint32_t refills_timed;
    uint32_t last_decode_us;
    uint32_t max_decode_us;
    uint64_t sum_decode_us;
    uint32_t last_bytes;
    uint32_t last_frequency;
    uint32_t last_format;
    uint32_t last_source;
    uint32_t static_loads;
    uint32_t static_bytes_max;
    uint32_t queued;
    uint32_t queued_playing;
    uint32_t plays;
    uint8_t ready_logged;
#if defined(ISAAC_VITA_AUDIO_REFILL_REPLAYS)
    uint32_t site_plays;        /* alSourcePlay returning to QueueData */
    uint32_t replays;           /* ... with the source AL_STOPPED */
    uint32_t partial_refills;   /* stream alBufferData below the slot size */
    uint32_t last_replay_source;
    /* the guest stopped this source since its last play: the next play
     * from QueueData is a restart, not a replay */
    uint8_t stopped_since_play[AUDIO_REFILL_SOURCE_TABLE];
#endif
} audio_refill_state;

static audio_refill_state s_refill;

static uint32_t audio_refill_clamp(uint64_t value)
{
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

static int audio_refill_power_of_two(uint32_t value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

static uint32_t audio_refill_live_streams(uint64_t now)
{
    uint32_t index;
    uint32_t live = 0U;

    for (index = 0U; index < AUDIO_REFILL_SOURCE_TABLE; ++index) {
        uint64_t at = s_refill.last_refill_us[index];

        if (at && now >= at && now - at <= AUDIO_REFILL_LIVE_WINDOW_US)
            ++live;
    }
    return live;
}

void isaac_vita_audio_refill_manager_ready(void)
{
#if defined(ISAAC_VITA_AUDIO_WORKER) && defined(__vita__)
    SceUID self;
    int before;
    int result;
    int after;
#endif

    if (s_refill.ready_logged)
        return;
    s_refill.ready_logged = 1U;
#if defined(ISAAC_VITA_AUDIO_WORKER) && defined(__vita__)
    /* Sound::Manager::Initialize runs on the translated owner thread, so
     * "self" is the frame thread.  Keep it off the mixer's core. */
    self = sceKernelGetThreadId();
    before = sceKernelGetThreadCpuAffinityMask(self);
    result = sceKernelChangeThreadCpuAffinityMask(
        self, AUDIO_REFILL_OWNER_MASK);
    after = sceKernelGetThreadCpuAffinityMask(self);
#endif
#if defined(ISAAC_VITA_AUDIO_WORKER)
    isaac_vita_log(
        "KAGE VITA AUDIO WORKER: pacing=on spacing_us=%u min_headroom=%d "
        "slot_bytes=%u mixer_affinity=0x%08x owner_before=0x%08x "
        "owner_after=0x%08x owner_rc=0x%08x",
        (unsigned)ISAAC_VITA_AUDIO_REFILL_SPACING_US,
        (int)ISAAC_VITA_AUDIO_REFILL_MIN_HEADROOM,
        (unsigned)ISAAC_VITA_AUDIO_STREAM_SLOT_BYTES,
#if defined(__vita__)
        (unsigned)_oal_thread_affinity, (unsigned)before, (unsigned)after,
        (unsigned)result);
#else
        0U, 0U, 0U, 0U);
#endif
#else
    isaac_vita_log(
        "KAGE VITA AUDIO WORKER: pacing=off receipt=on slot_bytes=%u",
        (unsigned)ISAAC_VITA_AUDIO_STREAM_SLOT_BYTES);
#endif
}

int32_t isaac_vita_audio_refill_filter_processed(
    uint32_t source, int32_t processed, int32_t queued)
{
    (void)source;
#if defined(ISAAC_VITA_AUDIO_WORKER)
    return isaac_vita_audio_refill_pace_decide(
        &s_refill.pacer, processed, queued, isaac_vita_get_process_time(),
        ISAAC_VITA_AUDIO_REFILL_SPACING_US);
#else
    /* Receipt-only build: count what an unpaced guest sees. */
    if (s_refill.pacer.queries != UINT32_MAX)
        ++s_refill.pacer.queries;
    if (processed > 0 && s_refill.pacer.positive != UINT32_MAX)
        ++s_refill.pacer.positive;
    (void)queued;
    return processed;
#endif
}

void isaac_vita_audio_refill_note_unqueue(uint32_t source)
{
    s_refill.pending_source = source;
    s_refill.pending_unqueue_us = isaac_vita_get_process_time();
    if (s_refill.unqueues != UINT32_MAX)
        ++s_refill.unqueues;
    /* Only streams (and the Theora audio interface) unqueue.  Every stream
     * unqueue is followed by alBufferData from 0x5be117 in the same Update,
     * so unqueues piling up with refills still at zero means the return-site
     * match is inert (the first pacer build failed exactly this way, but
     * silently).  Bounded: once per power of two from 16. */
    if (s_refill.refills == 0U && s_refill.unqueues >= 16U &&
            audio_refill_power_of_two(s_refill.unqueues))
        isaac_vita_log(
            "KAGE VITA AUDIO REFILL WARN: unqueues=%u static=%u but no "
            "alBufferData returned to %08x yet (site match inert, or "
            "Theora-only so far)",
            (unsigned)s_refill.unqueues, (unsigned)s_refill.static_loads,
            (unsigned)ISAAC_VITA_AUDIO_STREAM_BUFFER_DATA_RETURN);
}

void isaac_vita_audio_refill_note_buffer_data(
    uint32_t return_address, uint32_t format, uint32_t bytes,
    uint32_t frequency)
{
    uint64_t now = isaac_vita_get_process_time();
    uint32_t count;

    if (!ISAAC_VITA_AUDIO_REFILL_IS_SITE(
            return_address, ISAAC_VITA_AUDIO_STREAM_BUFFER_DATA_RETURN)) {
        /* Whole-sample uploads (sound effects, including the mid-play
         * "Sound N was not preloaded" loads).  Counted only. */
        if (s_refill.static_loads != UINT32_MAX)
            ++s_refill.static_loads;
        if (bytes > s_refill.static_bytes_max)
            s_refill.static_bytes_max = bytes;
        return;
    }

    count = s_refill.refills == UINT32_MAX ? UINT32_MAX : ++s_refill.refills;
    s_refill.last_bytes = bytes;
    s_refill.last_frequency = frequency;
    s_refill.last_format = format;
#if defined(ISAAC_VITA_AUDIO_REFILL_REPLAYS)
    /* ASYNC partial slots: any positive byte count is a refill */
    if (bytes < ISAAC_VITA_AUDIO_STREAM_SLOT_BYTES &&
            s_refill.partial_refills != UINT32_MAX)
        ++s_refill.partial_refills;
#endif
    if (s_refill.pending_unqueue_us && now >= s_refill.pending_unqueue_us &&
            now - s_refill.pending_unqueue_us <=
                AUDIO_REFILL_PENDING_LIMIT_US) {
        uint32_t decode_us = audio_refill_clamp(
            now - s_refill.pending_unqueue_us);

        s_refill.last_decode_us = decode_us;
        s_refill.sum_decode_us += decode_us;
        if (decode_us > s_refill.max_decode_us)
            s_refill.max_decode_us = decode_us;
        if (s_refill.refills_timed != UINT32_MAX)
            ++s_refill.refills_timed;
        s_refill.last_source = s_refill.pending_source;
    } else {
        s_refill.last_decode_us = 0U;
    }
    s_refill.pending_unqueue_us = 0U;

    if (audio_refill_power_of_two(count) || (count & 0xffU) == 0U) {
        uint32_t timed = s_refill.refills_timed ? s_refill.refills_timed : 1U;

        isaac_vita_log(
            "KAGE VITA AUDIO REFILL: n=%u timed=%u src=%u bytes=%u freq=%u "
            "fmt=0x%04x decode_us=%u mean_us=%u max_us=%u sum_ms=%u "
            "streams=%u queued=%u playing=%u plays=%u unq=%u static=%u/%u "
            "q=%u pos=%u grant=%u defer=%u force=%u cap=%u"
#if defined(ISAAC_VITA_AUDIO_REFILL_REPLAYS)
            " replays=%u site_plays=%u partial=%u"
#endif
            ,
            (unsigned)count, (unsigned)s_refill.refills_timed,
            (unsigned)s_refill.last_source, (unsigned)bytes,
            (unsigned)frequency, (unsigned)format,
            (unsigned)s_refill.last_decode_us,
            (unsigned)audio_refill_clamp(s_refill.sum_decode_us / timed),
            (unsigned)s_refill.max_decode_us,
            (unsigned)audio_refill_clamp(s_refill.sum_decode_us / 1000U),
            (unsigned)audio_refill_live_streams(now),
            (unsigned)s_refill.queued, (unsigned)s_refill.queued_playing,
            (unsigned)s_refill.plays, (unsigned)s_refill.unqueues,
            (unsigned)s_refill.static_loads,
            (unsigned)s_refill.static_bytes_max,
            (unsigned)s_refill.pacer.queries,
            (unsigned)s_refill.pacer.positive,
            (unsigned)s_refill.pacer.granted,
            (unsigned)s_refill.pacer.deferred,
            (unsigned)s_refill.pacer.forced,
            (unsigned)s_refill.pacer.capped
#if defined(ISAAC_VITA_AUDIO_REFILL_REPLAYS)
            , (unsigned)s_refill.replays, (unsigned)s_refill.site_plays,
            (unsigned)s_refill.partial_refills
#endif
            );
    }
}

void isaac_vita_audio_refill_note_queue(
    uint32_t source, uint32_t return_address, int32_t source_state)
{
    if (!ISAAC_VITA_AUDIO_REFILL_IS_SITE(
            return_address, ISAAC_VITA_AUDIO_STREAM_QUEUE_RETURN))
        return;
    if (s_refill.queued != UINT32_MAX)
        ++s_refill.queued;
    if (source_state == ISAAC_VITA_AUDIO_AL_PLAYING &&
            s_refill.queued_playing != UINT32_MAX)
        ++s_refill.queued_playing;
    if (source < AUDIO_REFILL_SOURCE_TABLE)
        s_refill.last_refill_us[source] = isaac_vita_get_process_time();
}

void isaac_vita_audio_refill_note_play(void)
{
    if (s_refill.plays != UINT32_MAX)
        ++s_refill.plays;
}

#if defined(ISAAC_VITA_AUDIO_REFILL_REPLAYS)
void isaac_vita_audio_refill_note_stop(uint32_t source)
{
    if (source < AUDIO_REFILL_SOURCE_TABLE)
        s_refill.stopped_since_play[source] = 1U;
}

void isaac_vita_audio_refill_note_play_state(
    uint32_t source, uint32_t return_address, int32_t source_state,
    int32_t queued)
{
    /* a source name beyond the table cannot be tracked for guest stops, so
     * it is never claimed as a replay (the manager hands out names < 128) */
    int eligible = source < AUDIO_REFILL_SOURCE_TABLE &&
                   !s_refill.stopped_since_play[source];

    isaac_vita_audio_refill_note_play();
    if (source < AUDIO_REFILL_SOURCE_TABLE)
        s_refill.stopped_since_play[source] = 0U;
    if (!ISAAC_VITA_AUDIO_REFILL_IS_SITE(
            return_address, ISAAC_VITA_AUDIO_STREAM_PLAY_RETURN))
        return;
    if (s_refill.site_plays != UINT32_MAX)
        ++s_refill.site_plays;
    if (source_state != ISAAC_VITA_AUDIO_AL_STOPPED || queued <= 0 ||
            !eligible)
        return;
    if (s_refill.replays != UINT32_MAX)
        ++s_refill.replays;
    s_refill.last_replay_source = source;
    /* every replay is an audible gap: log the first 16, then powers of two */
    if (s_refill.replays <= 16U || audio_refill_power_of_two(s_refill.replays))
        isaac_vita_log(
            "KAGE VITA AUDIO REPLAY: n=%u src=%u queued=%d plays=%u "
            "site_plays=%u refills=%u last_decode_us=%u (source ran dry; "
            "QueueData restarted it)",
            (unsigned)s_refill.replays, (unsigned)source, (int)queued,
            (unsigned)s_refill.plays, (unsigned)s_refill.site_plays,
            (unsigned)s_refill.refills, (unsigned)s_refill.last_decode_us);
}
#endif

#endif
