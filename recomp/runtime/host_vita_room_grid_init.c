#include "host_vita_room_grid_init.h"
#include "host_vita_heap.h"
#include <errno.h>
#include <stdatomic.h>

#if !defined(ISAAC_VITA_ROOM_GRID_INIT_NATIVE) || !ISAAC_VITA_ROOM_GRID_INIT_NATIVE
#error Compile the room grid initializer only when explicitly enabled
#endif
#if !defined(ISAAC_VITA_HEAP_RANGE_LEASE)
#error Room grid initialization requires exact requested-size heap leases
#endif

#if defined(__arm__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define ROOM_GRID_NEON 1
#else
#define ROOM_GRID_NEON 0
#endif

#ifndef ISAAC_VITA_ROOM_GRID_INIT_BUILD_ID
#define ISAAC_VITA_ROOM_GRID_INIT_BUILD_ID "room-grid:unstamped"
#endif
void isaac_vita_log(const char *format, ...);
static atomic_flag s_reported = ATOMIC_FLAG_INIT;

/* Constants copied from the pinned allocation/alignment/initializer span
 * 002c42eb..002c436c.  These are NOT zero-filled 32-byte records: bytes 25..27
 * are padding left untouched by the original stores. */
enum {
    GRID_ROWS = 448U, GRID_STRIDE = 32U, GRID_BYTES = 0x3800U,
    GRID_ALLOCATION_BYTES = 0x3823U, GRID_SEED = 0x16a9de81U
};

int isaac_vita_room_grid_init_try(CPU *__restrict c, uint32_t raw_allocation)
{
    const int saved_errno = errno;
    uint32_t grid, token, row;
    int result = 0;
    if (!c || c->fault || c->ecx != GRID_ROWS || !raw_allocation ||
        raw_allocation > UINT32_MAX - GRID_ALLOCATION_BYTES)
        goto done;
    grid = c->eax;
    if (grid != ((raw_allocation + 0x23U) & ~31U))
        goto done;
    token = isaac_vita_guest_heap_lease_exact_range(
        (const void *)(uintptr_t)raw_allocation,
        (const void *)(uintptr_t)grid, GRID_BYTES);
    if (!token)
        goto done;

#if ROOM_GRID_NEON
    const uint32x4_t zero = vdupq_n_u32(0U);
    const uint32x2_t seed_zero = vset_lane_u32(GRID_SEED, vdup_n_u32(0U), 0);
#endif
    for (row = 0U; row < GRID_ROWS - 1U; ++row) {
        uint8_t *p = (uint8_t *)(uintptr_t)(grid + row * GRID_STRIDE);
#if ROOM_GRID_NEON
        /* Disjoint, aligned ordinary heap fields: combine adjacent stores,
         * never touch padding and retain ascending row/field order. */
        vst1q_u32((uint32_t *)(void *)p, zero);
        vst1_u32((uint32_t *)(void *)(p + 16U), seed_zero);
#else
        const uint32_t zero_word = 0U, seed = GRID_SEED;
        memcpy(p, &zero_word, 4U);
        memcpy(p + 4U, &zero_word, 4U);
        memcpy(p + 8U, &zero_word, 4U);
        memcpy(p + 12U, &zero_word, 4U);
        memcpy(p + 16U, &seed, 4U);
        memcpy(p + 20U, &zero_word, 4U);
#endif
        p[24] = 0U;
        const uint32_t last_seed = GRID_SEED;
        memcpy(p + 28U, &last_seed, 4U);
    }
    c->eax = grid + (GRID_ROWS - 1U) * GRID_STRIDE;
    c->ecx = 1U;
    if (!isaac_vita_guest_heap_lease_release(token)) {
        errno = saved_errno;
        guest_fault(c, grid, "Room grid initialization lease release failed");
        result = -1; /* Never replay the guest loop with a live lease. */
        goto done;
    }
    result = 1;
    if (!atomic_flag_test_and_set_explicit(&s_reported, memory_order_relaxed))
        isaac_vita_log("[kage-vita] Room grid init engaged bid=%.32s rows=447 stride=32 final=guest neon=%u\n",
            ISAAC_VITA_ROOM_GRID_INIT_BUILD_ID, (unsigned)ROOM_GRID_NEON);
done:
    errno = saved_errno;
    return result;
}
