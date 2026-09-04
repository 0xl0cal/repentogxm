#ifndef ISAAC_HOST_VITA_AUDIO_REFILL_H
#define ISAAC_HOST_VITA_AUDIO_REFILL_H

#include <stdint.h>

/* Frozen KAGE::Sound::StreamSource facts (8,650,240-byte Repentance PE).
 *
 * StreamSource::Update (0x005bf100) walks the four 12-byte queue slots at
 * this+0x58.  For a slot in state 3 (queued) it asks
 * alGetSourcei(AL_BUFFERS_PROCESSED), unqueues that many names, then decodes
 * AT MOST ONE free slot per Update through the virtual Decode
 * (StreamSourceOgg::Decode, 0x005a20e0), which fills 0x10000 bytes of PCM by
 * running the translated stb_vorbis decoder
 * (stb_vorbis_get_samples_short_interleaved, 0x005bdf80).  The decoded slot is
 * then handed to 0x005be080: alBufferData (returns to 0x005be117) followed by
 * alSourceQueueBuffers (returns to 0x005be126) and alSourcePlay when the
 * source is not playing.
 *
 * Bundle29 phase A measured this on the frame thread: with audio ON the
 * service phase p50 stays 44 us but its p95 is 77-155 ms in every window
 * (ph120.t svc=44/77525/183644), against 10/12/13 us with audio OFF.  A 64 KiB
 * slot holds 0.372 s of 44.1 kHz stereo, so every layered music stream needs
 * one translated decode per 0.372 s and, because the layers start together,
 * they all come due in the same Update.  The burst, not the mean, is what
 * halves the menu present rate and drops the 30 Hz simulation. */
#define ISAAC_VITA_AUDIO_STREAM_SLOT_BYTES        0x00010000U
#define ISAAC_VITA_AUDIO_STREAM_SLOT_COUNT        4U
#define ISAAC_VITA_AUDIO_STREAM_UPDATE_RVA        0x005bf100U
/* alGetSourcei(AL_BUFFERS_PROCESSED) return inside StreamSource::Update.  The
 * pacer applies only to this caller: the Theoraplayer audio interface
 * (0x005adc70) also asks AL_BUFFERS_PROCESSED but derives its video clock
 * from the unqueued byte total plus AL_SEC_OFFSET, so it must always see the
 * true count. */
#define ISAAC_VITA_AUDIO_STREAM_PROCESSED_RETURN  0x005bf136U
#define ISAAC_VITA_AUDIO_STREAM_DECODE_RVA        0x005a20e0U
#define ISAAC_VITA_AUDIO_STREAM_QUEUE_DATA_RVA    0x005be080U
#define ISAAC_VITA_AUDIO_STREAM_BUFFER_DATA_RETURN 0x005be117U
#define ISAAC_VITA_AUDIO_STREAM_QUEUE_RETURN      0x005be126U
#define ISAAC_VITA_AUDIO_STB_GET_SAMPLES_RVA      0x005bdf80U

/* Generated call sites push the BARE RVA as the return marker
 * (guest_0177.c: gpush_generated(c, 0x5bf136U), 0x5be117U, 0x5be126U); only
 * immediate data operands carry GUEST_IMAGE_BASE.  The first pacer build
 * compared against GUEST_IMAGE_BASE + rva and was therefore inert on device.
 * Accept both forms, like host_vita_user32.c, so neither convention can turn
 * the site match into a silent no-op. */
#if defined(GUEST_IMAGE_BASE)
#define ISAAC_VITA_AUDIO_REFILL_IS_SITE(return_word, rva)     ((return_word) == (uint32_t)(rva) ||      (return_word) == (uint32_t)GUEST_IMAGE_BASE + (uint32_t)(rva))
#else
#define ISAAC_VITA_AUDIO_REFILL_IS_SITE(return_word, rva)     ((return_word) == (uint32_t)(rva))
#endif

#define ISAAC_VITA_AUDIO_AL_BUFFERS_PROCESSED     0x1016
#define ISAAC_VITA_AUDIO_AL_BUFFERS_QUEUED        0x1015
#define ISAAC_VITA_AUDIO_AL_SOURCE_STATE          0x1010
#define ISAAC_VITA_AUDIO_AL_PLAYING               0x1012

/* Default spacing between two granted stream refills.  One translated 64 KiB
 * decode is on the order of 20 ms, so one grant per 20 ms keeps at most one
 * decode inside any 60 Hz loop iteration and lets 14 concurrent streams (two
 * seven-layer music sets during a crossfade) all refill inside one 372 ms
 * slot period.  Overridable from CMake. */
#ifndef ISAAC_VITA_AUDIO_REFILL_SPACING_US
#define ISAAC_VITA_AUDIO_REFILL_SPACING_US 20000U
#endif

/* Refills are granted truthfully whenever a stream has this many or fewer
 * unprocessed buffers left (the one playing plus at most one more), so a
 * deferral can never starve a source: at 4 slots that leaves >= 0.37 s. */
#define ISAAC_VITA_AUDIO_REFILL_MIN_HEADROOM 1

/* Counter semantics: StreamSource::Update asks AL_BUFFERS_PROCESSED once per
 * queued slot (up to 4 per stream per Update).  A granted stream answers 1 on
 * its first query and the real 0 on the rest, but a deferred stream is shown
 * 0 on every query of that Update, so "deferred" counts queries, not
 * Updates: expect defer to run about 4x the number of deferred Updates and
 * do not read defer >> grant as starvation.  Starvation shows up as forced
 * growing towards granted. */
typedef struct isaac_vita_audio_refill_pacer {
    uint64_t last_grant_us;
    uint32_t queries;
    uint32_t positive;
    uint32_t granted;
    uint32_t deferred;
    uint32_t forced;
    uint32_t capped;
    uint8_t armed;
} isaac_vita_audio_refill_pacer;

/* Pure decision: the value the guest is shown for AL_BUFFERS_PROCESSED.  The
 * returned count is never above the real one, so the guest's later
 * alSourceUnqueueBuffers of that count cannot fail, and every deferred buffer
 * stays processed inside OpenAL until a later query reports it. */
int32_t isaac_vita_audio_refill_pace_decide(
    isaac_vita_audio_refill_pacer *pacer, int32_t processed, int32_t queued,
    uint64_t now_us, uint64_t spacing_us);

#if defined(ISAAC_VITA_AUDIO_REFILL_HOOKS)
/* Called by the OpenAL import adapters on the translated owner thread.
 * Nothing here touches guest memory or OpenAL; the adapters pass the
 * already-read values. */
void isaac_vita_audio_refill_manager_ready(void);
/* Returns the value to store for AL_BUFFERS_PROCESSED. */
int32_t isaac_vita_audio_refill_filter_processed(
    uint32_t source, int32_t processed, int32_t queued);
void isaac_vita_audio_refill_note_unqueue(uint32_t source);
void isaac_vita_audio_refill_note_buffer_data(
    uint32_t return_address, uint32_t format, uint32_t bytes,
    uint32_t frequency);
void isaac_vita_audio_refill_note_queue(
    uint32_t source, uint32_t return_address, int32_t source_state);
void isaac_vita_audio_refill_note_play(void);

#if defined(ISAAC_VITA_AUDIO_REFILL_REPLAYS)
/* Replay counter (defined together with ISAAC_VITA_NATIVE_VORBIS_ASYNC so
 * the option-OFF objects stay byte-identical).  StreamSource::QueueData
 * (0x005be080) ends with `call [vtable+0x38]` (IsPlaying) and, when that is
 * false, alSourcePlay(this+0x30) returning to 0x005be144.  A fresh stream
 * arrives there in AL_INITIAL; a stream whose source ran dry arrives in
 * AL_STOPPED with the buffers it just queued, and the play restarts it: an
 * audible gap.  A stop the guest itself issued (alSourceStop, music change)
 * also leaves AL_STOPPED, so it clears the source's replay eligibility until
 * its next play.  Source names beyond the 128-entry per-source table (the
 * manager hands out names < 128) are counted as plays only.  Gate:
 * replays=0. */
#define ISAAC_VITA_AUDIO_STREAM_PLAY_RETURN       0x005be144U
#define ISAAC_VITA_AUDIO_AL_INITIAL               0x1011
#define ISAAC_VITA_AUDIO_AL_STOPPED               0x1014
/* Replaces note_play: every alSourcePlay counts as a play; a play returning
 * to the QueueData site with the source AL_STOPPED, buffers queued and no
 * guest alSourceStop since its previous play counts as a replay. */
void isaac_vita_audio_refill_note_play_state(
    uint32_t source, uint32_t return_address, int32_t source_state,
    int32_t queued);
void isaac_vita_audio_refill_note_stop(uint32_t source);
#endif
#endif

#endif
