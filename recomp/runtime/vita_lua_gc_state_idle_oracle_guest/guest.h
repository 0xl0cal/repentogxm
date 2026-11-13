/* Stand-in for recomp/runtime/guest.h, for vita_lua_gc_state_idle_oracle.c
 * only.  The oracle includes this file by its path before host_vita_lua_gc.c
 * and it claims the real header's include guard (guest.h: ISAAC_GUEST_H), so
 * the `#include "guest.h"` of host_vita_lua_gc.h, which a quoted include
 * resolves next to the including file whatever -I says, becomes empty.
 * host_vita_lua_gc.c needs four things from the real header: the CPU type,
 * the guest stack read that yields the lua_gc call site, and the image base
 * its finalizer-range test subtracts.  The oracle supplies the call site
 * directly, so nothing here models guest memory. */
#ifndef ISAAC_GUEST_H
#define ISAAC_GUEST_H

#include <stdint.h>

typedef struct CPU {
    uint32_t esp;
    uint32_t eax;
} CPU;

/* The oracle never passes a finalizer target; any base serves. */
#define GUEST_IMAGE_BASE 0U

extern uint32_t g_oracle_gc_site;

static inline uint32_t guest_stack_address(CPU *c, uint32_t address,
                                           uint32_t size, uint32_t flags)
{
    (void)c;
    (void)address;
    (void)size;
    (void)flags;
    return 1U;
}

static inline uint32_t ld32(uint32_t address)
{
    (void)address;
    return g_oracle_gc_site;
}

#endif
