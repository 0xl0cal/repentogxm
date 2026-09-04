#include <stdint.h>

#include "kage_vita_stall_probe.h"

/* Compile-only owner used to pin the release OFF contract.  This uses the
 * same wrappers as guest.c/backend/hooks; OFF must be a literal empty body,
 * while ON must retain every expected relocation to the diagnostic module. */
void kage_vita_stall_gate_owner(uint32_t kind, uint32_t target,
                                uint32_t return_rva, uint32_t count)
{
    /* Also keep -Werror clean in the OFF expansion; these casts emit no code. */
    (void)kind;
    (void)target;
    (void)return_rva;
    (void)count;
    KAGE_VITA_STALL_START();
    KAGE_VITA_STALL_NOTE_LOOP();
    KAGE_VITA_STALL_NOTE_UPDATE();
    KAGE_VITA_STALL_NOTE_RENDER();
    KAGE_VITA_STALL_NOTE_RENDER_RETURN();
    KAGE_VITA_STALL_NOTE_PRESENT_ENTER(count);
    KAGE_VITA_STALL_NOTE_PRESENT_RETURN(count + 1u);
    KAGE_VITA_STALL_NOTE_GUEST_SITE(return_rva);
    KAGE_VITA_STALL_NOTE_DISPATCH(kind, target, return_rva);
    KAGE_VITA_STALL_NOTE_LOGICAL_FREAD(count);
    KAGE_VITA_STALL_NOTE_LOADING_SWAP(count);
    KAGE_VITA_STALL_TRACE_SYNC(KAGE_VITA_STALL_SYNC_FREAD_POWER, count);
    KAGE_VITA_STALL_STOP();
}
