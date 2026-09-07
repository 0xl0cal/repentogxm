#ifndef ISAAC_HOST_VITA_TEXEL_SCRATCH_H
#define ISAAC_HOST_VITA_TEXEL_SCRATCH_H

#include <stddef.h>
#include <stdint.h>

typedef enum isaac_vita_texel_scratch_result {
    ISAAC_VITA_TEXEL_SCRATCH_REJECTED = -1,
    ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED = 0,
    ISAAC_VITA_TEXEL_SCRATCH_HANDLED = 1
} isaac_vita_texel_scratch_result;

typedef struct isaac_vita_texel_scratch_decision {
    void *pointer;
    uint32_t fault_value;
    const char *fault;
} isaac_vita_texel_scratch_decision;

/* The owner is resolved from the complete frozen EBP chain at the imported
 * malloc boundary.  Only the synchronously uploaded ImagePng and
 * ProceduralImageBase owners are accepted.  A clean native reservation failure
 * returns NOT_HANDLED so the existing guest heap and its OOM diagnostic remain
 * the fallback. */
int isaac_vita_texel_scratch_malloc(
    uint32_t owner_return_rva, size_t size,
    uint32_t guest_stack_floor, uint32_t guest_stack_ceiling,
    isaac_vita_texel_scratch_decision *decision);

/* Exact-base free/realloc are handled by the scratch owner.  Foreign pointers
 * remain on the ordinary guest-heap path; interior and stale scratch pointers
 * are rejected before they can reach newlib. */
int isaac_vita_texel_scratch_free(
    void *pointer, isaac_vita_texel_scratch_decision *decision);
int isaac_vita_texel_scratch_realloc(
    void *pointer, size_t size,
    isaac_vita_texel_scratch_decision *decision);

#if defined(ISAAC_VITA_PNG_PREMULTIPLY_NATIVE)
struct IsaacVitaPngPremultiply;
/* Pure in-place transform of an exact live PNG-owned scratch allocation.
 * Try-lock rejection changes no pixels or ownership. Never calls the guest. */
int isaac_vita_texel_scratch_png_premultiply(
    const struct IsaacVitaPngPremultiply *params);
#endif

#if defined(ISAAC_VITA_PNG_TEXEL_INIT_ELISION)
/* Read-only O(1) admission for exactly one live PNG-owned scratch request.
 * Busy, stale, foreign owner/base and size mismatch all return zero. */
int isaac_vita_texel_scratch_png_exact(uint32_t base, uint32_t bytes);
#endif

#if defined(ISAAC_VITA_NATIVE_PNG_ROW_BATCH)
/* Copy only middle rows into the live PNG scratch while holding its owner
 * lock. First/final rows and all padding remain the caller's responsibility. */
int isaac_vita_texel_scratch_png_middle_rows(uint32_t base, uint32_t stride,
    uint32_t rowbytes, uint32_t height, const uint8_t *filtered_rows);
#endif

#ifdef ISAAC_VITA_TEXEL_SCRATCH_ORACLE
#if defined(ISAAC_VITA_PNG_PREMULTIPLY_NATIVE) || \
    defined(ISAAC_VITA_PNG_TEXEL_INIT_ELISION)
int isaac_vita_texel_scratch_oracle_hold_lock(void);
void isaac_vita_texel_scratch_oracle_drop_lock(void);
#endif
typedef struct isaac_vita_texel_scratch_snapshot {
    uintptr_t base;
    int32_t uid;
    uint32_t poisoned;
    uint32_t live;
    size_t live_size;
    size_t high_water;
    uint32_t acquired_count;
    uint32_t freed_count;
    uint32_t busy_fallback_count;
    uint32_t oversize_fallback_count;
} isaac_vita_texel_scratch_snapshot;

int isaac_vita_texel_scratch_oracle_snapshot(
    isaac_vita_texel_scratch_snapshot *snapshot);
int isaac_vita_texel_scratch_oracle_reset(void);
#endif

#endif
