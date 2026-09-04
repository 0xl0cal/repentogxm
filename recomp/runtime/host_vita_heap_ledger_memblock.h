#ifndef ISAAC_HOST_VITA_HEAP_LEDGER_MEMBLOCK_H
#define ISAAC_HOST_VITA_HEAP_LEDGER_MEMBLOCK_H

#include <stddef.h>
#include <stdint.h>

#define ISAAC_VITA_HEAP_LEDGER_PAGE_BYTES 0x1000U
#define ISAAC_VITA_HEAP_LEDGER_INVALID_UID ((int32_t)-1)

typedef struct isaac_vita_heap_ledger_storage {
    void *base;
    int32_t uid;
    size_t usable_bytes;
    size_t block_bytes;
} isaac_vita_heap_ledger_storage;

enum {
    ISAAC_VITA_HEAP_LEDGER_STORAGE_ORPHANED = -1,
    ISAAC_VITA_HEAP_LEDGER_STORAGE_FAILED = 0,
    ISAAC_VITA_HEAP_LEDGER_STORAGE_READY = 1
};

/* USER_RW blocks whose requested size is a power of two make Vita3K retain a
 * second, equally large alignment quantum.  This layout page-rounds the table
 * and adds one or two pages so the final request always has a 4-KiB low bit. */
int isaac_vita_heap_ledger_memblock_layout(
    size_t usable_bytes, size_t *block_bytes_out);

/* Acquire zeroed native-only storage.  ORPHANED means acquisition failed and
 * rollback of the new UID also failed; the returned storage remains owned by
 * the caller until process exit or a later explicit test cleanup. */
int isaac_vita_heap_ledger_memblock_acquire(
    size_t usable_bytes, isaac_vita_heap_ledger_storage *storage_out);

/* Return the raw sceKernelFreeMemBlock result.  On failure, storage remains
 * unchanged and owned by the caller. */
int isaac_vita_heap_ledger_memblock_release(
    const isaac_vita_heap_ledger_storage *storage);

#endif
