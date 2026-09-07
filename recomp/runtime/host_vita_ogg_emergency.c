/* One retained emergency USER_RW slot for the exact OGG Open allocation which
 * terminated the measured post-death run after newlib fragmentation.  This is
 * intentionally not a general audio pool: the PE proves no global stream-count
 * bound. The opt-in Queue policy admits its exact allocation owner to the SAME
 * single slot after native malloc failure. A live slot still falls through to
 * the original allocation failure; no backing is shared by two live streams. */
#include <limits.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <psp2/kernel/sysmem.h>

#include "host_vita_ogg_emergency.h"
#include "kage_vita_texture_memory.h"
#include "platform.h"

#if !defined(ISAAC_VITA_OGG_EMERGENCY_ORACLE) && UINTPTR_MAX != UINT32_MAX
#error OGG emergency slot requires a 32-bit identity-mapped Vita address space
#endif

typedef struct ogg_emergency_state {
    SceUID uid;
    uint32_t poisoned;
    uint32_t live;
    size_t live_size;
    uint32_t acquired_count;
    uint32_t freed_count;
    uint32_t live_fallback_count;
    uint32_t reserve_failure_count;
} ogg_emergency_state;

_Static_assert((ISAAC_VITA_OGG_OPEN_BACKING_BYTES &
                (ISAAC_VITA_OGG_MEMBLOCK_PAGE_BYTES - 1U)) == 0U,
               "OGG emergency backing must be page aligned");
_Static_assert(KAGE_VITA_TEXEL_LINK_END <
                   KAGE_VITA_TEXEL_HEAP_RETAINED_END,
               "OGG emergency frozen heap interval is invalid");
_Static_assert(KAGE_VITA_TEXEL_GUEST_IMAGE_BASE <
                   KAGE_VITA_TEXEL_GUEST_IMAGE_END,
               "OGG emergency frozen image interval is invalid");
_Static_assert(ISAAC_VITA_OGG_OPEN_ALLOCATION_RETURN_RVA !=
                   ISAAC_VITA_OGG_QUEUE_ALLOCATION_RETURN_RVA &&
               ISAAC_VITA_OGG_OPEN_ALLOCATION_RETURN_RVA !=
                   ISAAC_VITA_OGG_QUEUE_DECODER_RETURN_RVA &&
               ISAAC_VITA_OGG_OPEN_ALLOCATION_RETURN_RVA !=
                   ISAAC_VITA_OGG_THIRD_SIZE_OWNER_RVA,
               "OGG Open owner must remain uniquely scoped");

static ogg_emergency_state s_state = { .uid = -1 };
static atomic_uintptr_t s_published_base = ATOMIC_VAR_INIT((uintptr_t)0U);
static atomic_flag s_state_lock = ATOMIC_FLAG_INIT;

static int ogg_emergency_try_lock(void)
{
    return !atomic_flag_test_and_set_explicit(
        &s_state_lock, memory_order_acquire);
}

static void ogg_emergency_unlock(void)
{
    atomic_flag_clear_explicit(&s_state_lock, memory_order_release);
}

static void ogg_emergency_clear_decision(
    isaac_vita_ogg_emergency_decision *decision)
{
    if (!decision)
        return;
    decision->pointer = NULL;
    decision->fault_value = 0U;
    decision->fault = NULL;
}

static int ogg_emergency_reject(
    isaac_vita_ogg_emergency_decision *decision, uint32_t value,
    const char *fault)
{
    if (decision) {
        decision->pointer = NULL;
        decision->fault_value = value;
        decision->fault = fault;
    }
    return ISAAC_VITA_OGG_EMERGENCY_REJECTED;
}

static int ogg_emergency_ranges_overlap(
    uintptr_t first_start, uintptr_t first_end,
    uintptr_t second_start, uintptr_t second_end)
{
    return first_start < second_end && second_start < first_end;
}

static int ogg_emergency_base_is_valid(const void *base)
{
    uintptr_t value = (uintptr_t)base;

    if (!value ||
        (value & (ISAAC_VITA_OGG_MEMBLOCK_PAGE_BYTES - 1U)) != 0U ||
        value > UINTPTR_MAX - ISAAC_VITA_OGG_OPEN_BACKING_BYTES)
        return 0;
#ifndef ISAAC_VITA_OGG_EMERGENCY_ORACLE
    if (value > (uintptr_t)UINT32_MAX -
                    ISAAC_VITA_OGG_OPEN_BACKING_BYTES)
        return 0;
#endif
    return 1;
}

static int ogg_emergency_range_is_safe(
    uintptr_t base, uint32_t guest_stack_floor,
    uint32_t guest_stack_ceiling)
{
    uintptr_t end = base + ISAAC_VITA_OGG_OPEN_BACKING_BYTES;

    if (guest_stack_floor >= guest_stack_ceiling)
        return 0;
    if (ogg_emergency_ranges_overlap(
            base, end, KAGE_VITA_TEXEL_LINK_END,
            KAGE_VITA_TEXEL_HEAP_RETAINED_END))
        return 0;
    if (ogg_emergency_ranges_overlap(
            base, end, KAGE_VITA_TEXEL_GUEST_IMAGE_BASE,
            KAGE_VITA_TEXEL_GUEST_IMAGE_END))
        return 0;
    if (ogg_emergency_ranges_overlap(
            base, end, guest_stack_floor, guest_stack_ceiling))
        return 0;
    return 1;
}

/* Return 1 when ready, 0 for a clean allocation/rollback failure, and -1 if
 * rollback itself failed and the owner must remain poisoned. */
static int ogg_emergency_reserve_locked(
    uint32_t guest_stack_floor, uint32_t guest_stack_ceiling,
    isaac_vita_ogg_emergency_decision *decision)
{
    SceUID uid;
    void *base = NULL;
    int get_result;
    int free_result;
    const char *status;

    if (s_state.poisoned)
        return ogg_emergency_reject(
            decision, 0U, "OGG emergency memblock is poisoned");
    if (atomic_load_explicit(&s_published_base, memory_order_relaxed))
        return 1;

    uid = sceKernelAllocMemBlock(
        "isaac_ogg_emergency", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
        ISAAC_VITA_OGG_OPEN_BACKING_BYTES, NULL);
    if (uid < 0) {
        ++s_state.reserve_failure_count;
        isaac_vita_log(
            "OGG emergency reserve: status=alloc-failed result=0x%08x "
            "capacity=%u",
            (unsigned)uid,
            (unsigned)ISAAC_VITA_OGG_OPEN_BACKING_BYTES);
        return 0;
    }

    get_result = sceKernelGetMemBlockBase(uid, &base);
    if (get_result >= 0 && ogg_emergency_base_is_valid(base) &&
        ogg_emergency_range_is_safe(
            (uintptr_t)base, guest_stack_floor, guest_stack_ceiling)) {
        s_state.uid = uid;
        atomic_store_explicit(
            &s_published_base, (uintptr_t)base, memory_order_release);
        isaac_vita_log(
            "OGG emergency reserve: status=ready uid=0x%08x base=%p "
            "end=0x%08x capacity=%u",
            (unsigned)uid, base,
            (unsigned)((uintptr_t)base +
                       ISAAC_VITA_OGG_OPEN_BACKING_BYTES),
            (unsigned)ISAAC_VITA_OGG_OPEN_BACKING_BYTES);
        return 1;
    }

    status = get_result < 0 ? "get-base-failed" :
        !ogg_emergency_base_is_valid(base) ? "invalid-base" :
        "range-overlap";
    free_result = sceKernelFreeMemBlock(uid);
    ++s_state.reserve_failure_count;
    isaac_vita_log(
        "OGG emergency reserve: status=%s uid=0x%08x get=0x%08x "
        "base=%p rollback=0x%08x",
        status, (unsigned)uid, (unsigned)get_result, base,
        (unsigned)free_result);
    if (free_result >= 0)
        return 0;

    s_state.uid = uid;
    s_state.poisoned = 1U;
    return ogg_emergency_reject(
        decision, (uint32_t)free_result,
        "OGG emergency memblock rollback failed");
}

int isaac_vita_ogg_emergency_malloc(
    uint32_t owner_return_rva, size_t size,
    uint32_t guest_stack_floor, uint32_t guest_stack_ceiling,
    isaac_vita_ogg_emergency_decision *decision)
{
    uintptr_t base;
    uint32_t acquired_count;
    int reserve_result;

    ogg_emergency_clear_decision(decision);
    if ((owner_return_rva !=
            ISAAC_VITA_OGG_OPEN_ALLOCATION_RETURN_RVA
#ifdef ISAAC_VITA_OGG_QUEUE_EMERGENCY
         && owner_return_rva !=
            ISAAC_VITA_OGG_QUEUE_ALLOCATION_RETURN_RVA
#endif
        ) ||
        size != ISAAC_VITA_OGG_OPEN_BACKING_BYTES)
        return ISAAC_VITA_OGG_EMERGENCY_NOT_HANDLED;
    if (!decision)
        return ISAAC_VITA_OGG_EMERGENCY_REJECTED;
    if (guest_stack_floor >= guest_stack_ceiling)
        return ogg_emergency_reject(
            decision, guest_stack_floor,
            "OGG emergency guest stack range is invalid");
    if (!ogg_emergency_try_lock())
        return ISAAC_VITA_OGG_EMERGENCY_NOT_HANDLED;

    if (s_state.poisoned) {
        ogg_emergency_unlock();
        return ogg_emergency_reject(
            decision, owner_return_rva,
            "OGG emergency memblock is poisoned");
    }
    if (s_state.live) {
        ++s_state.live_fallback_count;
        ogg_emergency_unlock();
        return ISAAC_VITA_OGG_EMERGENCY_NOT_HANDLED;
    }

    base = atomic_load_explicit(&s_published_base, memory_order_relaxed);
    if (base && !ogg_emergency_range_is_safe(
                    base, guest_stack_floor, guest_stack_ceiling)) {
        ogg_emergency_unlock();
        return ogg_emergency_reject(
            decision, (uint32_t)base,
            "OGG emergency retained range became unsafe");
    }
    reserve_result = ogg_emergency_reserve_locked(
        guest_stack_floor, guest_stack_ceiling, decision);
    if (reserve_result <= 0) {
        ogg_emergency_unlock();
        return reserve_result < 0
            ? ISAAC_VITA_OGG_EMERGENCY_REJECTED
            : ISAAC_VITA_OGG_EMERGENCY_NOT_HANDLED;
    }

    base = atomic_load_explicit(&s_published_base, memory_order_relaxed);
    if (!base) {
        s_state.poisoned = 1U;
        ogg_emergency_unlock();
        return ogg_emergency_reject(
            decision, owner_return_rva,
            "OGG emergency lost its reserved base");
    }
    s_state.live = 1U;
    s_state.live_size = size;
    ++s_state.acquired_count;
    acquired_count = s_state.acquired_count;
    decision->pointer = (void *)base;
    ogg_emergency_unlock();
    isaac_vita_log(
        "OGG emergency acquire: owner=0x%08x request=%u acquired=%u",
        (unsigned)owner_return_rva, (unsigned)size,
        (unsigned)acquired_count);
    return ISAAC_VITA_OGG_EMERGENCY_HANDLED;
}

int isaac_vita_ogg_emergency_free(
    void *pointer, isaac_vita_ogg_emergency_decision *decision)
{
    uintptr_t base;
    uintptr_t value = (uintptr_t)pointer;
    uint32_t freed_count;

    ogg_emergency_clear_decision(decision);
    if (!pointer)
        return ISAAC_VITA_OGG_EMERGENCY_NOT_HANDLED;
    base = atomic_load_explicit(&s_published_base, memory_order_acquire);
    if (!base || value < base ||
        value >= base + ISAAC_VITA_OGG_OPEN_BACKING_BYTES)
        return ISAAC_VITA_OGG_EMERGENCY_NOT_HANDLED;
    if (!decision)
        return ISAAC_VITA_OGG_EMERGENCY_REJECTED;
    if (value != base)
        return ogg_emergency_reject(
            decision, (uint32_t)value,
            "free received an interior OGG emergency pointer");
    if (!ogg_emergency_try_lock())
        return ogg_emergency_reject(
            decision, (uint32_t)value,
            "OGG emergency concurrent/reentrant free");
    if (base != atomic_load_explicit(
                    &s_published_base, memory_order_relaxed)) {
        s_state.poisoned = 1U;
        ogg_emergency_unlock();
        return ogg_emergency_reject(
            decision, (uint32_t)value,
            "OGG emergency base changed while freeing");
    }
    if (!s_state.live) {
        ogg_emergency_unlock();
        return ogg_emergency_reject(
            decision, (uint32_t)value,
            "OGG emergency double/stale free");
    }

    s_state.live = 0U;
    s_state.live_size = 0U;
    ++s_state.freed_count;
    freed_count = s_state.freed_count;
    ogg_emergency_unlock();
    isaac_vita_log(
        "OGG emergency release: freed=%u",
        (unsigned)freed_count);
    return ISAAC_VITA_OGG_EMERGENCY_HANDLED;
}

int isaac_vita_ogg_emergency_realloc(
    void *pointer, size_t size,
    isaac_vita_ogg_emergency_decision *decision)
{
    uintptr_t base;
    uintptr_t value = (uintptr_t)pointer;

    ogg_emergency_clear_decision(decision);
    if (!pointer)
        return ISAAC_VITA_OGG_EMERGENCY_NOT_HANDLED;
    base = atomic_load_explicit(&s_published_base, memory_order_acquire);
    if (!base || value < base ||
        value >= base + ISAAC_VITA_OGG_OPEN_BACKING_BYTES)
        return ISAAC_VITA_OGG_EMERGENCY_NOT_HANDLED;
    if (!decision)
        return ISAAC_VITA_OGG_EMERGENCY_REJECTED;
    if (value != base)
        return ogg_emergency_reject(
            decision, (uint32_t)value,
            "realloc received an interior OGG emergency pointer");
    if (!ogg_emergency_try_lock())
        return ogg_emergency_reject(
            decision, (uint32_t)value,
            "OGG emergency concurrent/reentrant realloc");
    if (base != atomic_load_explicit(
                    &s_published_base, memory_order_relaxed)) {
        s_state.poisoned = 1U;
        ogg_emergency_unlock();
        return ogg_emergency_reject(
            decision, (uint32_t)value,
            "OGG emergency base changed while reallocating");
    }
    if (!s_state.live) {
        ogg_emergency_unlock();
        return ogg_emergency_reject(
            decision, (uint32_t)value,
            "realloc received a stale OGG emergency pointer");
    }

    if (!size) {
        s_state.live = 0U;
        s_state.live_size = 0U;
        ++s_state.freed_count;
        decision->pointer = NULL;
    }
    else if (size <= ISAAC_VITA_OGG_OPEN_BACKING_BYTES) {
        s_state.live_size = size;
        decision->pointer = (void *)base;
    }
    else {
        /* Standard realloc failure: retain the old live allocation. */
        decision->pointer = NULL;
    }
    ogg_emergency_unlock();
    return ISAAC_VITA_OGG_EMERGENCY_HANDLED;
}

#ifdef ISAAC_VITA_HEAP_CENSUS
uint32_t isaac_vita_ogg_emergency_live_count(void)
{
    uint32_t live;

    if (!ogg_emergency_try_lock())
        return UINT32_MAX;
    live = s_state.live;
    ogg_emergency_unlock();
    return live;
}
#endif

#ifdef ISAAC_VITA_OGG_EMERGENCY_ORACLE
int isaac_vita_ogg_emergency_oracle_snapshot(
    isaac_vita_ogg_emergency_snapshot *snapshot)
{
    if (!snapshot || !ogg_emergency_try_lock())
        return 0;
    snapshot->base = atomic_load_explicit(
        &s_published_base, memory_order_relaxed);
    snapshot->uid = (int32_t)s_state.uid;
    snapshot->poisoned = s_state.poisoned;
    snapshot->live = s_state.live;
    snapshot->live_size = s_state.live_size;
    snapshot->acquired_count = s_state.acquired_count;
    snapshot->freed_count = s_state.freed_count;
    snapshot->live_fallback_count = s_state.live_fallback_count;
    snapshot->reserve_failure_count = s_state.reserve_failure_count;
    ogg_emergency_unlock();
    return 1;
}

int isaac_vita_ogg_emergency_oracle_reset(void)
{
    int result = 0;

    if (!ogg_emergency_try_lock())
        return INT_MIN;
    if (s_state.uid >= 0)
        result = sceKernelFreeMemBlock(s_state.uid);
    memset(&s_state, 0, sizeof s_state);
    s_state.uid = -1;
    atomic_store_explicit(
        &s_published_base, (uintptr_t)0U, memory_order_release);
    ogg_emergency_unlock();
    return result;
}

int isaac_vita_ogg_emergency_oracle_force_lock(int locked)
{
    if (locked)
        return ogg_emergency_try_lock();
    ogg_emergency_unlock();
    return 1;
}
#endif
