/* One persistent USER_RW block for transient KAGE texel backing.
 *
 * The frozen ImagePng and ProceduralImageBase paths upload this backing
 * synchronously with glTexImage2D and free it before the allocating routine
 * returns.  ImagePng also frees it on its libpng-longjmp cleanup path.  We
 * retain the native memblock until process exit but expose at most one logical
 * lease.  A concurrent/reentrant request falls back to the ordinary guest heap
 * and can never receive the already-live scratch base. */
#include <limits.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <psp2/kernel/sysmem.h>

#include "host_vita_texel_scratch.h"
#include "kage_vita_texture_memory.h"
#include "platform.h"
#if defined(ISAAC_VITA_PNG_PREMULTIPLY_NATIVE)
#include "host_vita_png_premultiply.h"
#endif

#if !defined(ISAAC_VITA_TEXEL_SCRATCH_ORACLE) && UINTPTR_MAX != UINT32_MAX
#error texel scratch requires a 32-bit identity-mapped Vita address space
#endif

typedef struct texel_scratch_state {
    SceUID uid;
    uint32_t poisoned;
    uint32_t guest_stack_floor;
    uint32_t guest_stack_ceiling;
    uint32_t live;
    size_t live_size;
    size_t high_water;
    uint32_t acquired_count;
    uint32_t freed_count;
    uint32_t busy_fallback_count;
    uint32_t oversize_fallback_count;
#if defined(ISAAC_VITA_PNG_PREMULTIPLY_NATIVE) || \
    defined(ISAAC_VITA_PNG_TEXEL_INIT_ELISION) || defined(ISAAC_VITA_NATIVE_PNG_ROW_BATCH)
    uint32_t owner_return_rva;
#endif
} texel_scratch_state;

_Static_assert(KAGE_VITA_TEXEL_PNG_LOADER_RETURN ==
                   KAGE_VITA_TEXEL_LOADER_RETURN_1,
               "PNG loader owner alias drifted");
_Static_assert(KAGE_VITA_TEXEL_LOADER_RETURN_4 !=
                   KAGE_VITA_TEXEL_PNG_LOADER_RETURN &&
               KAGE_VITA_TEXEL_LOADER_RETURN_5 !=
                   KAGE_VITA_TEXEL_PNG_LOADER_RETURN &&
               KAGE_VITA_TEXEL_LOADER_RETURN_4 !=
                   KAGE_VITA_TEXEL_LOADER_RETURN_5,
               "transient texel loader owners must stay distinct");
_Static_assert(KAGE_VITA_TEXEL_MEMBLOCK_8M_BYTES ==
                   KAGE_VITA_TEXEL_OOM_REQUEST +
                       KAGE_VITA_TEXEL_MEMBLOCK_PAGE_BYTES,
               "8 MiB texel scratch padding contract drifted");
_Static_assert((KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES &
                (KAGE_VITA_TEXEL_MEMBLOCK_PAGE_BYTES - 1U)) == 0U,
               "texel scratch capacity must be page aligned");
_Static_assert(KAGE_VITA_TEXEL_SCRATCH_REQUEST_MAX_BYTES <=
                   KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES,
               "texel scratch request limit exceeds its memblock");
_Static_assert(KAGE_VITA_TEXEL_SCRATCH_REQUEST_MAX_BYTES >=
                   KAGE_VITA_TEXEL_ALIGN8_PROBE_REQUEST,
               "texel scratch no longer fits the measured controls PNG");
_Static_assert(KAGE_VITA_TEXEL_LINK_END <
                   KAGE_VITA_TEXEL_HEAP_RETAINED_END,
               "texel scratch frozen heap interval is invalid");
_Static_assert(KAGE_VITA_TEXEL_GUEST_IMAGE_BASE <
                   KAGE_VITA_TEXEL_GUEST_IMAGE_END,
               "texel scratch frozen image interval is invalid");

static texel_scratch_state s_state = { .uid = -1 };
static atomic_uintptr_t s_published_base = ATOMIC_VAR_INIT((uintptr_t)0U);
static atomic_flag s_state_lock = ATOMIC_FLAG_INIT;

static int texel_scratch_owner_is_transient(uint32_t owner_return_rva)
{
    switch (owner_return_rva) {
    case KAGE_VITA_TEXEL_PNG_LOADER_RETURN:
    case KAGE_VITA_TEXEL_LOADER_RETURN_4:
    case KAGE_VITA_TEXEL_LOADER_RETURN_5:
        return 1;
    default:
        return 0;
    }
}

static int texel_scratch_try_lock(void)
{
    return !atomic_flag_test_and_set_explicit(
        &s_state_lock, memory_order_acquire);
}

static void texel_scratch_unlock(void)
{
    atomic_flag_clear_explicit(&s_state_lock, memory_order_release);
}

static void texel_scratch_clear_decision(
    isaac_vita_texel_scratch_decision *decision)
{
    if (!decision)
        return;
    decision->pointer = NULL;
    decision->fault_value = 0U;
    decision->fault = NULL;
}

static int texel_scratch_reject(
    isaac_vita_texel_scratch_decision *decision, uint32_t value,
    const char *fault)
{
    if (decision) {
        decision->pointer = NULL;
        decision->fault_value = value;
        decision->fault = fault;
    }
    return ISAAC_VITA_TEXEL_SCRATCH_REJECTED;
}

static int texel_scratch_ranges_overlap(uintptr_t first_start,
                                        uintptr_t first_end,
                                        uintptr_t second_start,
                                        uintptr_t second_end)
{
    return first_start < second_end && second_start < first_end;
}

static int texel_scratch_base_is_valid(const void *base)
{
    uintptr_t value = (uintptr_t)base;

    if (!value ||
        (value & (KAGE_VITA_TEXEL_MEMBLOCK_PAGE_BYTES - 1U)) != 0U ||
        value > UINTPTR_MAX - KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES)
        return 0;
#ifndef ISAAC_VITA_TEXEL_SCRATCH_ORACLE
    if (value > (uintptr_t)UINT32_MAX -
                    KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES)
        return 0;
#endif
    return 1;
}

static int texel_scratch_range_is_safe(uintptr_t base,
                                       uint32_t guest_stack_floor,
                                       uint32_t guest_stack_ceiling)
{
    uintptr_t end = base + KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES;

    if (guest_stack_floor >= guest_stack_ceiling)
        return 0;
    if (texel_scratch_ranges_overlap(
            base, end, KAGE_VITA_TEXEL_LINK_END,
            KAGE_VITA_TEXEL_HEAP_RETAINED_END))
        return 0;
    if (texel_scratch_ranges_overlap(
            base, end, KAGE_VITA_TEXEL_GUEST_IMAGE_BASE,
            KAGE_VITA_TEXEL_GUEST_IMAGE_END))
        return 0;
    if (texel_scratch_ranges_overlap(
            base, end, guest_stack_floor, guest_stack_ceiling))
        return 0;
    return 1;
}

/* Return 1 when ready, 0 for a clean native allocation failure, and -1 when
 * rollback failed and the seam can no longer make a safe decision. */
static int texel_scratch_reserve_locked(
    uint32_t guest_stack_floor, uint32_t guest_stack_ceiling,
    isaac_vita_texel_scratch_decision *decision)
{
    SceUID uid;
    void *base = NULL;
    int get_result;
    int free_result;
    const char *status;

    if (s_state.poisoned)
        return texel_scratch_reject(
            decision, 0U, "texel scratch memblock is poisoned");
    if (atomic_load_explicit(&s_published_base, memory_order_relaxed))
        return 1;

    uid = sceKernelAllocMemBlock(
        "isaac_texel_scratch", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
        KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES, NULL);
    if (uid < 0) {
        isaac_vita_log(
            "texel scratch reserve: status=alloc-failed result=0x%08x "
            "capacity=%u",
            (unsigned)uid,
            (unsigned)KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES);
        return 0;
    }

    get_result = sceKernelGetMemBlockBase(uid, &base);
    if (get_result >= 0 && texel_scratch_base_is_valid(base) &&
        texel_scratch_range_is_safe(
            (uintptr_t)base, guest_stack_floor, guest_stack_ceiling)) {
        s_state.uid = uid;
        s_state.guest_stack_floor = guest_stack_floor;
        s_state.guest_stack_ceiling = guest_stack_ceiling;
        atomic_store_explicit(
            &s_published_base, (uintptr_t)base, memory_order_release);
        isaac_vita_log(
            "texel scratch reserve: status=ready uid=0x%08x base=%p "
            "end=0x%08x capacity=%u stack=0x%08x..0x%08x",
            (unsigned)uid, base,
            (unsigned)((uintptr_t)base +
                       KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES),
            (unsigned)KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES,
            (unsigned)guest_stack_floor, (unsigned)guest_stack_ceiling);
        return 1;
    }

    status = get_result < 0 ? "get-base-failed" :
        !texel_scratch_base_is_valid(base) ? "invalid-base" :
        "range-overlap";
    free_result = sceKernelFreeMemBlock(uid);
    isaac_vita_log(
        "texel scratch reserve: status=%s uid=0x%08x get=0x%08x "
        "base=%p rollback=0x%08x",
        status, (unsigned)uid, (unsigned)get_result, base,
        (unsigned)free_result);
    if (free_result >= 0)
        return 0;

    s_state.uid = uid;
    s_state.poisoned = 1U;
    return texel_scratch_reject(
        decision, (uint32_t)free_result,
        "texel scratch memblock rollback failed");
}

int isaac_vita_texel_scratch_malloc(
    uint32_t owner_return_rva, size_t size,
    uint32_t guest_stack_floor, uint32_t guest_stack_ceiling,
    isaac_vita_texel_scratch_decision *decision)
{
    uintptr_t base;
    int reserve_result;

    texel_scratch_clear_decision(decision);
    if (!texel_scratch_owner_is_transient(owner_return_rva))
        return ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED;
    if (!decision)
        return ISAAC_VITA_TEXEL_SCRATCH_REJECTED;
    if (!size || size > KAGE_VITA_TEXEL_SCRATCH_REQUEST_MAX_BYTES) {
        if (texel_scratch_try_lock()) {
            ++s_state.oversize_fallback_count;
            texel_scratch_unlock();
        }
        isaac_vita_log(
            "texel scratch fallback: reason=request-range request=%u max=%u",
            (unsigned)size,
            (unsigned)KAGE_VITA_TEXEL_SCRATCH_REQUEST_MAX_BYTES);
        return ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED;
    }
    if (guest_stack_floor >= guest_stack_ceiling)
        return texel_scratch_reject(
            decision, guest_stack_floor,
            "texel scratch guest stack range is invalid");
    if (!texel_scratch_try_lock()) {
        isaac_vita_log(
            "texel scratch fallback: reason=busy request=%u owner=0x%08x",
            (unsigned)size, (unsigned)owner_return_rva);
        return ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED;
    }

    if (s_state.poisoned) {
        texel_scratch_unlock();
        return texel_scratch_reject(
            decision, owner_return_rva,
            "texel scratch memblock is poisoned");
    }
    if (s_state.live) {
        ++s_state.busy_fallback_count;
        texel_scratch_unlock();
        isaac_vita_log(
            "texel scratch fallback: reason=live request=%u owner=0x%08x",
            (unsigned)size, (unsigned)owner_return_rva);
        return ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED;
    }

    base = atomic_load_explicit(&s_published_base, memory_order_relaxed);
    if (base && !texel_scratch_range_is_safe(
                    base, guest_stack_floor, guest_stack_ceiling)) {
        texel_scratch_unlock();
        isaac_vita_log(
            "texel scratch fallback: reason=stack-overlap request=%u "
            "stack=0x%08x..0x%08x",
            (unsigned)size, (unsigned)guest_stack_floor,
            (unsigned)guest_stack_ceiling);
        return ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED;
    }
    reserve_result = texel_scratch_reserve_locked(
        guest_stack_floor, guest_stack_ceiling, decision);
    if (reserve_result <= 0) {
        texel_scratch_unlock();
        return reserve_result < 0
            ? ISAAC_VITA_TEXEL_SCRATCH_REJECTED
            : ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED;
    }

    base = atomic_load_explicit(&s_published_base, memory_order_relaxed);
    if (!base) {
        s_state.poisoned = 1U;
        texel_scratch_unlock();
        return texel_scratch_reject(
            decision, owner_return_rva,
            "texel scratch lost its reserved base");
    }
    s_state.live = 1U;
    s_state.live_size = size;
#if defined(ISAAC_VITA_PNG_PREMULTIPLY_NATIVE) || \
    defined(ISAAC_VITA_PNG_TEXEL_INIT_ELISION) || defined(ISAAC_VITA_NATIVE_PNG_ROW_BATCH)
    s_state.owner_return_rva = owner_return_rva;
#endif
    if (size > s_state.high_water)
        s_state.high_water = size;
    ++s_state.acquired_count;
    decision->pointer = (void *)base;
    texel_scratch_unlock();
    return ISAAC_VITA_TEXEL_SCRATCH_HANDLED;
}

int isaac_vita_texel_scratch_free(
    void *pointer, isaac_vita_texel_scratch_decision *decision)
{
    uintptr_t base;
    uintptr_t value = (uintptr_t)pointer;

    texel_scratch_clear_decision(decision);
    if (!pointer)
        return ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED;
    base = atomic_load_explicit(&s_published_base, memory_order_acquire);
    if (!base)
        return ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED;
    if (value < base ||
        value >= base + KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES)
        return ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED;
    if (!decision)
        return ISAAC_VITA_TEXEL_SCRATCH_REJECTED;
    if (value != base)
        return texel_scratch_reject(
            decision, (uint32_t)value,
            "free received an interior texel scratch pointer");
    if (!texel_scratch_try_lock())
        return texel_scratch_reject(
            decision, (uint32_t)value,
            "texel scratch concurrent/reentrant free");
    if (base != atomic_load_explicit(
                    &s_published_base, memory_order_relaxed)) {
        s_state.poisoned = 1U;
        texel_scratch_unlock();
        return texel_scratch_reject(
            decision, (uint32_t)value,
            "texel scratch base changed while freeing");
    }
    if (!s_state.live) {
        texel_scratch_unlock();
        return texel_scratch_reject(
            decision, (uint32_t)value, "texel scratch double/stale free");
    }

    s_state.live = 0U;
    s_state.live_size = 0U;
#if defined(ISAAC_VITA_PNG_PREMULTIPLY_NATIVE) || \
    defined(ISAAC_VITA_PNG_TEXEL_INIT_ELISION) || defined(ISAAC_VITA_NATIVE_PNG_ROW_BATCH)
    s_state.owner_return_rva = 0U;
#endif
    ++s_state.freed_count;
    if (s_state.freed_count == 1U ||
        (s_state.freed_count & 255U) == 0U) {
        isaac_vita_log(
            "texel scratch activity: acquired=%u freed=%u high_water=%u "
            "busy_fallback=%u oversize_fallback=%u",
            (unsigned)s_state.acquired_count,
            (unsigned)s_state.freed_count,
            (unsigned)s_state.high_water,
            (unsigned)s_state.busy_fallback_count,
            (unsigned)s_state.oversize_fallback_count);
    }
    texel_scratch_unlock();
    return ISAAC_VITA_TEXEL_SCRATCH_HANDLED;
}

#if defined(ISAAC_VITA_PNG_PREMULTIPLY_NATIVE)
int isaac_vita_texel_scratch_png_premultiply(
    const IsaacVitaPngPremultiply *p)
{
    uint64_t end;
    int result = 0;
    if (!p || !p->base || !p->table || !p->width || !p->rows ||
        p->stride < (uint64_t)p->width * 4u ||
        (uint64_t)p->base + p->bytes > UINT32_MAX ||
        (uintptr_t)p->table > UINTPTR_MAX - 65536u ||
        ((uint64_t)p->base < (uint64_t)(uintptr_t)p->table + 65536u &&
         (uint64_t)(uintptr_t)p->table < (uint64_t)p->base + p->bytes))
        return 0;
    end = (uint64_t)(p->rows - 1u) * p->stride + (uint64_t)p->width * 4u;
    if (end > p->bytes || !texel_scratch_try_lock())
        return 0;
    if (!s_state.poisoned && s_state.live &&
        s_state.owner_return_rva == KAGE_VITA_TEXEL_PNG_LOADER_RETURN &&
        s_state.live_size == p->bytes &&
        atomic_load_explicit(&s_published_base, memory_order_relaxed) == p->base) {
        isaac_vita_png_premultiply_rows(p);
        result = 1;
    }
    texel_scratch_unlock();
    return result;
}
#endif

#if defined(ISAAC_VITA_PNG_TEXEL_INIT_ELISION)
int isaac_vita_texel_scratch_png_exact(uint32_t base, uint32_t bytes)
{
    int result;
    if (!base || !bytes || !texel_scratch_try_lock())
        return 0;
    result = !s_state.poisoned && s_state.live &&
        s_state.owner_return_rva == KAGE_VITA_TEXEL_PNG_LOADER_RETURN &&
        s_state.live_size == bytes &&
        atomic_load_explicit(&s_published_base, memory_order_relaxed) == base;
    texel_scratch_unlock();
    return result;
}
#endif

#if defined(ISAAC_VITA_NATIVE_PNG_ROW_BATCH)
int isaac_vita_texel_scratch_png_middle_rows(uint32_t base, uint32_t stride,
    uint32_t rowbytes, uint32_t height, const uint8_t *filtered_rows)
{
    uint64_t end;
    int result = 0;
    if (!base || !filtered_rows || !rowbytes || height < 3u ||
        stride < rowbytes)
        return 0;
    end = (uint64_t)(height - 1u) * stride + rowbytes;
    if ((uint64_t)base + end > UINT32_MAX || !texel_scratch_try_lock())
        return 0;
    if (!s_state.poisoned && s_state.live &&
        s_state.owner_return_rva == KAGE_VITA_TEXEL_PNG_LOADER_RETURN &&
        end <= s_state.live_size &&
        atomic_load_explicit(&s_published_base, memory_order_relaxed) == base &&
        !((uintptr_t)filtered_rows < (uint64_t)base + end &&
          base < (uint64_t)(uintptr_t)filtered_rows +
              (uint64_t)height * (rowbytes + 1u))) {
        uint8_t *destination = (uint8_t *)(uintptr_t)base + stride;
        const uint8_t *source = filtered_rows + rowbytes + 2u;
        const uint8_t *stop = (uint8_t *)(uintptr_t)base +
            (size_t)(height - 1u) * stride;
        while (destination != stop) {
            memcpy(destination, source, rowbytes);
            destination += stride;
            source += rowbytes + 1u;
        }
        result = 1;
    }
    texel_scratch_unlock();
    return result;
}
#endif

int isaac_vita_texel_scratch_realloc(
    void *pointer, size_t size,
    isaac_vita_texel_scratch_decision *decision)
{
    uintptr_t base;
    uintptr_t value = (uintptr_t)pointer;

    texel_scratch_clear_decision(decision);
    if (!pointer)
        return ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED;
    base = atomic_load_explicit(&s_published_base, memory_order_acquire);
    if (!base)
        return ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED;
    if (value < base ||
        value >= base + KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES)
        return ISAAC_VITA_TEXEL_SCRATCH_NOT_HANDLED;
    if (!decision)
        return ISAAC_VITA_TEXEL_SCRATCH_REJECTED;
    if (value != base)
        return texel_scratch_reject(
            decision, (uint32_t)value,
            "realloc received an interior texel scratch pointer");
    if (!texel_scratch_try_lock())
        return texel_scratch_reject(
            decision, (uint32_t)value,
            "texel scratch concurrent/reentrant realloc");
    if (!s_state.live) {
        texel_scratch_unlock();
        return texel_scratch_reject(
            decision, (uint32_t)value,
            "realloc received a stale texel scratch pointer");
    }

    if (!size) {
        s_state.live = 0U;
        s_state.live_size = 0U;
        ++s_state.freed_count;
        decision->pointer = NULL;
    }
    else if (size <= KAGE_VITA_TEXEL_SCRATCH_REQUEST_MAX_BYTES) {
        s_state.live_size = size;
        if (size > s_state.high_water)
            s_state.high_water = size;
        decision->pointer = (void *)base;
    }
    else {
        /* Standard realloc failure: retain the old live allocation. */
        decision->pointer = NULL;
    }
    texel_scratch_unlock();
    return ISAAC_VITA_TEXEL_SCRATCH_HANDLED;
}

#ifdef ISAAC_VITA_TEXEL_SCRATCH_ORACLE
#if defined(ISAAC_VITA_PNG_PREMULTIPLY_NATIVE) || \
    defined(ISAAC_VITA_PNG_TEXEL_INIT_ELISION)
int isaac_vita_texel_scratch_oracle_hold_lock(void)
{
    return texel_scratch_try_lock();
}

void isaac_vita_texel_scratch_oracle_drop_lock(void)
{
    texel_scratch_unlock();
}
#endif
int isaac_vita_texel_scratch_oracle_snapshot(
    isaac_vita_texel_scratch_snapshot *snapshot)
{
    if (!snapshot || !texel_scratch_try_lock())
        return 0;
    snapshot->base = atomic_load_explicit(
        &s_published_base, memory_order_relaxed);
    snapshot->uid = (int32_t)s_state.uid;
    snapshot->poisoned = s_state.poisoned;
    snapshot->live = s_state.live;
    snapshot->live_size = s_state.live_size;
    snapshot->high_water = s_state.high_water;
    snapshot->acquired_count = s_state.acquired_count;
    snapshot->freed_count = s_state.freed_count;
    snapshot->busy_fallback_count = s_state.busy_fallback_count;
    snapshot->oversize_fallback_count = s_state.oversize_fallback_count;
    texel_scratch_unlock();
    return 1;
}

int isaac_vita_texel_scratch_oracle_reset(void)
{
    int result = 0;

    if (!texel_scratch_try_lock())
        return INT_MIN;
    if (s_state.uid >= 0)
        result = sceKernelFreeMemBlock(s_state.uid);
    memset(&s_state, 0, sizeof s_state);
    s_state.uid = -1;
    atomic_store_explicit(
        &s_published_base, (uintptr_t)0U, memory_order_release);
    texel_scratch_unlock();
    return result;
}
#endif
