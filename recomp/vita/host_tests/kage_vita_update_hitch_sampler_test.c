/* ISAAC_VITA_UPDATE_HITCH_PROBE: the guest sampler's trail, executed.
 *
 * test_kage_vita_update_hitch.py links this with the production
 * kage_vita_guest_sampler.c and its existing oracle
 * (kage_vita_guest_sampler_oracle.c, whose main is renamed on the command
 * line with -Dmain=kvuh_sampler_oracle_main).  The oracle runs its usual
 * scenarios; afterwards the trail must hold exactly the samples the
 * oracle's ph120.hs records counted (the driver sums their total= fields),
 * with the same keys: every pair is printed, and the driver compares the
 * multiset of self keys with the oracle's ph120.hot lists and the caller
 * keys with its ph120.hotc lists.  The oracle's own output must be what it
 * is without the probe. */
#include <stdint.h>
#include <stdio.h>

#include "kage_vita_update_hitch.h"

int kvuh_sampler_oracle_main(void);

/* The define is given to the whole command line, so the oracle's scheduler
 * unit calls the probe; the records are not this leg's subject. */
void kage_vita_update_hitch_begin(uint64_t begin_us) { (void)begin_us; }
void kage_vita_update_hitch_end(uint64_t begin_us, uint64_t end_us,
                                uint32_t frame_before, uint32_t frame_after)
{
    (void)begin_us; (void)end_us; (void)frame_before; (void)frame_after;
}

#undef main
int main(void)
{
    static uint32_t self[KAGE_VITA_UPDATE_HITCH_TRAIL];
    static uint32_t caller[KAGE_VITA_UPDATE_HITCH_TRAIL];
    uint32_t seq, copied, lost = 0u, nonzero = 0u, i;
    int rc;

    if (kage_vita_guest_sampler_trail_seq() != 0u) {
        fprintf(stderr, "trail not empty before the first sample\n");
        return 1;
    }
    rc = kvuh_sampler_oracle_main();
    if (rc != 0)
        return rc;
    seq = kage_vita_guest_sampler_trail_seq();
    copied = kage_vita_guest_sampler_trail_copy(0u, seq, self, caller, &lost);
    /* Zero is never a function key (kage_vita_guest_sampler.c, KVGS_KEY). */
    for (i = 0u; i < copied; ++i)
        nonzero += self[i] != 0u && caller[i] != 0u;
    printf("trail: seq=%u copied=%u lost=%u nonzero=%u\n",
           seq, copied, lost, nonzero);
    for (i = 0u; i < copied; ++i)
        printf("trail-pair: %x %x\n", (unsigned)self[i], (unsigned)caller[i]);
    return 0;
}
