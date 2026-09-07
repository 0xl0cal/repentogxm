#ifndef ISAAC_LASER_HALO_PROFILE_H
#define ISAAC_LASER_HALO_PROFILE_H

#include <stdint.h>

/* Private, counter-only ABI. Never extend vitaGL.h/GpuDrawStats: their
 * snapshots also occur inside individual laser calls. These counters belong
 * to the GL owner and are read only between completed outer-loop windows.
 * uint32 arithmetic wraps, exactly like the existing GPU draw census. */
#define ISAAC_LASER_HALO_STATS_ABI 2u
#define ISAAC_LASER_HALO_STATS_WORDS 14u
#define ISAAC_LASER_HALO_STATS_LEGACY_WORDS 11u
enum {
    ISAAC_HALO_META, ISAAC_HALO_LIMITS, ISAAC_HALO_STATE, ISAAC_HALO_PLAIN,
    ISAAC_HALO_UV, ISAAC_HALO_OUTPUT, ISAAC_HALO_LIGHT, ISAAC_HALO_CAP,
    ISAAC_HALO_OUTCOMES,
    /* Local-only reason: contributes to LIMITS and its too_big subset. */
    ISAAC_HALO_TOO_BIG
};
typedef struct vglIsaacLaserHaloStats {
    uint32_t abi_version;
    uint32_t attempts;
    uint32_t outcome[ISAAC_HALO_OUTCOMES];
    uint32_t too_big;
    uint32_t white_enabled; /* census only: HALO_PROFILE && STAGING_PLAIN_FUSION */
    uint32_t ordinary_plain_requests;
    uint32_t ordinary_white_requests; /* subset; not shader/SDK/GPU success */
} vglIsaacLaserHaloStats;
_Static_assert(sizeof(vglIsaacLaserHaloStats) ==
               ISAAC_LASER_HALO_STATS_WORDS * sizeof(uint32_t),
               "private halo statistics ABI drifted");

/* Strong when requested: a missing native endpoint fails linking.
 * An old getter rejects a new 14-word request, invalidating the snapshot.
 * words=11 writes exactly the legacy 44-byte prefix with abi_version=1.
 * words=14 returns ABI2. Other counts/null return zero without writing.
 * Never consumes; old records/disabled census do not measure white zeros. */
uint32_t vglGetIsaacLaserHaloStats(vglIsaacLaserHaloStats *out, uint32_t words);

#endif
