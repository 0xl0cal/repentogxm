/* Native-only ownership-ledger storage outside the fixed-size newlib arena. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <psp2/kernel/sysmem.h>

#include "host_vita_heap_ledger_memblock.h"

_Static_assert((ISAAC_VITA_HEAP_LEDGER_PAGE_BYTES &
                (ISAAC_VITA_HEAP_LEDGER_PAGE_BYTES - 1U)) == 0U,
               "ledger memblock page size must be a power of two");

static void heap_ledger_storage_clear(
    isaac_vita_heap_ledger_storage *storage)
{
    storage->base = NULL;
    storage->uid = ISAAC_VITA_HEAP_LEDGER_INVALID_UID;
    storage->usable_bytes = 0U;
    storage->block_bytes = 0U;
}

int isaac_vita_heap_ledger_memblock_layout(
    size_t usable_bytes, size_t *block_bytes_out)
{
    const size_t page = ISAAC_VITA_HEAP_LEDGER_PAGE_BYTES;
    size_t rounded;
    size_t block;

    if (block_bytes_out)
        *block_bytes_out = 0U;
    if (!usable_bytes || !block_bytes_out ||
        usable_bytes > UINT32_MAX || usable_bytes > SIZE_MAX - (page - 1U))
        return 0;
    rounded = (usable_bytes + page - 1U) & ~(page - 1U);
    if (rounded > UINT32_MAX - page)
        return 0;
    block = rounded + page;
    if ((block & (page - 1U)) != 0U)
        return 0;
    if ((block & (~block + 1U)) != page) {
        if (block > UINT32_MAX - page)
            return 0;
        block += page;
    }
    if ((block & (~block + 1U)) != page)
        return 0;
    *block_bytes_out = block;
    return 1;
}

int isaac_vita_heap_ledger_memblock_acquire(
    size_t usable_bytes, isaac_vita_heap_ledger_storage *storage_out)
{
    isaac_vita_heap_ledger_storage candidate;
    SceUID uid;
    void *base = NULL;
    size_t block_bytes;
    int get_result;
    int free_result;

    if (!storage_out)
        return ISAAC_VITA_HEAP_LEDGER_STORAGE_FAILED;
    heap_ledger_storage_clear(storage_out);
    heap_ledger_storage_clear(&candidate);
    if (!isaac_vita_heap_ledger_memblock_layout(
            usable_bytes, &block_bytes))
        return ISAAC_VITA_HEAP_LEDGER_STORAGE_FAILED;

    uid = sceKernelAllocMemBlock(
        "isaac_heap_ledger", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
        (SceSize)block_bytes, NULL);
    if (uid < 0)
        return ISAAC_VITA_HEAP_LEDGER_STORAGE_FAILED;
    candidate.uid = (int32_t)uid;
    candidate.usable_bytes = usable_bytes;
    candidate.block_bytes = block_bytes;

    get_result = sceKernelGetMemBlockBase(uid, &base);
    if (get_result >= 0 && base &&
        ((uintptr_t)base & (ISAAC_VITA_HEAP_LEDGER_PAGE_BYTES - 1U)) == 0U &&
        (uintptr_t)base <= UINTPTR_MAX - block_bytes) {
        candidate.base = base;
        /* The fake oracle deliberately returns dirty pages.  Zero the whole
         * block so neither table bytes nor retained padding carry old data. */
        memset(base, 0, block_bytes);
        *storage_out = candidate;
        return ISAAC_VITA_HEAP_LEDGER_STORAGE_READY;
    }

    candidate.base = base;
    free_result = sceKernelFreeMemBlock(uid);
    if (free_result >= 0)
        return ISAAC_VITA_HEAP_LEDGER_STORAGE_FAILED;
    *storage_out = candidate;
    return ISAAC_VITA_HEAP_LEDGER_STORAGE_ORPHANED;
}

int isaac_vita_heap_ledger_memblock_release(
    const isaac_vita_heap_ledger_storage *storage)
{
    if (!storage || storage->uid < 0)
        return 0;
    return sceKernelFreeMemBlock((SceUID)storage->uid);
}
