/* Host oracle for the stream refill pacer decision (host_vita_audio_refill.c).
 * It replays the frozen StreamSource::Update query pattern: one
 * AL_BUFFERS_PROCESSED query per queued slot per Update, 4 slots per stream,
 * N streams whose slots come due together. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_vita_audio_refill.h"

static unsigned s_failures;

static void expect(int condition, const char *message)
{
    if (!condition) {
        ++s_failures;
        fprintf(stderr, "FAIL: %s\n", message);
    }
}

static void test_zero_passthrough(void)
{
    isaac_vita_audio_refill_pacer pacer;

    memset(&pacer, 0, sizeof pacer);
    expect(isaac_vita_audio_refill_pace_decide(&pacer, 0, 4, 1000U, 20000U)
               == 0, "zero processed passes through");
    expect(isaac_vita_audio_refill_pace_decide(&pacer, -1, 4, 1000U, 20000U)
               == -1, "negative passes through");
    expect(pacer.queries == 2U && pacer.positive == 0U &&
           pacer.granted == 0U && pacer.armed == 0U,
           "zero answers never arm the gate");
}

static void test_spacing_and_cap(void)
{
    isaac_vita_audio_refill_pacer pacer;
    uint64_t now = 1000000U;

    memset(&pacer, 0, sizeof pacer);
    /* Stream A: 1 of 4 processed, plenty of headroom: granted. */
    expect(isaac_vita_audio_refill_pace_decide(&pacer, 1, 4, now, 20000U)
               == 1, "first positive answer is granted");
    /* Stream B in the same Update: deferred to zero. */
    expect(isaac_vita_audio_refill_pace_decide(&pacer, 1, 4, now + 10U,
                                               20000U) == 0,
           "second stream inside the spacing window is deferred");
    /* Stream A's second slot query (already unqueued): zero stays zero. */
    expect(isaac_vita_audio_refill_pace_decide(&pacer, 0, 3, now + 20U,
                                               20000U) == 0,
           "already-unqueued query is untouched");
    /* Next Update, 16.7 ms later: still inside 20 ms, deferred. */
    expect(isaac_vita_audio_refill_pace_decide(&pacer, 1, 4, now + 16700U,
                                               20000U) == 0,
           "next loop inside the window is still deferred");
    /* Two loops later: granted, and two processed are capped to one. */
    expect(isaac_vita_audio_refill_pace_decide(&pacer, 2, 4, now + 33400U,
                                               20000U) == 1,
           "grant after the window caps two processed to one");
    expect(pacer.granted == 2U && pacer.deferred == 2U &&
           pacer.capped == 1U && pacer.forced == 0U,
           "counters follow the grant/defer/cap sequence");
}

static void test_headroom_valve(void)
{
    isaac_vita_audio_refill_pacer pacer;
    uint64_t now = 5000000U;

    memset(&pacer, 0, sizeof pacer);
    expect(isaac_vita_audio_refill_pace_decide(&pacer, 1, 4, now, 20000U)
               == 1, "arm the gate");
    /* A source down to its playing buffer is always answered truthfully,
     * with the full count, even inside the window. */
    expect(isaac_vita_audio_refill_pace_decide(&pacer, 3, 4, now + 1U,
                                               20000U) == 3,
           "one unprocessed buffer left: full truthful count");
    /* End-of-stream drain: processed == queued must never be hidden. */
    expect(isaac_vita_audio_refill_pace_decide(&pacer, 4, 4, now + 2U,
                                               20000U) == 4,
           "drained source reports every processed buffer");
    /* Two-slot streams (queued == 2) are never paced. */
    expect(isaac_vita_audio_refill_pace_decide(&pacer, 1, 2, now + 3U,
                                               20000U) == 1,
           "two-slot stream is never deferred");
    expect(pacer.forced == 3U && pacer.deferred == 0U,
           "valve answers are counted as forced");
}

static void test_backwards_clock(void)
{
    isaac_vita_audio_refill_pacer pacer;

    memset(&pacer, 0, sizeof pacer);
    expect(isaac_vita_audio_refill_pace_decide(&pacer, 1, 4, 900000U,
                                               20000U) == 1, "arm");
    /* A clock that went backwards must not defer forever. */
    expect(isaac_vita_audio_refill_pace_decide(&pacer, 1, 4, 100000U,
                                               20000U) == 1,
           "backwards clock grants");
}

static void test_seven_layers_within_slot_period(void)
{
    /* Seven synchronized layers, 4 slots of 0.372 s each, 60 Hz loop,
     * 20 ms spacing: every layer must be granted before its own slot period
     * ends without the valve ever firing, and never two in one loop. */
    isaac_vita_audio_refill_pacer pacer;
    int pending[7];
    uint64_t now = 0U;
    unsigned loop;
    unsigned granted = 0U;
    unsigned max_per_loop = 0U;

    memset(&pacer, 0, sizeof pacer);
    for (loop = 0U; loop < 7U; ++loop)
        pending[loop] = 1;
    for (loop = 0U; loop < 60U && granted < 7U; ++loop) {
        unsigned stream;
        unsigned this_loop = 0U;

        now = (uint64_t)loop * 16667U;
        for (stream = 0U; stream < 7U; ++stream) {
            int32_t shown;

            if (!pending[stream])
                continue;
            shown = isaac_vita_audio_refill_pace_decide(
                &pacer, 1, 4, now, 20000U);
            if (shown) {
                pending[stream] = 0;
                ++granted;
                ++this_loop;
            }
        }
        if (this_loop > max_per_loop)
            max_per_loop = this_loop;
    }
    expect(granted == 7U, "all seven layers were granted");
    expect(max_per_loop == 1U, "never more than one decode per loop");
    expect(now < 372000U, "seven layers fit inside one 0.372 s slot period");
    expect(pacer.forced == 0U, "the headroom valve did not have to fire");
}

int main(void)
{
    test_zero_passthrough();
    test_spacing_and_cap();
    test_headroom_valve();
    test_backwards_clock();
    test_seven_layers_within_slot_period();
    if (s_failures) {
        fprintf(stderr, "%u refill pacer failures\n", s_failures);
        return 1;
    }
    puts("vita-audio-refill-oracle: PASS");
    return 0;
}
