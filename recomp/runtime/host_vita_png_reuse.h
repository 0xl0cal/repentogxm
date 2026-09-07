#ifndef HOST_VITA_PNG_REUSE_H
#define HOST_VITA_PNG_REUSE_H

#include "host_vita_native_png.h"

/* Decoded rows, NOT ImageManager objects or GPU resources. Payload lives in
 * an independent, fixed-budget optional reserve: never in the decoder scratch.
 * Borrowing that scratch's tail would change initial bytes seen by a later
 * malformed permissive-tinfl fallback. No retained FILE, guest pointer
 * identity, or hash-only hit is involved. The arena owner supplies storage;
 * this module performs no allocations.
 * Owner-thread only, matching the native PNG decoder's singleton contract. */
#define ISAAC_NP_REUSE_SLOTS 32U
#if ISAAC_VITA_NATIVE_PNG_REUSE_LARGE
#define ISAAC_NP_REUSE_INPUT_BYTES (128U * 1024U)
#else
#define ISAAC_NP_REUSE_INPUT_BYTES (64U * 1024U)
#endif
typedef struct {
    /* valid: 0 empty, 1 strict, 2 small history-safe, 3 large history-safe.
     * Class 3 only occupies unused gaps and yields before ANY small store. */
    uint32_t valid, offset, bytes, raw_bytes;
    uint32_t width, height, channels, color_type, rowbytes;
    uint32_t input_bytes, crc_entry, crc_final, gamma;
    uint32_t last_filter, filters[5];
    uint8_t gamma_table[256];
} IsaacNpReuseEntry;

typedef struct IsaacNpReuse {
    uint8_t *arena;
    uint32_t bytes, cursor, raw_bytes, next_slot;
    uint32_t lookups, hits, stores, evictions, skipped, hit_kib;
    IsaacNpReuseEntry entries[ISAAC_NP_REUSE_SLOTS];
} IsaacNpReuse;

void isaac_np_reuse_init(IsaacNpReuse *cache, uint8_t *arena, uint32_t bytes);
int isaac_np_reuse_disjoint(const IsaacNpReuse *cache, const void *ptr, uint32_t bytes);
int isaac_np_reuse_begin(IsaacNpReuse *cache, uint8_t *raw, uint32_t bytes);
int isaac_np_reuse_load(IsaacNpReuse *cache, const isaac_np_params *p,
    const uint8_t *input, uint32_t n, uint32_t crc,
    isaac_np_work *work, isaac_np_result *out);
/* Caller must have independently accepted the COMPLETE single IDAT through
 * strict libdeflate and then completed unfilter/gamma successfully. Do not
 * store unchecked permissive tinfl successes: malformed distance-zero streams
 * can depend on old scratch contents. Optional REUSE_TINFL accepts only a
 * complete successful original decode with explicit history-safe observation;
 * the single wholly-staged IDAT and exact input/gamma equality still apply. */
void isaac_np_reuse_store(IsaacNpReuse *cache, const isaac_np_params *p,
    const uint8_t *input, uint32_t n, const isaac_np_work *work,
    const isaac_np_result *out);

#endif
