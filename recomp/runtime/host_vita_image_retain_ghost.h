#ifndef ISAAC_HOST_VITA_IMAGE_RETAIN_GHOST_H
#define ISAAC_HOST_VITA_IMAGE_RETAIN_GHOST_H

/* Image-retain sizing probe (ISAAC_VITA_IMAGE_RETAIN_GHOST, profiling only;
 * requires ISAAC_VITA_IMAGE_RETAIN with BUDGET_MB=0, so the game frees every
 * sheet exactly as the OFF build does).
 *
 * GHOST LRU.  Every eligible sheet the observer frees is recorded by name
 * hash and texel bytes.  A sheet can only be offered again after a fresh
 * ImageManager::Load decoded and uploaded it again, so a re-offer of a known
 * name is one PNG re-decode that retention could have saved.  Its stack
 * distance -- the bytes of the other sheets freed after its previous free --
 * is the smallest LRU byte budget that would have kept it (Mattson).  The
 * histogram of distances is therefore the hit curve for every budget at once,
 * from one run.  It is conservative: a sheet freed later but reloaded before
 * this one is still counted in the distance (the probe only learns about a
 * reload at the next free), so real budgets needed are <= these.
 *
 * MEMORY.  Per-window minimum of vitaGL free RAM / VRAM / PHYCONT, their sum,
 * and the kernel's free USER_RW, sampled every present and at every offer;
 * plus the sampler's own cost.  The 1 Hz debug-overlay peaks miss transients
 * shorter than a second, which is exactly a room transition.
 *
 * Header-only, pure, no allocator: included by host_vita_image_retain.c and
 * by the host test recomp/test_image_retain_ghost.py. */

#include <stdint.h>
#include <string.h>

#define IRG_SLOTS   1024U
#define IRG_BUCKETS 9U   /* distance <= 4, 8, 16, 24, 32, 48, 64, 96 MiB, > 96 */

static const uint32_t irg_edge_mib[IRG_BUCKETS - 1U] = {
    4U, 8U, 16U, 24U, 32U, 48U, 64U, 96U
};

typedef struct isaac_irg_stats {
    uint32_t offers;        /* eligible sheets freed */
    uint32_t cold;          /* first free of a name (or after it was dropped) */
    uint32_t reloads;       /* re-offers: one re-decode each */
    uint32_t reload_kb;     /* their texel KiB */
    uint32_t dropped;       /* names pushed out of the full ghost table */
    uint32_t n[IRG_BUCKETS];
    uint32_t kb[IRG_BUCKETS];
} isaac_irg_stats;

typedef struct isaac_irg_mem {
    uint32_t n;             /* samples this window */
    uint32_t min_ram, min_vram, min_phy, min_sum, min_user;
    uint32_t us_sum, us_max;
} isaac_irg_mem;

typedef struct isaac_irg {
    uint32_t key[IRG_SLOTS];
    uint32_t bytes[IRG_SLOTS];
    uint32_t seq[IRG_SLOTS];
    uint32_t used;
    uint32_t clock;
    isaac_irg_stats st;
    isaac_irg_mem mem;
} isaac_irg;

static inline uint32_t irg_fnv1a_step(uint32_t h, uint8_t byte)
{
    return (h ^ byte) * 16777619U;
}
#define IRG_FNV1A_INIT 2166136261U

static inline uint32_t irg_bucket(uint64_t distance)
{
    uint32_t b;

    for (b = 0U; b < IRG_BUCKETS - 1U; ++b) {
        if (distance <= ((uint64_t)irg_edge_mib[b] << 20))
            return b;
    }
    return IRG_BUCKETS - 1U;
}

static inline uint32_t irg_kb(uint32_t bytes)
{
    return (uint32_t)(((uint64_t)bytes + 1023U) >> 10);
}

static inline void irg_offer(isaac_irg *g, uint32_t key, uint32_t bytes)
{
    uint32_t i, slot = g->used, oldest = 0U;
    uint64_t distance = 0U;

    ++g->st.offers;
    for (i = 0U; i < g->used; ++i) {
        if (g->key[i] == key) {
            slot = i;
            break;
        }
    }
    if (slot < g->used) {
        for (i = 0U; i < g->used; ++i) {
            if (g->seq[i] > g->seq[slot])
                distance += g->bytes[i];
        }
        i = irg_bucket(distance);
        ++g->st.n[i];
        g->st.kb[i] += irg_kb(bytes);
        ++g->st.reloads;
        g->st.reload_kb += irg_kb(bytes);
    } else {
        ++g->st.cold;
        if (g->used < IRG_SLOTS) {
            slot = g->used++;
        } else {
            for (i = 1U; i < IRG_SLOTS; ++i) {
                if (g->seq[i] < g->seq[oldest])
                    oldest = i;
            }
            slot = oldest;
            ++g->st.dropped;
        }
        g->key[slot] = key;
    }
    g->bytes[slot] = bytes;
    g->seq[slot] = ++g->clock;
}

static inline void irg_mem_note(isaac_irg_mem *m, uint32_t ram, uint32_t vram,
                                uint32_t phy, uint32_t user, uint32_t us)
{
    uint64_t sum = (uint64_t)ram + vram + phy;
    uint32_t sum32 = sum > 0xFFFFFFFFU ? 0xFFFFFFFFU : (uint32_t)sum;

    if (m->n == 0U) {
        m->min_ram = ram;
        m->min_vram = vram;
        m->min_phy = phy;
        m->min_sum = sum32;
        m->min_user = user;
    } else {
        if (ram < m->min_ram) m->min_ram = ram;
        if (vram < m->min_vram) m->min_vram = vram;
        if (phy < m->min_phy) m->min_phy = phy;
        if (sum32 < m->min_sum) m->min_sum = sum32;
        if (user < m->min_user) m->min_user = user;
    }
    ++m->n;
    m->us_sum += us;
    if (us > m->us_max)
        m->us_max = us;
}

#endif /* ISAAC_HOST_VITA_IMAGE_RETAIN_GHOST_H */
