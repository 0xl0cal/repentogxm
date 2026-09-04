#ifndef KAGE_VITA_RAW_GXM_PROBE_H
#define KAGE_VITA_RAW_GXM_PROBE_H

#include <stdint.h>

/* Diagnostic-only owner-thread bracket.  The implementation intercepts raw
 * SceGxm imports through GNU ld --wrap and records only the first four game
 * presents after the loading handoff. */
#define KAGE_VITA_RAW_GXM_FRAME_LIMIT 4u
#define KAGE_VITA_RAW_GXM_RUN_CAPACITY 8u

void kage_vita_raw_gxm_probe_begin_frame(uint32_t present_index);
void kage_vita_raw_gxm_probe_end_frame(uint32_t present_index);

#endif
