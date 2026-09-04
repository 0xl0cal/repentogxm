#ifndef ISAAC_KAGE_VITA_FX_ROLLBACK_H
#define ISAAC_KAGE_VITA_FX_ROLLBACK_H

#include <stdint.h>

#include "guest.h"

/*
 * Return 0 when the default-OFF policy is disabled, 1 with an exact rollback
 * plan, and -1 when the enabled failure edge no longer satisfies its pinned
 * container/record invariants.  This helper only observes state: the
 * generated owner performs the destructor call and commits vector.end.
 */
int kage_vita_fx_rollback_prepare(
    const CPU *cpu, uint32_t root_rva, uint32_t *owner_out,
    uint32_t *last_out, uint32_t *end_out);

/* Called only after the generated owner has destructed and popped the record. */
void kage_vita_fx_rollback_note(
    uint32_t root_rva, uint32_t owner, uint32_t last, uint32_t old_end);

#endif
