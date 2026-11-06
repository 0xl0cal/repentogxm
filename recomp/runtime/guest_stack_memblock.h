#ifndef ISAAC_GUEST_STACK_MEMBLOCK_H
#define ISAAC_GUEST_STACK_MEMBLOCK_H

/* Dedicated USER_RW backing for the 4 MiB guest stack
 * (ISAAC_VITA_GUEST_STACK_MEMBLOCK).  This header is psp2-free so guest.c
 * stays host-preprocessable for the text pins; the sysmem transaction lives
 * in guest_stack_memblock.c and is compiled only when the knob is ON. */

#include <stddef.h>
#include <stdint.h>

/* USER_RW blocks whose requested size is a power of two make Vita3K's
 * allocator retain a second, equally large alignment quantum.  Requesting
 * GUEST_STACK_SIZE plus one page keeps the request's low set bit at 4 KiB:
 * Vita3K's USER_RW allocator retains one extra low-bit quantum (measured),
 * so RETAINED is request + 4 KiB there instead of 8 MiB; whether hardware
 * retains it is unmeasured. */
#define GUEST_STACK_MEMBLOCK_PAGE_BYTES      0x00001000U
#define GUEST_STACK_MEMBLOCK_REQUEST_BYTES   0x00401000U
#define GUEST_STACK_MEMBLOCK_RETAINED_BYTES  0x00402000U
#define GUEST_STACK_MEMBLOCK_NAME            "isaac_guest_stack"

enum {
    GUEST_STACK_MEMBLOCK_NONE = 0,
    GUEST_STACK_MEMBLOCK_READY = 1,
    GUEST_STACK_MEMBLOCK_ALLOC_FAILED = 2,
    GUEST_STACK_MEMBLOCK_GET_FAILED = 3,
    GUEST_STACK_MEMBLOCK_INVALID_BASE = 4,
    GUEST_STACK_MEMBLOCK_WRAP = 5,
    GUEST_STACK_MEMBLOCK_ORPHANED = 6
};

typedef struct guest_stack_memblock_snapshot {
    int32_t   uid;             /* alloc rc; the block UID when >= 0 */
    int32_t   get_result;      /* sceKernelGetMemBlockBase rc */
    int32_t   rollback_result; /* sceKernelFreeMemBlock rc of a failed
                                * transaction (0 when none ran) */
    uintptr_t base;            /* mapped base, 0 unless the get succeeded */
    uint32_t  request_bytes;   /* GUEST_STACK_MEMBLOCK_REQUEST_BYTES */
    uint32_t  status;          /* GUEST_STACK_MEMBLOCK_* */
} guest_stack_memblock_snapshot;

/* Acquire GUEST_STACK_MEMBLOCK_REQUEST_BYTES of USER_RW for a stack of
 * exactly usable_bytes == REQUEST - PAGE.  Returns 1 = READY, 0 = failed and
 * rolled back (uid/get/rollback stay in *out for the boot receipt),
 * -1 = ORPHANED (rollback failed too; the UID stays owned).  The block is not
 * zeroed here: guest_stack_init memsets GUEST_STACK_SIZE itself. */
int guest_stack_memblock_acquire(size_t usable_bytes,
                                 guest_stack_memblock_snapshot *out);

/* Free the block of a READY or ORPHANED snapshot; raw sceKernelFreeMemBlock
 * rc.  0 without a call for NULL, uid < 0 or a rolled-back snapshot. */
int guest_stack_memblock_release(const guest_stack_memblock_snapshot *s);

/* Receipt word for a GUEST_STACK_MEMBLOCK_* status. */
const char *guest_stack_memblock_status_text(uint32_t status);

/* Defined in guest.c under the knob: copy of the stack owner's snapshot for
 * the boot receipt.  Returns 0 only for a NULL argument. */
int guest_stack_memblock_snapshot_get(guest_stack_memblock_snapshot *out);

#endif
