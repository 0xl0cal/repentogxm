#ifndef ISAAC_NATIVE_THEORA_SNAPSHOT_H
#define ISAAC_NATIVE_THEORA_SNAPSHOT_H
#if defined(ISAAC_VITA_THEORA_CADENCE_PROFILE) && ISAAC_VITA_THEORA_CADENCE_PROFILE
#include <stdint.h>
/* Main-thread, read-only view of the existing lifetime counters. No reset. */
typedef struct VitaTheoraNativeSnapshot {
    uint32_t packets, rgb_decodes, vorbis_syntheses, dup_frames, packet_errors;
    uint64_t packetin_us_sum, rgb_us_sum, vorbis_us_sum;
} VitaTheoraNativeSnapshot;
void isaac_vita_native_theora_snapshot(VitaTheoraNativeSnapshot *out);
#endif
#endif
