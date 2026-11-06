/* ISAAC_VITA_HEAP_FREE_PREFILTER: one range test in front of the three
 * fixed-block free routes of the imported guest free (OGG emergency slot,
 * ANM2 scratch, texel scratch).
 *
 * Today every guest free calls isaac_vita_ogg_emergency_free,
 * isaac_vita_anm2_scratch_free and isaac_vita_texel_scratch_free in turn.
 * Each one clears its decision, acquire-loads its published memblock base and
 * returns NOT_HANDLED, with no other side effect, for every pointer outside
 * its own block (host_vita_ogg_emergency.c isaac_vita_ogg_emergency_free,
 * host_vita_anm2_scratch.c isaac_vita_anm2_scratch_free: only the six exact
 * site addresses base+offset < base+ISAAC_VITA_ANM2_MEMBLOCK_BYTES,
 * host_vita_texel_scratch.c isaac_vita_texel_scratch_free).  On the device
 * 0.06 % of frees reach a block (ph120.hc, 94 windows).
 *
 * With the knob ON each module keeps its published base in an exported
 * range record instead of a file-static atomic (the same object, so there is
 * still exactly one source of truth) and stores base + its OWN extent in
 * `end` before the existing release store of `base`.  The heap import then
 * skips all three calls when the pointer is outside every published range.
 *
 * Exactness: "outside every range" is only concluded from values the module
 * itself would read.  A module reached with such a pointer returns
 * NOT_HANDLED without side effects, so skipping the call changes nothing.
 * Torn or stale reads are conservative: base != 0 with end still 0 makes
 * end - base = -base and every v >= base answers "maybe"; base == 0 with a
 * stale end answers "maybe" below that end; the module then decides as today.
 * Reading 0 for a base that another thread is publishing concurrently is a
 * value the module's own acquire load could have read as well, because no
 * pointer into the block exists before publication.
 *
 * The extent is the module's own: the texel capacity is a TU-scoped define
 * (ISAAC_VITA_TEXEL_SCRATCH_8M/_32M reach only the texel and entry TUs), so
 * the heap must never compute it from its own copy of the constant.
 *
 * The records are defined by the module TUs only under the knob, so a build
 * that sets the knob on the heap TU but not on a module TU fails to link
 * instead of silently skipping that module. */
#ifndef HOST_VITA_HEAP_FREE_PREFILTER_H
#define HOST_VITA_HEAP_FREE_PREFILTER_H

#include <stdatomic.h>
#include <stdint.h>

typedef struct isaac_vita_heap_free_range {
    /* The module's published memblock base; 0 while unpublished. */
    atomic_uintptr_t base;
    /* base + the module's own extent; stored relaxed before base. */
    atomic_uintptr_t end;
} isaac_vita_heap_free_range;

/* Defined by host_vita_ogg_emergency.c, host_vita_anm2_scratch.c and
 * host_vita_texel_scratch.c, each only when compiled with the knob. */
extern isaac_vita_heap_free_range isaac_vita_ogg_emergency_free_range;
extern isaac_vita_heap_free_range isaac_vita_anm2_scratch_free_range;
extern isaac_vita_heap_free_range isaac_vita_texel_scratch_free_range;

/* 1 when `value` may lie in the published range (or the record is
 * mid-publication / mid-reset); 0 only when the module would certainly
 * answer NOT_HANDLED.  Unsigned wrap makes value < base fail the test. */
static inline int isaac_vita_heap_free_range_may_own(
    isaac_vita_heap_free_range *range, uintptr_t value)
{
    uintptr_t base = atomic_load_explicit(&range->base, memory_order_relaxed);
    uintptr_t end = atomic_load_explicit(&range->end, memory_order_relaxed);

    return value - base < end - base;
}

#endif
