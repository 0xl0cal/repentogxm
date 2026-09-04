#include "kage_vita_fx_rollback.h"

#include <stddef.h>
#include <stdint.h>

#if defined(ISAAC_VITA_FXLAYERS_NULL_ROLLBACK) && \
    !defined(ISAAC_VITA_HEAP_RANGE_LEASE)
#define ISAAC_VITA_HEAP_RANGE_LEASE 1
#endif

#include "host_vita_heap.h"

#define KAGE_VITA_FX_ROOT_OUTER 0x004e8330U
#define KAGE_VITA_FX_ROOT_INNER 0x004e85b3U
#define KAGE_VITA_FX_FRAME_END  0x0001013cU
#define KAGE_VITA_FX_FRAME_OWNER 0x00010134U
#define KAGE_VITA_FX_VECTOR_BEGIN 0x18U
#define KAGE_VITA_FX_VECTOR_END   0x1cU
#define KAGE_VITA_FX_VECTOR_CAP   0x20U
#define KAGE_VITA_FX_RECORD_SIZE  0x4cU
#define KAGE_VITA_FX_RECORD_IMAGE 0x04U
#define KAGE_VITA_FX_RECORD_TYPE  0x2cU
#define KAGE_VITA_FX_LIGHTING_TYPE 4U
#define KAGE_VITA_FX_BIG_ALLOCATION 0x1000U
#define KAGE_VITA_FX_BIG_OVERHEAD   0x23U
#define KAGE_VITA_FX_BIG_ALIGNMENT  0x20U

#if defined(ISAAC_VITA_FXLAYERS_NULL_ROLLBACK) && \
    !defined(KAGE_VITA_FX_ROLLBACK_READ32)
static uint32_t kage_vita_fx_read32(uint32_t address)
{
    return *(const uint32_t *)(uintptr_t)address;
}
#define KAGE_VITA_FX_ROLLBACK_READ32(address) \
    kage_vita_fx_read32((address))
#endif

#ifndef KAGE_VITA_FX_ROLLBACK_HEAP_LEASE
#define KAGE_VITA_FX_ROLLBACK_HEAP_LEASE(address, size, base_out) \
    isaac_vita_guest_heap_lease_containing( \
        (const void *)(uintptr_t)(address), (size), (base_out))
#endif

#ifndef KAGE_VITA_FX_ROLLBACK_HEAP_RELEASE
#define KAGE_VITA_FX_ROLLBACK_HEAP_RELEASE(lease) \
    isaac_vita_guest_heap_lease_release((lease))
#endif

#ifndef KAGE_VITA_FX_ROLLBACK_LOG
extern void isaac_vita_log(const char *format, ...);
#define KAGE_VITA_FX_ROLLBACK_LOG isaac_vita_log
#endif

int kage_vita_fx_rollback_prepare(
    const CPU *cpu, uint32_t root_rva, uint32_t *owner_out,
    uint32_t *last_out, uint32_t *end_out)
{
#if !defined(ISAAC_VITA_FXLAYERS_NULL_ROLLBACK)
    (void)cpu;
    (void)root_rva;
    (void)owner_out;
    (void)last_out;
    (void)end_out;
    return 0;
#else
    uint32_t frame;
    uint32_t saved_end;
    uint32_t owner;
    uint32_t begin;
    uint32_t end;
    uint32_t cap;
    uint32_t last;
    uint32_t span;
    uint32_t lease_range;
    size_t lease_size;
    uintptr_t allocation_base_native = 0U;
    uint32_t allocation_base;
    uint32_t lease;

    if (!cpu || !owner_out || !last_out || !end_out)
        return -1;
    if (root_rva != KAGE_VITA_FX_ROOT_OUTER &&
        root_rva != KAGE_VITA_FX_ROOT_INNER)
        return -1;
    if (cpu->ebp < KAGE_VITA_FX_FRAME_END)
        return -1;
    frame = cpu->ebp - KAGE_VITA_FX_FRAME_END;
    if (!guest_stack_contains(cpu, frame, 0x0cU))
        return -1;

    /* This is not a general guest-pointer validator.  The generated seam
     * calls it only at the two build-test-pinned translated failure edges,
     * while FXLayers owns a live frame and vector owner.  Within that trusted
     * edge, the checks below fail closed before any record/destructor action. */
    saved_end = KAGE_VITA_FX_ROLLBACK_READ32(frame);
    owner = KAGE_VITA_FX_ROLLBACK_READ32(
        cpu->ebp - KAGE_VITA_FX_FRAME_OWNER);
    if (!owner || (owner & 3U) != 0U ||
        owner > UINT32_MAX - KAGE_VITA_FX_VECTOR_CAP)
        return -1;

    begin = KAGE_VITA_FX_ROLLBACK_READ32(
        owner + KAGE_VITA_FX_VECTOR_BEGIN);
    end = KAGE_VITA_FX_ROLLBACK_READ32(
        owner + KAGE_VITA_FX_VECTOR_END);
    cap = KAGE_VITA_FX_ROLLBACK_READ32(
        owner + KAGE_VITA_FX_VECTOR_CAP);

    /* Do not inspect or destroy a record until the whole vector is valid. */
    if (!begin || ((begin | end | cap) & 3U) != 0U ||
        begin > end || end > cap ||
        end - begin < KAGE_VITA_FX_RECORD_SIZE ||
        (end - begin) % KAGE_VITA_FX_RECORD_SIZE != 0U ||
        (cap - begin) % KAGE_VITA_FX_RECORD_SIZE != 0U ||
        saved_end != end || cpu->esi != end ||
        cap - begin > UINT32_MAX - 4U)
        return -1;

    span = cap - begin;
    lease_range = begin;
    lease_size = (size_t)span;
    if (span >= KAGE_VITA_FX_BIG_ALLOCATION) {
        if (begin < 4U)
            return -1;
        lease_range = begin - 4U;
        lease_size += 4U;
    }
    lease = KAGE_VITA_FX_ROLLBACK_HEAP_LEASE(
        lease_range, lease_size, &allocation_base_native);
    if (!lease)
        return -1;
    if (allocation_base_native > UINT32_MAX)
        goto reject_lease;
    allocation_base = (uint32_t)allocation_base_native;

    /* This is the exact MSVC vector allocation split in owner 0x004ef2e0.
     * Small buffers expose malloc's base.  At 0x1000 bytes and above the
     * owner mallocs span+0x23, rounds (raw+0x23) down to 32-byte alignment,
     * and stores raw at begin[-1].  Validate that header only after the range
     * lease has made it safe to read. */
    if (span < KAGE_VITA_FX_BIG_ALLOCATION) {
        if (allocation_base != begin)
            goto reject_lease;
    } else {
        uint32_t gap;
        if ((begin & (KAGE_VITA_FX_BIG_ALIGNMENT - 1U)) != 0U ||
            allocation_base > begin - 4U)
            goto reject_lease;
        gap = begin - allocation_base;
        if (gap < 4U || gap > KAGE_VITA_FX_BIG_OVERHEAD ||
            KAGE_VITA_FX_ROLLBACK_READ32(begin - 4U) != allocation_base)
            goto reject_lease;
    }

    last = end - KAGE_VITA_FX_RECORD_SIZE;
    if (KAGE_VITA_FX_ROLLBACK_READ32(
            last + KAGE_VITA_FX_RECORD_IMAGE) != 0U ||
        KAGE_VITA_FX_ROLLBACK_READ32(
            last + KAGE_VITA_FX_RECORD_TYPE) !=
            KAGE_VITA_FX_LIGHTING_TYPE)
        goto reject_lease;

    /* No lease may cross the translated destructor: guest_fault exits it by
     * longjmp.  The short lease only makes the validation reads atomic with
     * respect to native free/realloc; normal vector synchronization owns the
     * later destructor/commit exactly as it did in the original guest. */
    if (!KAGE_VITA_FX_ROLLBACK_HEAP_RELEASE(lease))
        return -1;
    *owner_out = owner;
    *last_out = last;
    *end_out = end;
    return 1;

reject_lease:
    (void)KAGE_VITA_FX_ROLLBACK_HEAP_RELEASE(lease);
    return -1;
#endif
}

void kage_vita_fx_rollback_note(
    uint32_t root_rva, uint32_t owner, uint32_t last, uint32_t old_end)
{
#if defined(ISAAC_VITA_FXLAYERS_NULL_ROLLBACK)
    KAGE_VITA_FX_ROLLBACK_LOG(
        "FXLayers NULL-image rollback: root=0x%08x owner=0x%08x "
        "end=0x%08x->0x%08x type=4",
        root_rva, owner, old_end, last);
#else
    (void)root_rva;
    (void)owner;
    (void)last;
    (void)old_end;
#endif
}
