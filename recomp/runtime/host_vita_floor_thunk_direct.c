#include "host_vita_floor_thunk_direct.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "host_vita_import_id.h"
#include "host_vita_math.h"
#if defined(ISAAC_VITA_GUEST_SAMPLER)
#include "kage_vita_guest_sampler.h"
#endif

#if !defined(ISAAC_VITA_FLOOR_THUNK_FASTPATH) || \
    !ISAAC_VITA_FLOOR_THUNK_FASTPATH
#error The floor thunk direct helper must only be compiled when enabled
#endif

/* Frozen dense row 313 of host_vita_import_id_map.inc: slot 0x606524,
 * api-ms-win-crt-math-l1-1-0.dll!floor, ISAAC_VITA_IMPORT_MATH local 2. */
enum {
    ISAAC_FLOOR_IAT_RVA = 0x00606524U,
    ISAAC_FLOOR_IMPORT_ID = 313U,
    ISAAC_FLOOR_MATH_INDEX = 2U,
    /* cdecl frame the thunk executes: return word + binary64 argument. */
    ISAAC_FLOOR_FRAME_BYTES = 12U
};
_Static_assert(ISAAC_FLOOR_IAT_RVA == ISAAC_VITA_MATH_FLOOR_IAT_RVA,
               "floor thunk seam slot drifted from the math family");
_Static_assert(ISAAC_VITA_MATH_FLOOR_THUNK_RVA == 0x005EC3B2U,
               "floor thunk seam root drifted from the math family");
_Static_assert(ISAAC_FLOOR_IMPORT_ID < ISAAC_VITA_IMPORT_ID_COUNT,
               "floor dense ID outside the 413-row contract");

/* The token guest_import_call accepts for this row: the slot's own VA
 * (guest.c guest_import_direct_rebuild).  Not an enum: it does not fit int. */
#define ISAAC_FLOOR_SLOT_VA \
    ((uint32_t)(GUEST_IMAGE_BASE + ISAAC_FLOOR_IAT_RVA))

int isaac_vita_floor_thunk_direct_try(
    CPU *__restrict c, uint32_t target)
{
    uint32_t esp;
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    uint32_t sampler_enclosing_target;
#endif

    /* Fail closed exactly where guest_import_call would: the 413-row table
     * must have registered and the slot word must still be this row's token
     * (a guest store into the IAT, a zeroed slot or a re-registration that
     * was refused all miss).  The frame check replaces the two noinline
     * range-checked helpers of the indexed route; a frame that fails it
     * takes the original route, which then raises the original stack fault
     * with its own receipts. */
    if (c == NULL || !g_isaac_vita_import_ids_ready ||
            target != ISAAC_FLOOR_SLOT_VA)
        return 0;
    esp = c->esp;
    if (!guest_stack_contains(c, esp, ISAAC_FLOOR_FRAME_BYTES))
        return 0;

#if defined(ISAAC_VITA_GUEST_SAMPLER)
    /* Publish the import as the current indirect callee for the duration of
     * the host work and restore the enclosing target after (the shape of
     * guest_try_direct_sync_import_call). */
    sampler_enclosing_target = g_kage_guest_last_indirect_target;
    g_kage_guest_last_indirect_target = target;
#endif
    /* Receipts of the indexed direct route, in its order: one logical call,
     * import coverage, the per-import census slot and the host import count
     * the family endpoint increments before it runs the handler. */
    GUEST_PHASE_PROFILE_NOTE_CALL();
    guest_coverage_import(ISAAC_FLOOR_IMPORT_ID);
    GUEST_PHASE_PROFILE_NOTE_IMPORT(ISAAC_FLOOR_IMPORT_ID);
    ++g_host_import_calls;

    /* vita_math_floor (host_vita_math.c):
     *     fpush(c, floor(ldd(guest_stack_address(c, c->esp + 4U, 8U, 0U))));
     *     (void)gpop(c);
     * guest_stack_address noted the argument address as a low-water
     * candidate; gpop read the return word (dead) and advanced ESP by four.
     * The return word is not inspected here either (translated callers push
     * RVAs, never image VAs). */
    guest_stack_note_low(c, esp + 4U);
    fpush(c, floor(ldd(esp + 4U)));
    c->esp = esp + 4U;
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    g_kage_guest_last_indirect_target = sampler_enclosing_target;
#endif
    return 1;
}
