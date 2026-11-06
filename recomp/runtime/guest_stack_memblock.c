/* Dedicated USER_RW memblock behind the 4 MiB guest stack.  Pure sysmem
 * transaction: no allocator, no memset, no logging (the boot receipt is
 * printed by entry_vita.c from the snapshot). */
#include <stddef.h>
#include <stdint.h>

#include <psp2/kernel/sysmem.h>

#include "guest_stack_memblock.h"

_Static_assert((GUEST_STACK_MEMBLOCK_PAGE_BYTES &
                (GUEST_STACK_MEMBLOCK_PAGE_BYTES - 1U)) == 0U,
               "guest stack memblock page size must be a power of two");
_Static_assert((GUEST_STACK_MEMBLOCK_REQUEST_BYTES &
                (~GUEST_STACK_MEMBLOCK_REQUEST_BYTES + 1U)) ==
                   GUEST_STACK_MEMBLOCK_PAGE_BYTES,
               "guest stack memblock request must keep a 4 KiB low bit");
_Static_assert(GUEST_STACK_MEMBLOCK_RETAINED_BYTES ==
                   GUEST_STACK_MEMBLOCK_REQUEST_BYTES +
                       GUEST_STACK_MEMBLOCK_PAGE_BYTES,
               "Vita3K retains one more low-bit quantum");

static void guest_stack_memblock_clear(guest_stack_memblock_snapshot *s)
{
    s->uid = -1;
    s->get_result = 0;
    s->rollback_result = 0;
    s->base = (uintptr_t)0U;
    s->request_bytes = GUEST_STACK_MEMBLOCK_REQUEST_BYTES;
    s->status = GUEST_STACK_MEMBLOCK_NONE;
}

int guest_stack_memblock_acquire(size_t usable_bytes,
                                 guest_stack_memblock_snapshot *out)
{
    SceUID uid;
    void *base = NULL;
    int get_result;

    if (!out)
        return 0;
    guest_stack_memblock_clear(out);
    if (usable_bytes != (size_t)(GUEST_STACK_MEMBLOCK_REQUEST_BYTES -
                                 GUEST_STACK_MEMBLOCK_PAGE_BYTES)) {
        /* Contract violation: rejected before any sysmem call.  uid == -1
         * (never a kernel rc) tells the receipt apart from a real failure. */
        out->status = GUEST_STACK_MEMBLOCK_ALLOC_FAILED;
        return 0;
    }

    uid = sceKernelAllocMemBlock(
        GUEST_STACK_MEMBLOCK_NAME, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
        (SceSize)GUEST_STACK_MEMBLOCK_REQUEST_BYTES, NULL);
    out->uid = (int32_t)uid;
    if (uid < 0) {
        out->status = GUEST_STACK_MEMBLOCK_ALLOC_FAILED;
        return 0;
    }

    get_result = sceKernelGetMemBlockBase(uid, &base);
    out->get_result = get_result;
    out->base = (uintptr_t)base;
    if (get_result < 0)
        out->status = GUEST_STACK_MEMBLOCK_GET_FAILED;
    else if (!base ||
             ((uintptr_t)base & (GUEST_STACK_MEMBLOCK_PAGE_BYTES - 1U)) != 0U)
        out->status = GUEST_STACK_MEMBLOCK_INVALID_BASE;
    else if ((uintptr_t)base >
             UINTPTR_MAX - (uintptr_t)GUEST_STACK_MEMBLOCK_REQUEST_BYTES)
        out->status = GUEST_STACK_MEMBLOCK_WRAP;
    else {
        out->status = GUEST_STACK_MEMBLOCK_READY;
        return 1;
    }

    out->rollback_result = sceKernelFreeMemBlock(uid);
    if (out->rollback_result >= 0)
        return 0;
    out->status = GUEST_STACK_MEMBLOCK_ORPHANED;
    return -1;
}

int guest_stack_memblock_release(const guest_stack_memblock_snapshot *s)
{
    if (!s || s->uid < 0 ||
        (s->status != GUEST_STACK_MEMBLOCK_READY &&
         s->status != GUEST_STACK_MEMBLOCK_ORPHANED))
        return 0;
    return sceKernelFreeMemBlock((SceUID)s->uid);
}

const char *guest_stack_memblock_status_text(uint32_t status)
{
    switch (status) {
    case GUEST_STACK_MEMBLOCK_NONE:
        return "none";
    case GUEST_STACK_MEMBLOCK_READY:
        return "ready";
    case GUEST_STACK_MEMBLOCK_ALLOC_FAILED:
        return "alloc-failed";
    case GUEST_STACK_MEMBLOCK_GET_FAILED:
        return "get-base-failed";
    case GUEST_STACK_MEMBLOCK_INVALID_BASE:
        return "invalid-base";
    case GUEST_STACK_MEMBLOCK_WRAP:
        return "wrap";
    case GUEST_STACK_MEMBLOCK_ORPHANED:
        return "orphaned";
    default:
        return "unknown";
    }
}
