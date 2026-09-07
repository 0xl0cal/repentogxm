#ifndef ISAAC_HOST_VITA_ANM2_SCRATCH_H
#define ISAAC_HOST_VITA_ANM2_SCRATCH_H

#include <stddef.h>
#include <stdint.h>

/* Frozen input identities for the narrow workaround. */
#define ISAAC_VITA_ANM2_FROZEN_PE_SHA256 \
    "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
#define ISAAC_VITA_ANM2_CANONICAL_MAP_SCHEMA 1U
#define ISAAC_VITA_ANM2_CANONICAL_MAP_ALGORITHM \
    "isaac-vita-linker-map-v1"
#define ISAAC_VITA_ANM2_FROZEN_CANONICAL_MAP_SHA256 \
    "21bc2db8bbdecc4d58d8abd2bedad5471b6c61e8a97773d6e72ec0924ccfa66f"
/* guest_0001.c owns the six literal allocations.  guest_0183.c owns the
 * public malloc wrapper; guest_0184.c owns operator new and its independent
 * merged copy of the shared allocation tail. */
#define ISAAC_VITA_ANM2_GENERATED_OWNER_SHA256 \
    "8e6af1fa9ba606e1a6f525d5815a92c3ad32a8cd1243ca005abcf53d17bd639e"
#define ISAAC_VITA_ANM2_GENERATED_WRAPPER_0_SHA256 \
    "570b8e97a0007d3abddc6044d32f3d328613ad226f2b6f9a3187a95d69977b49"
#define ISAAC_VITA_ANM2_GENERATED_WRAPPER_1_SHA256 \
    "f99b82112f2fdb73943e79426436ac15b0dc0aa0d58a4c1191daec03e4bbfb86"

/* At the imported malloc boundary the translated x86 stack is:
 *   ESP+0  = 0x005ead12, return after the malloc IAT thunk
 *   ESP+4  = request size
 *   ESP+8  = operator-new wrapper's saved EBP
 *   ESP+12 = original operator-new owner return RVA
 * The hook belongs only at the heap-import boundary, after stack validation. */
#define ISAAC_VITA_ANM2_MALLOC_IMPORT_RETURN_RVA 0x005ead12U
#define ISAAC_VITA_ANM2_OWNER_STACK_OFFSET       12U
#define ISAAC_VITA_ANM2_OPERATOR_NEW_RVA         0x005eb09cU

#define ISAAC_VITA_ANM2_OWNER_RETURN_0 0x0000aa5dU
#define ISAAC_VITA_ANM2_OWNER_RETURN_1 0x0000aaa9U
#define ISAAC_VITA_ANM2_OWNER_RETURN_2 0x0000aaf9U
#define ISAAC_VITA_ANM2_OWNER_RETURN_3 0x0000ab59U
#define ISAAC_VITA_ANM2_OWNER_RETURN_4 0x0000aba9U
#define ISAAC_VITA_ANM2_OWNER_RETURN_5 0x0000ac25U

#define ISAAC_VITA_ANM2_SMALL_SEGMENT_BYTES 420000U
#define ISAAC_VITA_ANM2_LARGE_SEGMENT_BYTES 540000U
#define ISAAC_VITA_ANM2_SEGMENT_COUNT        6U
#define ISAAC_VITA_ANM2_PAYLOAD_BYTES        2760000U
#define ISAAC_VITA_ANM2_MEMBLOCK_PAGE_BYTES  4096U
#define ISAAC_VITA_ANM2_MEMBLOCK_BYTES       2760704U
#define ISAAC_VITA_ANM2_SEGMENT_ALIGNMENT    16U
#define ISAAC_VITA_ANM2_VITA3K_ALIGNMENT     8192U
#define ISAAC_VITA_ANM2_VITA3K_RETAINED_MAX  2768896U
/* Copied from VitaSDK psp2/kernel/error.h.  This is the only native reserve
 * failure for which the exact six-owner session may use the ordinary guest
 * heap router: no memblock transaction exists and the already-reserved
 * overflow mspace may still have capacity. */
#define ISAAC_VITA_ANM2_NO_FREE_PHYSICAL_PAGE \
    ((int32_t)UINT32_C(0x80024302))

/* Current combined-candidate fixed-map contract.  The ordinary 81 MiB guest
 * heap can retain one extra MiB for alignment; page_up(_end + heap + pad) is
 * therefore the first address available before the PE.  Vita3K can retain
 * one size-alignment quantum beyond the requested ANM2 memblock, and that
 * complete upper bound must still fit the measured gap.  The kernel remains
 * free to place USER_RW elsewhere, but its actual returned range must not
 * intersect either the fixed PE image or the active guest stack. */
#define ISAAC_VITA_ANM2_LINK_END              0x82ad1560U
#define ISAAC_VITA_ANM2_HEAP_BYTES            0x05100000U
#define ISAAC_VITA_ANM2_HEAP_ALIGNMENT_PAD    0x00100000U
#define ISAAC_VITA_ANM2_HEAP_RETAINED_END     0x87cd2000U
#define ISAAC_VITA_ANM2_GUEST_IMAGE_BASE      0x98000000U
#define ISAAC_VITA_ANM2_GUEST_IMAGE_BYTES     0x0085f000U
#define ISAAC_VITA_ANM2_GUEST_IMAGE_END       0x9885f000U
#define ISAAC_VITA_ANM2_PRE_IMAGE_GAP_BYTES   0x1032e000U
#define ISAAC_VITA_ANM2_GAP_REMAINING_BYTES   0x1008a000U

typedef enum isaac_vita_anm2_scratch_result {
    ISAAC_VITA_ANM2_SCRATCH_REJECTED = -1,
    ISAAC_VITA_ANM2_SCRATCH_NOT_HANDLED = 0,
    ISAAC_VITA_ANM2_SCRATCH_HANDLED = 1,
    ISAAC_VITA_ANM2_SCRATCH_ROUTE_GUEST_HEAP = 2
} isaac_vita_anm2_scratch_result;

typedef struct isaac_vita_anm2_scratch_decision {
    void *pointer;
    uint32_t fault_value;
    const char *fault;
} isaac_vita_anm2_scratch_decision;

/* A handled allocation may deliberately return NULL for native reserve
 * failures other than NO_FREE_PHYSICAL_PAGE; operator new then follows its
 * unchanged new-handler/bad_alloc path.  The one explicit ROUTE_GUEST_HEAP
 * result preserves the exact six-owner/order contract while asking the caller
 * to continue through the normal ledger-backed newlib/overflow-mspace router.
 * Such pointers never become scratch-owned and their free/realloc operations
 * therefore remain with that ordinary router. */
int isaac_vita_anm2_scratch_malloc(
    uint32_t owner_return_rva, size_t size,
    uint32_t guest_stack_floor, uint32_t guest_stack_ceiling,
    isaac_vita_anm2_scratch_decision *decision);

/* Only the six exact segment bases are handled.  NULL, interior pointers and
 * all foreign pointers stay on the existing heap-import path. */
int isaac_vita_anm2_scratch_free(
    void *pointer, isaac_vita_anm2_scratch_decision *decision);

#if defined(ISAAC_VITA_ANM2_POOL_INIT) && ISAAC_VITA_ANM2_POOL_INIT
/* Complete the frozen initializers only while this exact segment is live in
 * its just-acquired scratch session. Holds the existing ownership lock across
 * all writes; rejection writes nothing. Ordinary guest-heap pools are excluded. */
int isaac_vita_anm2_scratch_init_pool(
    unsigned index, uint32_t pool,
    uint32_t guest_stack_floor, uint32_t guest_stack_ceiling);
#endif

#ifdef ISAAC_VITA_ANM2_SCRATCH_ORACLE
typedef struct isaac_vita_anm2_scratch_snapshot {
    uintptr_t base;
    int32_t uid;
    uint32_t poisoned;
    uint32_t guest_heap_fallback;
    uint32_t session_id;
    uint32_t live_count;
    uint32_t acquired_count;
    uint32_t freed_count;
    uint32_t peak_live;
} isaac_vita_anm2_scratch_snapshot;

int isaac_vita_anm2_scratch_oracle_snapshot(
    isaac_vita_anm2_scratch_snapshot *snapshot);
int isaac_vita_anm2_scratch_oracle_reset(void);
#endif

#endif
