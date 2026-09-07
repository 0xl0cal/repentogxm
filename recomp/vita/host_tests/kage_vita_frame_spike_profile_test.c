#include "../../runtime/kage_vita_frame_spike_profile.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    KageVitaFrameSpikeWindow s = {0};
    kage_vita_spike_record(&s, 1u, 18000u);
    kage_vita_spike_record(&s, 2u, 9000u);
    kage_vita_spike_record(&s, 3u, 4000u);
    s.current.draws = 33u;
    kage_vita_spike_finish(&s, 11u, 2000000u, 28000u);
    kage_vita_spike_record(&s, 1u, 1000u);
    kage_vita_spike_record(&s, 2u, 34000u);
    kage_vita_spike_record(&s, 3u, 12000u);
    s.current.draws = 44u;
    kage_vita_spike_finish(&s, 12u, 2036000u, 36000u);
    assert(s.samples == 2u);
    assert(s.worst[0].loop == 12u && s.worst[0].phase_us[1] == 1000u);
    assert(s.worst[1].loop == 11u && s.worst[1].draws == 33u);
    assert(s.worst[2].loop == 12u && s.worst[2].phase_us[3] == 12000u);
    assert(s.worst[0].phase_mask == 14u);
    assert(s.current.draws == 0u && s.current.phase_mask == 0u);
    /* Multiple nested presents add; unsupported OTHER/GAP do not leak in. */
    kage_vita_spike_record(&s, 3u, UINT32_MAX);
    kage_vita_spike_record(&s, 3u, 1u);
    kage_vita_spike_record(&s, 7u, 999u);
    assert(s.current.phase_us[3] == UINT32_MAX && s.current.clamped);
    kage_vita_spike_finish(&s, 13u, UINT64_MAX, UINT64_MAX);
    assert(s.worst[0].whole_us == UINT32_MAX);
    assert(s.worst[0].at_ms == UINT32_MAX && s.worst[0].clamped);
    memset(&s, 0, sizeof s);
    kage_vita_spike_finish(&s, 14u, 2200000u, 0u);
    assert(s.samples == 1u && s.worst[2].loop == 14u);
    puts("frame spike profile: PASS");
    return 0;
}
