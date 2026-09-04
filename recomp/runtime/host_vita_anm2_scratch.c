/* Dedicated, default-OFF storage for the six fixed ANM2 parser pools.
 *
 * This source is linked only when ISAAC_VITA_ANM2_SCRATCH is selected by the
 * Vita recipe.  It never calls malloc/calloc/realloc/free: one page-rounded
 * USER_RW memblock is reserved lazily and intentionally retained until
 * process exit.  Pool ownership remains exact-base and session-scoped. */
#include <limits.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <psp2/kernel/sysmem.h>

#include "host_vita_anm2_scratch.h"
#include "platform.h"

#if !defined(ISAAC_VITA_ANM2_SCRATCH_ORACLE) && UINTPTR_MAX != UINT32_MAX
#error ANM2 scratch requires a 32-bit identity-mapped Vita address space
#endif

typedef struct anm2_scratch_site {
    uint32_t owner_return_rva;
    uint32_t size;
    uint32_t offset;
} anm2_scratch_site;

typedef struct anm2_scratch_state {
    SceUID uid;
    uint32_t poisoned;
    uint32_t guest_heap_fallback;
    uint32_t guest_stack_floor;
    uint32_t guest_stack_ceiling;
    uint32_t session_id;
    uint32_t live_count;
    uint32_t acquired_count;
    uint32_t freed_count;
    uint32_t peak_live;
    uint8_t live[ISAAC_VITA_ANM2_SEGMENT_COUNT];
} anm2_scratch_state;

static const anm2_scratch_site s_sites[ISAAC_VITA_ANM2_SEGMENT_COUNT] = {
    { ISAAC_VITA_ANM2_OWNER_RETURN_0,
      ISAAC_VITA_ANM2_SMALL_SEGMENT_BYTES,       0U },
    { ISAAC_VITA_ANM2_OWNER_RETURN_1,
      ISAAC_VITA_ANM2_SMALL_SEGMENT_BYTES,  420000U },
    { ISAAC_VITA_ANM2_OWNER_RETURN_2,
      ISAAC_VITA_ANM2_SMALL_SEGMENT_BYTES,  840000U },
    { ISAAC_VITA_ANM2_OWNER_RETURN_3,
      ISAAC_VITA_ANM2_SMALL_SEGMENT_BYTES, 1260000U },
    { ISAAC_VITA_ANM2_OWNER_RETURN_4,
      ISAAC_VITA_ANM2_LARGE_SEGMENT_BYTES, 1680000U },
    { ISAAC_VITA_ANM2_OWNER_RETURN_5,
      ISAAC_VITA_ANM2_LARGE_SEGMENT_BYTES, 2220000U }
};

_Static_assert(sizeof s_sites / sizeof s_sites[0] ==
                   ISAAC_VITA_ANM2_SEGMENT_COUNT,
               "ANM2 scratch site count drifted");
_Static_assert(4U * ISAAC_VITA_ANM2_SMALL_SEGMENT_BYTES +
                   2U * ISAAC_VITA_ANM2_LARGE_SEGMENT_BYTES ==
                   ISAAC_VITA_ANM2_PAYLOAD_BYTES,
               "ANM2 scratch payload arithmetic drifted");
_Static_assert((ISAAC_VITA_ANM2_PAYLOAD_BYTES +
                ISAAC_VITA_ANM2_MEMBLOCK_PAGE_BYTES - 1U) /
                   ISAAC_VITA_ANM2_MEMBLOCK_PAGE_BYTES *
                   ISAAC_VITA_ANM2_MEMBLOCK_PAGE_BYTES ==
                   ISAAC_VITA_ANM2_MEMBLOCK_BYTES,
               "ANM2 scratch page rounding drifted");
_Static_assert(2220000U + ISAAC_VITA_ANM2_LARGE_SEGMENT_BYTES ==
                   ISAAC_VITA_ANM2_PAYLOAD_BYTES,
               "ANM2 scratch final segment exceeds payload");
_Static_assert((ISAAC_VITA_ANM2_SMALL_SEGMENT_BYTES & 15U) == 0U &&
                   (ISAAC_VITA_ANM2_LARGE_SEGMENT_BYTES & 15U) == 0U,
               "ANM2 scratch segments must preserve 16-byte alignment");
_Static_assert(ISAAC_VITA_ANM2_GUEST_IMAGE_BASE +
                   ISAAC_VITA_ANM2_GUEST_IMAGE_BYTES ==
                   ISAAC_VITA_ANM2_GUEST_IMAGE_END,
               "ANM2 scratch frozen image range drifted");
_Static_assert((ISAAC_VITA_ANM2_HEAP_BYTES &
                (0U - ISAAC_VITA_ANM2_HEAP_BYTES)) ==
                   ISAAC_VITA_ANM2_HEAP_ALIGNMENT_PAD,
               "guest heap changed Vita3K alignment retention");
_Static_assert(((ISAAC_VITA_ANM2_LINK_END +
                 ISAAC_VITA_ANM2_HEAP_BYTES +
                 ISAAC_VITA_ANM2_HEAP_ALIGNMENT_PAD +
                 ISAAC_VITA_ANM2_MEMBLOCK_PAGE_BYTES - 1U) &
                ~(ISAAC_VITA_ANM2_MEMBLOCK_PAGE_BYTES - 1U)) ==
                   ISAAC_VITA_ANM2_HEAP_RETAINED_END,
               "ANM2 scratch heap retained-end formula drifted");
/* The following frozen gap arithmetic is an isolated capacity proof for the
 * previously accepted placement before the guest image.  It is not a runtime
 * placement requirement: eager USER_RW reservations may consume that gap, and
 * the kernel may return this block elsewhere.  The actual returned range is
 * accepted or rejected only by anm2_scratch_range_is_safe() below. */
_Static_assert(ISAAC_VITA_ANM2_GUEST_IMAGE_BASE -
                   ISAAC_VITA_ANM2_HEAP_RETAINED_END ==
                   ISAAC_VITA_ANM2_PRE_IMAGE_GAP_BYTES,
               "isolated ANM2 pre-image capacity arithmetic drifted");
_Static_assert(ISAAC_VITA_ANM2_MEMBLOCK_BYTES <=
                   ISAAC_VITA_ANM2_PRE_IMAGE_GAP_BYTES,
               "isolated ANM2 block no longer fits the old pre-image gap");
_Static_assert((ISAAC_VITA_ANM2_MEMBLOCK_BYTES &
                (0U - ISAAC_VITA_ANM2_MEMBLOCK_BYTES)) ==
                   ISAAC_VITA_ANM2_VITA3K_ALIGNMENT,
               "ANM2 page-rounded size changed Vita3K alignment");
_Static_assert(ISAAC_VITA_ANM2_MEMBLOCK_BYTES +
                   ISAAC_VITA_ANM2_VITA3K_ALIGNMENT ==
                   ISAAC_VITA_ANM2_VITA3K_RETAINED_MAX,
               "ANM2 Vita3K retained-span bound drifted");
_Static_assert(ISAAC_VITA_ANM2_VITA3K_RETAINED_MAX <=
                   ISAAC_VITA_ANM2_PRE_IMAGE_GAP_BYTES,
               "isolated ANM2 retained bound no longer fits old placement");
_Static_assert(ISAAC_VITA_ANM2_PRE_IMAGE_GAP_BYTES -
                   ISAAC_VITA_ANM2_VITA3K_RETAINED_MAX ==
                   ISAAC_VITA_ANM2_GAP_REMAINING_BYTES,
               "isolated ANM2 old-placement margin drifted");

static anm2_scratch_state s_state = { .uid = -1 };
static atomic_uintptr_t s_published_base = ATOMIC_VAR_INIT((uintptr_t)0U);
static atomic_flag s_state_lock = ATOMIC_FLAG_INIT;

static int anm2_scratch_try_lock(void)
{
    return !atomic_flag_test_and_set_explicit(
        &s_state_lock, memory_order_acquire);
}

static void anm2_scratch_unlock(void)
{
    atomic_flag_clear_explicit(&s_state_lock, memory_order_release);
}

static int anm2_scratch_should_log_session(uint32_t session_id)
{
    /* Successful per-session logging opens and closes the native log file.
     * Keep exact early/power-of-two progress without turning asset bursts into
     * hundreds of synchronous I/O operations.  Failures remain unconditional. */
    return session_id && !(session_id & (session_id - 1U));
}

static void anm2_scratch_clear_decision(
    isaac_vita_anm2_scratch_decision *decision)
{
    if (!decision)
        return;
    decision->pointer = NULL;
    decision->fault_value = 0U;
    decision->fault = NULL;
}

static int anm2_scratch_reject(
    isaac_vita_anm2_scratch_decision *decision, uint32_t value,
    const char *fault)
{
    if (decision) {
        decision->pointer = NULL;
        decision->fault_value = value;
        decision->fault = fault;
    }
    return ISAAC_VITA_ANM2_SCRATCH_REJECTED;
}

static int anm2_scratch_find_owner(uint32_t owner_return_rva)
{
    uint32_t index;

    for (index = 0U; index < ISAAC_VITA_ANM2_SEGMENT_COUNT; ++index) {
        if (s_sites[index].owner_return_rva == owner_return_rva)
            return (int)index;
    }
    return -1;
}

static int anm2_scratch_find_pointer(uintptr_t base, uintptr_t pointer)
{
    uint32_t index;

    for (index = 0U; index < ISAAC_VITA_ANM2_SEGMENT_COUNT; ++index) {
        if (base + s_sites[index].offset == pointer)
            return (int)index;
    }
    return -1;
}

/* The dedicated block has no transaction to own after the exact physical-page
 * failure.  Keep validating the frozen six-call shape, but let the common
 * ledger-backed heap router own every returned pointer and its lifetime.  The
 * fallback stays process-scoped so later sessions do not repeatedly probe a
 * native allocator that has already reported global page exhaustion. */
static int anm2_scratch_route_guest_heap_locked(
    int index, uint32_t owner_return_rva,
    uint32_t guest_stack_floor, uint32_t guest_stack_ceiling,
    isaac_vita_anm2_scratch_decision *decision)
{
    uintptr_t published = atomic_load_explicit(
        &s_published_base, memory_order_relaxed);

    if (!s_state.guest_heap_fallback || published || s_state.uid >= 0 ||
        s_state.live_count ||
        s_state.acquired_count > ISAAC_VITA_ANM2_SEGMENT_COUNT) {
        s_state.poisoned = 1U;
        anm2_scratch_unlock();
        return anm2_scratch_reject(
            decision, owner_return_rva,
            "ANM2 scratch guest-heap fallback state is corrupted");
    }
    if (s_state.guest_stack_floor != guest_stack_floor ||
        s_state.guest_stack_ceiling != guest_stack_ceiling) {
        anm2_scratch_unlock();
        return anm2_scratch_reject(
            decision, guest_stack_floor,
            "ANM2 scratch guest stack owner changed");
    }
    if (s_state.acquired_count == ISAAC_VITA_ANM2_SEGMENT_COUNT) {
        if (index != 0) {
            anm2_scratch_unlock();
            return anm2_scratch_reject(
                decision, owner_return_rva,
                "ANM2 scratch fallback session did not restart at AA5D");
        }
        s_state.acquired_count = 0U;
    }
    if (s_state.acquired_count == 0U) {
        if (index != 0) {
            anm2_scratch_unlock();
            return anm2_scratch_reject(
                decision, owner_return_rva,
                "ANM2 scratch fallback session did not start at AA5D");
        }
        ++s_state.session_id;
        if (!s_state.session_id)
            ++s_state.session_id;
        if (anm2_scratch_should_log_session(s_state.session_id))
            isaac_vita_log(
                "ANM2 scratch fallback session: id=%u first=0x%08x "
                "segments=%u payload=%u route=guest-heap",
                (unsigned)s_state.session_id, (unsigned)owner_return_rva,
                (unsigned)ISAAC_VITA_ANM2_SEGMENT_COUNT,
                (unsigned)ISAAC_VITA_ANM2_PAYLOAD_BYTES);
    }
    if ((uint32_t)index != s_state.acquired_count) {
        anm2_scratch_unlock();
        return anm2_scratch_reject(
            decision, owner_return_rva,
            "ANM2 scratch fallback allocation order drifted");
    }
    ++s_state.acquired_count;
    anm2_scratch_unlock();
    return ISAAC_VITA_ANM2_SCRATCH_ROUTE_GUEST_HEAP;
}

static int anm2_scratch_base_is_valid(const void *base)
{
    uintptr_t value = (uintptr_t)base;

    if (!value ||
        (value & (ISAAC_VITA_ANM2_MEMBLOCK_PAGE_BYTES - 1U)) != 0U ||
        value > UINTPTR_MAX - ISAAC_VITA_ANM2_MEMBLOCK_BYTES)
        return 0;
#ifndef ISAAC_VITA_ANM2_SCRATCH_ORACLE
    if (value > (uintptr_t)UINT32_MAX - ISAAC_VITA_ANM2_MEMBLOCK_BYTES)
        return 0;
#endif
    return 1;
}

static int anm2_scratch_ranges_overlap(uintptr_t first_start,
                                       uintptr_t first_end,
                                       uintptr_t second_start,
                                       uintptr_t second_end)
{
    return first_start < second_end && second_start < first_end;
}

static int anm2_scratch_range_is_safe(uintptr_t base,
                                      uint32_t guest_stack_floor,
                                      uint32_t guest_stack_ceiling)
{
    uintptr_t end = base + ISAAC_VITA_ANM2_MEMBLOCK_BYTES;

    if (guest_stack_floor >= guest_stack_ceiling)
        return 0;
    if (anm2_scratch_ranges_overlap(
            base, end, ISAAC_VITA_ANM2_GUEST_IMAGE_BASE,
            ISAAC_VITA_ANM2_GUEST_IMAGE_END))
        return 0;
    if (anm2_scratch_ranges_overlap(
            base, end, guest_stack_floor, guest_stack_ceiling))
        return 0;
    return 1;
}

/* Return 1 when ready, 0 for a clean native allocation failure, and -1 when
 * cleanup could not close the native transaction and the seam is poisoned. */
static int anm2_scratch_reserve_locked(
    uint32_t guest_stack_floor, uint32_t guest_stack_ceiling,
    isaac_vita_anm2_scratch_decision *decision)
{
    SceUID uid;
    void *base = NULL;
    int get_result;
    int free_result;
    const char *status;

    if (s_state.poisoned)
        return anm2_scratch_reject(
            decision, 0U, "ANM2 scratch memblock is poisoned");
    if (atomic_load_explicit(&s_published_base, memory_order_relaxed))
        return 1;

    uid = sceKernelAllocMemBlock(
        "isaac_anm2_scratch", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
        ISAAC_VITA_ANM2_MEMBLOCK_BYTES, NULL);
    if (uid < 0) {
        int route_guest_heap =
            (int32_t)uid == ISAAC_VITA_ANM2_NO_FREE_PHYSICAL_PAGE;

        isaac_vita_log(
            "ANM2 scratch reserve: status=alloc-failed result=0x%08x "
            "payload=%u block=%u route=%s",
            (unsigned)uid, (unsigned)ISAAC_VITA_ANM2_PAYLOAD_BYTES,
            (unsigned)ISAAC_VITA_ANM2_MEMBLOCK_BYTES,
            route_guest_heap ? "guest-heap" : "bad-alloc");
        if (route_guest_heap) {
            s_state.guest_heap_fallback = 1U;
            s_state.guest_stack_floor = guest_stack_floor;
            s_state.guest_stack_ceiling = guest_stack_ceiling;
            s_state.acquired_count = 0U;
            s_state.freed_count = 0U;
            s_state.peak_live = 0U;
            return ISAAC_VITA_ANM2_SCRATCH_ROUTE_GUEST_HEAP;
        }
        return 0;
    }

    get_result = sceKernelGetMemBlockBase(uid, &base);
    if (get_result >= 0 && anm2_scratch_base_is_valid(base) &&
        anm2_scratch_range_is_safe(
            (uintptr_t)base, guest_stack_floor, guest_stack_ceiling)) {
        s_state.uid = uid;
        s_state.guest_stack_floor = guest_stack_floor;
        s_state.guest_stack_ceiling = guest_stack_ceiling;
        atomic_store_explicit(
            &s_published_base, (uintptr_t)base, memory_order_release);
        isaac_vita_log(
            "ANM2 scratch reserve: status=ready uid=0x%08x base=%p "
            "end=0x%08x stack=0x%08x..0x%08x image=0x%08x..0x%08x "
            "payload=%u block=%u page=%u vita3k_alignment=%u",
            (unsigned)uid, base,
            (unsigned)((uintptr_t)base + ISAAC_VITA_ANM2_MEMBLOCK_BYTES),
            (unsigned)guest_stack_floor, (unsigned)guest_stack_ceiling,
            (unsigned)ISAAC_VITA_ANM2_GUEST_IMAGE_BASE,
            (unsigned)ISAAC_VITA_ANM2_GUEST_IMAGE_END,
            (unsigned)ISAAC_VITA_ANM2_PAYLOAD_BYTES,
            (unsigned)ISAAC_VITA_ANM2_MEMBLOCK_BYTES,
            (unsigned)ISAAC_VITA_ANM2_MEMBLOCK_PAGE_BYTES,
            (unsigned)ISAAC_VITA_ANM2_VITA3K_ALIGNMENT);
        return 1;
    }

    status = get_result < 0 ? "get-base-failed" :
        !anm2_scratch_base_is_valid(base) ? "invalid-base" :
        "range-overlap";
    free_result = sceKernelFreeMemBlock(uid);
    isaac_vita_log(
        "ANM2 scratch reserve: status=%s uid=0x%08x get=0x%08x "
        "base=%p end=0x%08x stack=0x%08x..0x%08x "
        "image=0x%08x..0x%08x rollback=0x%08x",
        status, (unsigned)uid, (unsigned)get_result, base,
        base ? (unsigned)((uintptr_t)base +
                          ISAAC_VITA_ANM2_MEMBLOCK_BYTES) : 0U,
        (unsigned)guest_stack_floor, (unsigned)guest_stack_ceiling,
        (unsigned)ISAAC_VITA_ANM2_GUEST_IMAGE_BASE,
        (unsigned)ISAAC_VITA_ANM2_GUEST_IMAGE_END,
        (unsigned)free_result);
    if (free_result >= 0)
        return 0;

    /* The block may still be live but no safe segment base was published.
     * Never allocate a replacement or fall back to newlib after that leak. */
    s_state.uid = uid;
    s_state.poisoned = 1U;
    return anm2_scratch_reject(
        decision, (uint32_t)free_result,
        "ANM2 scratch memblock rollback failed");
}

int isaac_vita_anm2_scratch_malloc(
    uint32_t owner_return_rva, size_t size,
    uint32_t guest_stack_floor, uint32_t guest_stack_ceiling,
    isaac_vita_anm2_scratch_decision *decision)
{
    uintptr_t base;
    int reserve_result;
    int index;

    anm2_scratch_clear_decision(decision);
    index = anm2_scratch_find_owner(owner_return_rva);
    if (index < 0)
        return ISAAC_VITA_ANM2_SCRATCH_NOT_HANDLED;
    if (size != (size_t)s_sites[index].size)
        return anm2_scratch_reject(
            decision, (uint32_t)size,
            "ANM2 scratch owner requested the wrong size");
    if (guest_stack_floor >= guest_stack_ceiling)
        return anm2_scratch_reject(
            decision, guest_stack_floor,
            "ANM2 scratch guest stack range is invalid");
    if (!decision)
        return ISAAC_VITA_ANM2_SCRATCH_REJECTED;
    if (!anm2_scratch_try_lock())
        return anm2_scratch_reject(
            decision, owner_return_rva,
            "ANM2 scratch concurrent/reentrant access");

    if (s_state.poisoned) {
        anm2_scratch_unlock();
        return anm2_scratch_reject(
            decision, owner_return_rva,
            "ANM2 scratch memblock is poisoned");
    }
    if (s_state.guest_heap_fallback)
        return anm2_scratch_route_guest_heap_locked(
            index, owner_return_rva, guest_stack_floor,
            guest_stack_ceiling, decision);
    if (s_state.live[index]) {
        anm2_scratch_unlock();
        return anm2_scratch_reject(
            decision, owner_return_rva,
            "ANM2 scratch pool overlap/reentrancy");
    }
    base = atomic_load_explicit(
        &s_published_base, memory_order_relaxed);
    if (base &&
        (s_state.guest_stack_floor != guest_stack_floor ||
         s_state.guest_stack_ceiling != guest_stack_ceiling)) {
        anm2_scratch_unlock();
        return anm2_scratch_reject(
            decision, guest_stack_floor,
            "ANM2 scratch guest stack owner changed");
    }

    if (s_state.live_count == 0U) {
        if (index != 0) {
            anm2_scratch_unlock();
            return anm2_scratch_reject(
                decision, owner_return_rva,
                "ANM2 scratch session did not start at AA5D");
        }
        reserve_result = anm2_scratch_reserve_locked(
            guest_stack_floor, guest_stack_ceiling, decision);
        if (reserve_result ==
                ISAAC_VITA_ANM2_SCRATCH_ROUTE_GUEST_HEAP)
            return anm2_scratch_route_guest_heap_locked(
                index, owner_return_rva, guest_stack_floor,
                guest_stack_ceiling, decision);
        if (reserve_result <= 0) {
            anm2_scratch_unlock();
            return reserve_result < 0
                ? ISAAC_VITA_ANM2_SCRATCH_REJECTED
                : ISAAC_VITA_ANM2_SCRATCH_HANDLED;
        }
        memset(s_state.live, 0, sizeof s_state.live);
        s_state.acquired_count = 0U;
        s_state.freed_count = 0U;
        s_state.peak_live = 0U;
        ++s_state.session_id;
        if (!s_state.session_id)
            ++s_state.session_id;
        if (anm2_scratch_should_log_session(s_state.session_id))
            isaac_vita_log(
                "ANM2 scratch session: id=%u first=0x%08x segments=%u "
                "payload=%u",
                (unsigned)s_state.session_id, (unsigned)owner_return_rva,
                (unsigned)ISAAC_VITA_ANM2_SEGMENT_COUNT,
                (unsigned)ISAAC_VITA_ANM2_PAYLOAD_BYTES);
    }

    if ((uint32_t)index != s_state.acquired_count) {
        anm2_scratch_unlock();
        return anm2_scratch_reject(
            decision, owner_return_rva,
            "ANM2 scratch allocation order drifted");
    }

    base = atomic_load_explicit(&s_published_base, memory_order_relaxed);
    if (!base) {
        s_state.poisoned = 1U;
        anm2_scratch_unlock();
        return anm2_scratch_reject(
            decision, owner_return_rva,
            "ANM2 scratch lost its reserved base");
    }
    s_state.live[index] = 1U;
    ++s_state.live_count;
    ++s_state.acquired_count;
    if (s_state.live_count > s_state.peak_live)
        s_state.peak_live = s_state.live_count;
    decision->pointer = (void *)(base + s_sites[index].offset);
    anm2_scratch_unlock();
    return ISAAC_VITA_ANM2_SCRATCH_HANDLED;
}

int isaac_vita_anm2_scratch_free(
    void *pointer, isaac_vita_anm2_scratch_decision *decision)
{
    uintptr_t published;
    uintptr_t value = (uintptr_t)pointer;
    int index;

    anm2_scratch_clear_decision(decision);
    if (!pointer)
        return ISAAC_VITA_ANM2_SCRATCH_NOT_HANDLED;
    published = atomic_load_explicit(
        &s_published_base, memory_order_acquire);
    if (!published)
        return ISAAC_VITA_ANM2_SCRATCH_NOT_HANDLED;
    index = anm2_scratch_find_pointer(published, value);
    if (index < 0)
        return ISAAC_VITA_ANM2_SCRATCH_NOT_HANDLED;
    if (!decision)
        return ISAAC_VITA_ANM2_SCRATCH_REJECTED;
    if (!anm2_scratch_try_lock())
        return anm2_scratch_reject(
            decision, (uint32_t)value,
            "ANM2 scratch concurrent/reentrant free");
    if (published != atomic_load_explicit(
                         &s_published_base, memory_order_relaxed)) {
        s_state.poisoned = 1U;
        anm2_scratch_unlock();
        return anm2_scratch_reject(
            decision, (uint32_t)value,
            "ANM2 scratch base changed while freeing");
    }
    if (!s_state.live[index]) {
        anm2_scratch_unlock();
        return anm2_scratch_reject(
            decision, (uint32_t)value,
            "ANM2 scratch double free");
    }

    s_state.live[index] = 0U;
    --s_state.live_count;
    ++s_state.freed_count;
    if (s_state.live_count == 0U) {
        int complete =
            s_state.acquired_count == ISAAC_VITA_ANM2_SEGMENT_COUNT &&
            s_state.freed_count == s_state.acquired_count;

        if (!complete || anm2_scratch_should_log_session(s_state.session_id))
            isaac_vita_log(
                "ANM2 scratch free summary: session=%u acquired=%u freed=%u "
                "peak=%u complete=%s",
                (unsigned)s_state.session_id,
                (unsigned)s_state.acquired_count,
                (unsigned)s_state.freed_count,
                (unsigned)s_state.peak_live,
                complete ? "yes" : "no");
    }
    anm2_scratch_unlock();
    return ISAAC_VITA_ANM2_SCRATCH_HANDLED;
}

#ifdef ISAAC_VITA_ANM2_SCRATCH_ORACLE
int isaac_vita_anm2_scratch_oracle_snapshot(
    isaac_vita_anm2_scratch_snapshot *snapshot)
{
    if (!snapshot || !anm2_scratch_try_lock())
        return 0;
    snapshot->base = atomic_load_explicit(
        &s_published_base, memory_order_relaxed);
    snapshot->uid = (int32_t)s_state.uid;
    snapshot->poisoned = s_state.poisoned;
    snapshot->guest_heap_fallback = s_state.guest_heap_fallback;
    snapshot->session_id = s_state.session_id;
    snapshot->live_count = s_state.live_count;
    snapshot->acquired_count = s_state.acquired_count;
    snapshot->freed_count = s_state.freed_count;
    snapshot->peak_live = s_state.peak_live;
    anm2_scratch_unlock();
    return 1;
}

int isaac_vita_anm2_scratch_oracle_reset(void)
{
    int result = 0;

    if (!anm2_scratch_try_lock())
        return INT_MIN;
    if (s_state.uid >= 0)
        result = sceKernelFreeMemBlock(s_state.uid);
    memset(&s_state, 0, sizeof s_state);
    s_state.uid = -1;
    atomic_store_explicit(
        &s_published_base, (uintptr_t)0U, memory_order_release);
    anm2_scratch_unlock();
    return result;
}
#endif
