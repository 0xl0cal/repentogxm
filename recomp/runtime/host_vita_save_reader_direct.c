#include "host_vita_save_reader_direct.h"

#include <stddef.h>
#include <stdint.h>

#if !defined(ISAAC_VITA_SAVE_READER_DIRECT_EDGES) || \
    !ISAAC_VITA_SAVE_READER_DIRECT_EDGES
#error The Save Reader direct edge helper must only be compiled when enabled
#endif

enum {
    ISAAC_SAVE_READER_TARGET_RVA = 0x0025ba60U
};

extern void sub_0025ba60(CPU *__restrict c);

int isaac_vita_save_reader_direct_try(
    CPU *__restrict c, uint32_t target)
{
    if (c == NULL ||
            (target != ISAAC_SAVE_READER_TARGET_RVA &&
             target != (uint32_t)(GUEST_IMAGE_BASE +
                                  ISAAC_SAVE_READER_TARGET_RVA)))
        return 0;

    /* Match the logical census of the cached guest_call being replaced.
     * The exported cache note is a no-op when that cache is not compiled. */
    GUEST_PHASE_PROFILE_NOTE_CALL();
    GUEST_PHASE_PROFILE_NOTE_LOOKUP();
#if defined(ISAAC_VITA_PHASE_PROFILE)
    guest_phase_profile_note_lookup_cache_hit();
#endif
    sub_0025ba60(c);
    return 1;
}
