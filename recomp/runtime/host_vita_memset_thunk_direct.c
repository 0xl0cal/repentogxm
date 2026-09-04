#include "host_vita_memset_thunk_direct.h"

#include <stddef.h>
#include <stdint.h>

#include "host_vita_import_id.h"
#include "host_vita_memory.h"

#if !defined(ISAAC_VITA_MEMSET_THUNK_FASTPATH) || \
    !ISAAC_VITA_MEMSET_THUNK_FASTPATH
#error The memset thunk direct helper must only be compiled when enabled
#endif

enum {
    ISAAC_MEMSET_IAT_RVA = 0x00606484U,
    ISAAC_MEMSET_IMPORT_ID = 280U,
    ISAAC_MEMSET_MEMORY_INDEX = 2U
};

int isaac_vita_memset_thunk_direct_try(
    CPU *__restrict c, uint32_t target)
{
    if (c == NULL || !g_isaac_vita_import_ids_ready ||
            (target != ISAAC_MEMSET_IAT_RVA &&
             target != (uint32_t)(GUEST_IMAGE_BASE + ISAAC_MEMSET_IAT_RVA)))
        return 0;

    /* guest_call records the logical call before import identity and before
     * a handler which may longjmp.  Preserve that ordering and the exact
     * dense coverage ID while bypassing only generic IAT classification and
     * the already-validated kind/local-index switch. */
    GUEST_PHASE_PROFILE_NOTE_CALL();
    guest_coverage_import(ISAAC_MEMSET_IMPORT_ID);
    if (!isaac_vita_memory_import_indexed(
            c, ISAAC_MEMSET_MEMORY_INDEX, &g_host_import_calls)) {
        guest_fault(c, target, ISAAC_VITA_MEMORY_MEMSET_NAME);
    }
    return 1;
}
