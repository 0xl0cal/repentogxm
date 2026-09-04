#ifndef ISAAC_HOST_VITA_SYNC_FASTPATH_H
#define ISAAC_HOST_VITA_SYNC_FASTPATH_H

/* Inline single-owner success path for the two hot recursive-lock imports.
 *
 * Ownership proof (code census, wf/opt-sync): the only code that reads or
 * writes a guest CRITICAL_SECTION owner/depth word pair is host_vita_sync.c
 * (through the CPU that entry_vita.c binds once with
 * isaac_vita_sync_bind_current_thread) and manual_kage_vita.c (init/delete
 * with that same CPU).  Every other native thread the runtime creates
 * (isaac_save_writer, isaac_nv_vorbis on core 2, isaac_lua_beat,
 * isaac_guest_sampler, isaac_stall_watch) never calls guest code and never
 * touches a CS word.  gen_all drops the game's worker threads.  So while
 * exactly one bound CPU exists, the CLAIMING CAS, the acquire/release
 * barriers and the DelayThread wait loop of vita_sync_services.c can never
 * observe a second participant: plain loads and stores from the owning
 * thread produce the identical 24-byte object, EAX, ESP, low-water mark and
 * host-import census as the registered endpoint.
 *
 * Fail closed.  The inline path handles only the states in which the
 * registered endpoint provably succeeds without waiting, logging or
 * faulting:
 *   Enter: owner == self && 0 < depth < UINT32_MAX  -> depth + 1
 *          owner == 0    && depth == 0              -> depth = 1, owner = self
 *   Leave: owner == self && depth > 1               -> depth - 1
 *          owner == self && depth == 1              -> depth = 0, owner = 0
 * Every other state (uninitialized, wrong magic/handle/version, unaligned or
 * wrapping address, foreign or CLAIMING owner, zero/overflowing depth, ESP
 * outside the owned stack, unbound CPU) returns 0 without any side effect
 * and the caller continues into the unchanged registered endpoint, so every
 * existing fault text, wait and telemetry counter stays reachable.
 *
 * Latch.  g_isaac_vita_sync_inline_owner arms on the first bound thread UID
 * and is permanently disabled when a second distinct UID binds, when a CPU
 * with a different identity performs any owner/depth operation through the
 * registered endpoint, or when isaac_vita_sync_inline_disable() is called.
 * Once disabled no CPU can match it again.
 *
 * What the latch is and is not.  It is a SEQUENTIAL-participant guard, not a
 * concurrency protocol: the latch word and the owner/depth words are plain
 * loads and stores with no barrier, and the inline path is check-then-act
 * (latch compare, ~40 instructions, then the stores).  It therefore protects
 * against a second participant whose first CS operation does not overlap an
 * in-flight inline operation of the owner: a second thread that binds or
 * reaches the endpoint while the owner sits between its latch check and its
 * stores could race the owner's plain stores, and nothing orders the
 * DISABLED store against the owner's next plain load.  Today this is
 * unreachable because no second thread executes guest code at all (census
 * above).  REQUIREMENT for any future CreateThread/_beginthreadex shim, or
 * any other change that lets a second thread run guest code or touch CS
 * words: call isaac_vita_sync_inline_disable() on the CREATING (owner)
 * thread BEFORE the spawn.  Program order then makes every later inline
 * check on the owner see DISABLED, the thread-creation service orders the
 * store before the child runs, and the child can only ever use the
 * registered endpoint.  The oracle drives that helper too. */

#include <stdint.h>

#include "guest.h"
#include "host_vita_sync.h"
#include "vita_sync_services.h"

/* A bound thread UID is a positive int32 (1..0x7fffffff); an unbound CPU
 * carries 0.  Neither sentinel can ever equal a CPU's vita_sync_thread_id. */
#define ISAAC_VITA_SYNC_INLINE_OWNER_NONE     UINT32_MAX
#define ISAAC_VITA_SYNC_INLINE_OWNER_DISABLED UINT32_C(0x80000000)

extern uint32_t g_isaac_vita_sync_inline_owner;

/* Permanently disarms the inline path.  Must run on the owning thread
 * before any second guest-code thread is spawned (see the header comment);
 * safe to call at any time and from any state, including before the first
 * bind (NONE -> DISABLED means the first bind can no longer arm). */
static inline void isaac_vita_sync_inline_disable(void)
{
    g_isaac_vita_sync_inline_owner = ISAAC_VITA_SYNC_INLINE_OWNER_DISABLED;
}

/* First bind arms; any later distinct identity disables forever. */
static inline void isaac_vita_sync_inline_note_bound(uint32_t thread_id)
{
    uint32_t owner = g_isaac_vita_sync_inline_owner;

    if (owner == ISAAC_VITA_SYNC_INLINE_OWNER_NONE)
        g_isaac_vita_sync_inline_owner = thread_id;
    else if (owner != thread_id)
        g_isaac_vita_sync_inline_owner =
            ISAAC_VITA_SYNC_INLINE_OWNER_DISABLED;
}

/* Registered-endpoint entry for Enter/Leave/TryEnter/Delete: while armed,
 * an operation by any other CPU identity is a second participant. */
static inline void isaac_vita_sync_inline_note_endpoint(const CPU *c)
{
    uint32_t owner = g_isaac_vita_sync_inline_owner;

    if (owner != ISAAC_VITA_SYNC_INLINE_OWNER_NONE &&
        owner != ISAAC_VITA_SYNC_INLINE_OWNER_DISABLED &&
        c->vita_sync_thread_id != owner)
        g_isaac_vita_sync_inline_owner =
            ISAAC_VITA_SYNC_INLINE_OWNER_DISABLED;
}

/* Returns the critical-section address when the registered endpoint's
 * argument fetch, boundary, magic, handle and version checks all pass for
 * this bound CPU, otherwise 0.  No side effects.
 *
 * Stack predicate: the endpoint reads [esp+4] through guest_stack_address
 * (needs stack_owner == c, floor < ceiling, floor <= esp+4 <= ceiling-4) and
 * then pops the return address and adjusts by four (needs esp >= floor and
 * esp+8 <= ceiling).  The union is exactly capacity >= 8 and
 * esp - floor <= capacity - 8, evaluated wrap-free because floor < ceiling. */
static inline uint32_t isaac_vita_sync_inline_cs_address(const CPU *__restrict c)
{
    uint32_t esp = c->esp;
    uint32_t floor;
    uint32_t ceiling;
    uint32_t capacity;
    uint32_t address;

    if (c->vita_sync_thread_id != g_isaac_vita_sync_inline_owner)
        return 0U;
    if (c->stack_owner != c)
        return 0U;
    floor = c->stack_floor;
    ceiling = c->stack_ceiling;
    if (floor >= ceiling)
        return 0U;
    capacity = ceiling - floor;
    if (capacity < 8U || esp - floor > capacity - 8U)
        return 0U;
    address = ld32(esp + 4U);
    /* address != 0, four-byte aligned, and address + 23 does not wrap:
     * the endpoint's vita_sync_cs_boundary predicate in one compare. */
    if ((address & 3U) != 0U || address - 4U > UINT32_MAX - 27U)
        return 0U;
    if (ld32(address) != ISAAC_VITA_SYNC_CS_MAGIC)
        return 0U;
    if (ld32(address + 4U) != (uint32_t)ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE)
        return 0U;
    if (ld32(address + 12U) != ISAAC_VITA_SYNC_CS_VERSION)
        return 0U;
    return address;
}

/* The endpoint's observable success effects: isaac_vita_sync_import_indexed
 * counts the call, guest_stack_address moved the low-water mark to esp+4,
 * vita_sync_stdcall_return popped the return address and one argument.
 * EAX is untouched (both imports are void). */
static inline void isaac_vita_sync_inline_return(CPU *__restrict c,
                                                 unsigned *call_count)
{
    uint32_t esp = c->esp;

    ++*call_count;
    if (esp + 4U < c->stack_low_water)
        c->stack_low_water = esp + 4U;
    c->esp = esp + 8U;
}

static inline int isaac_vita_sync_inline_enter(CPU *__restrict c,
                                               unsigned *call_count)
{
    uint32_t address = isaac_vita_sync_inline_cs_address(c);
    uint32_t self;
    uint32_t owner;
    uint32_t depth;

    if (!address)
        return 0;
    self = c->vita_sync_thread_id;
    owner = ld32(address + 16U);
    depth = ld32(address + 20U);
    if (owner == self) {
        if (depth == 0U || depth == UINT32_MAX)
            return 0;
        st32(address + 20U, depth + 1U);
    } else if (owner == 0U && depth == 0U) {
        st32(address + 20U, 1U);
        st32(address + 16U, self);
    } else {
        return 0;
    }
    isaac_vita_sync_inline_return(c, call_count);
    return 1;
}

static inline int isaac_vita_sync_inline_leave(CPU *__restrict c,
                                               unsigned *call_count)
{
    uint32_t address = isaac_vita_sync_inline_cs_address(c);
    uint32_t owner;
    uint32_t depth;

    if (!address)
        return 0;
    owner = ld32(address + 16U);
    depth = ld32(address + 20U);
    if (owner != c->vita_sync_thread_id || depth == 0U)
        return 0;
    if (depth > 1U) {
        st32(address + 20U, depth - 1U);
    } else {
        st32(address + 20U, 0U);
        st32(address + 16U, 0U);
    }
    isaac_vita_sync_inline_return(c, call_count);
    return 1;
}

#endif
