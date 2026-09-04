/* Default-off measurements and policy for KAGE texture backing storage. */
#include <stdint.h>

#include "kage_vita_texture_memory.h"

#if defined(ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC) || \
    defined(ISAAC_VITA_TEXTURE_ALIGN8_POLICY)
#include "platform.h"
#endif

#ifdef ISAAC_VITA_TEXTURE_MEMORY_ORACLE
void kage_vita_texture_memory_oracle_log(const char *format, ...);
#endif

#ifdef ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC
#include <malloc.h>
#include <stdatomic.h>
#include <stdlib.h>

#ifndef ISAAC_VITA_TEXTURE_MEMORY_ORACLE
#include <psp2/kernel/sysmem.h>
#endif

#ifdef ISAAC_VITA_TEXTURE_MEMORY_ORACLE
struct mallinfo kage_vita_texture_memory_oracle_mallinfo(void);
void *kage_vita_texture_memory_oracle_malloc(size_t size);
void kage_vita_texture_memory_oracle_free(void *pointer);
int32_t kage_vita_texture_memory_oracle_memblock_alloc(size_t size);
int kage_vita_texture_memory_oracle_memblock_get(int32_t uid, void **base);
int kage_vita_texture_memory_oracle_memblock_free(int32_t uid);
#define TEXTURE_MALLINFO kage_vita_texture_memory_oracle_mallinfo
#define TEXTURE_MALLOC   kage_vita_texture_memory_oracle_malloc
#define TEXTURE_FREE     kage_vita_texture_memory_oracle_free
#define TEXTURE_MEMBLOCK_ALLOC \
    kage_vita_texture_memory_oracle_memblock_alloc
#define TEXTURE_MEMBLOCK_GET \
    kage_vita_texture_memory_oracle_memblock_get
#define TEXTURE_MEMBLOCK_FREE \
    kage_vita_texture_memory_oracle_memblock_free
#define TEXTURE_LOG      kage_vita_texture_memory_oracle_log
#else
#define TEXTURE_MALLINFO mallinfo
#define TEXTURE_MALLOC   malloc
#define TEXTURE_FREE     free
#define TEXTURE_MEMBLOCK_ALLOC(size) \
    sceKernelAllocMemBlock( \
        "isaac_texel_probe", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, \
        (SceSize)(size), NULL)
#define TEXTURE_MEMBLOCK_GET sceKernelGetMemBlockBase
#define TEXTURE_MEMBLOCK_FREE sceKernelFreeMemBlock
#define TEXTURE_LOG      isaac_vita_log
#endif

typedef struct texture_memblock_probe_result {
    int32_t uid;
    int get_result;
    int free_result;
    uintptr_t base;
    uintptr_t end;
    const char *status;
} texture_memblock_probe_result;

_Static_assert((KAGE_VITA_TEXEL_MEMBLOCK_8M_BYTES &
                (0U - KAGE_VITA_TEXEL_MEMBLOCK_8M_BYTES)) ==
                   KAGE_VITA_TEXEL_MEMBLOCK_8M_ALIGNMENT,
               "8 MiB texel probe changed Vita3K alignment");
_Static_assert(KAGE_VITA_TEXEL_MEMBLOCK_8M_BYTES +
                   KAGE_VITA_TEXEL_MEMBLOCK_8M_ALIGNMENT ==
                   KAGE_VITA_TEXEL_MEMBLOCK_8M_RETAINED_MAX,
               "8 MiB texel probe retained-span arithmetic drifted");
_Static_assert((KAGE_VITA_TEXEL_MEMBLOCK_MAX_BYTES &
                (0U - KAGE_VITA_TEXEL_MEMBLOCK_MAX_BYTES)) ==
                   KAGE_VITA_TEXEL_MEMBLOCK_MAX_ALIGNMENT,
               "maximum texel probe changed Vita3K alignment");
_Static_assert(KAGE_VITA_TEXEL_MEMBLOCK_MAX_BYTES +
                   KAGE_VITA_TEXEL_MEMBLOCK_MAX_ALIGNMENT ==
                   KAGE_VITA_TEXEL_MEMBLOCK_MAX_RETAINED_MAX,
               "maximum texel probe retained-span arithmetic drifted");
_Static_assert(KAGE_VITA_TEXEL_LINK_END <
                   KAGE_VITA_TEXEL_HEAP_RETAINED_END &&
                   KAGE_VITA_TEXEL_HEAP_RETAINED_END <
                       KAGE_VITA_TEXEL_GUEST_IMAGE_BASE &&
                   KAGE_VITA_TEXEL_GUEST_IMAGE_BASE <
                       KAGE_VITA_TEXEL_GUEST_IMAGE_END,
               "texel probe frozen ranges are not ordered");

static int texture_ranges_overlap(uintptr_t first_start,
                                  uintptr_t first_end,
                                  uintptr_t second_start,
                                  uintptr_t second_end)
{
    return first_start < second_end && second_start < first_end;
}

static int texture_memblock_probe(
    size_t request, uint32_t guest_stack_floor, uint32_t guest_stack_ceiling,
    texture_memblock_probe_result *result)
{
    void *base_pointer = NULL;
    int safe = 0;

    result->uid = TEXTURE_MEMBLOCK_ALLOC(request);
    result->get_result = -1;
    result->free_result = -1;
    result->base = 0U;
    result->end = 0U;
    result->status = "alloc-failed";
    if (result->uid < 0)
        return 0;

    result->get_result = TEXTURE_MEMBLOCK_GET(
        result->uid, &base_pointer);
    if (result->get_result < 0) {
        result->status = "get-base-failed";
    }
    else {
        result->base = (uintptr_t)base_pointer;
        if (!result->base ||
            (result->base &
             (KAGE_VITA_TEXEL_MEMBLOCK_PAGE_BYTES - 1U)) != 0U ||
            result->base >
                (uintptr_t)UINT32_MAX - request) {
            result->status = "invalid-base";
        }
        else if (guest_stack_floor >= guest_stack_ceiling) {
            result->status = "invalid-stack";
        }
        else {
            result->end = result->base + request;
            if (texture_ranges_overlap(
                    result->base, result->end,
                    KAGE_VITA_TEXEL_LINK_END,
                    KAGE_VITA_TEXEL_HEAP_RETAINED_END)) {
                result->status = "heap-overlap";
            }
            else if (texture_ranges_overlap(
                         result->base, result->end,
                         KAGE_VITA_TEXEL_GUEST_IMAGE_BASE,
                         KAGE_VITA_TEXEL_GUEST_IMAGE_END)) {
                result->status = "image-overlap";
            }
            else if (texture_ranges_overlap(
                         result->base, result->end,
                         guest_stack_floor, guest_stack_ceiling)) {
                result->status = "stack-overlap";
            }
            else {
                result->status = "range-safe";
                safe = 1;
            }
        }
    }

    result->free_result = TEXTURE_MEMBLOCK_FREE(result->uid);
    if (result->free_result < 0) {
        result->status = "free-failed";
        return 0;
    }
    if (safe)
        result->status = "PASS";
    return safe;
}

static atomic_flag s_texel_oom_reported = ATOMIC_FLAG_INIT;

void kage_vita_texel_oom_diagnostic(
    size_t failed_request, uint32_t owner_return_rva,
    unsigned owner_chain_depth, const char *failure_reason,
    size_t ledger_live_requested,
    size_t ledger_largest_live_request, size_t ledger_live_count,
    int ledger_requested_accounting_exact, uint32_t guest_stack_floor,
    uint32_t guest_stack_ceiling)
{
    struct mallinfo heap;
    texture_memblock_probe_result memblock_8m;
    texture_memblock_probe_result memblock_max;
    void *large_probe;
    void *small_probe;
    int large_probe_passed;
    int small_probe_passed;

    if (!failure_reason)
        failure_reason = "unknown";

    if (atomic_flag_test_and_set_explicit(
            &s_texel_oom_reported, memory_order_relaxed))
        return;

    /* Snapshot immediately after the first failed allocation from one of the
     * six frozen texture loaders.  These raw probes never enter the guest
     * ownership ledger and are released before execution resumes.  They are
     * exact lower-bound tests, not a claim about the largest free chunk. */
    heap = TEXTURE_MALLINFO();
    large_probe = TEXTURE_MALLOC(KAGE_VITA_TEXEL_PROBE_LARGE_REQUEST);
    large_probe_passed = large_probe != NULL;
    if (large_probe)
        TEXTURE_FREE(large_probe);
    small_probe = TEXTURE_MALLOC(KAGE_VITA_TEXEL_PROBE_SMALL_REQUEST);
    small_probe_passed = small_probe != NULL;
    if (small_probe)
        TEXTURE_FREE(small_probe);
    (void)texture_memblock_probe(
        KAGE_VITA_TEXEL_MEMBLOCK_8M_BYTES,
        guest_stack_floor, guest_stack_ceiling, &memblock_8m);
    if (memblock_8m.uid < 0 || memblock_8m.free_result >= 0) {
        (void)texture_memblock_probe(
            KAGE_VITA_TEXEL_MEMBLOCK_MAX_BYTES,
            guest_stack_floor, guest_stack_ceiling, &memblock_max);
    }
    else {
        memblock_max.uid = -1;
        memblock_max.get_result = -1;
        memblock_max.free_result = -1;
        memblock_max.base = 0U;
        memblock_max.end = 0U;
        memblock_max.status = "skipped-after-free-failure";
    }

    TEXTURE_LOG(
        "texel OOM diagnostic: event=1 request=%u owner=0x%08x "
        "chain_depth=%u failure=%s part=allocator "
        "arena=%u allocated=%u free=%u chunks=%u "
        "ledger_live_requested=%u ledger_largest_live_request=%u "
        "ledger_live_count=%u ledger_requested_exact=%s "
        "probe_4660224=%s probe_2097152=%s",
        (unsigned)failed_request, (unsigned)owner_return_rva,
        owner_chain_depth, failure_reason, (unsigned)heap.arena,
        (unsigned)heap.uordblks, (unsigned)heap.fordblks,
        (unsigned)heap.ordblks, (unsigned)ledger_live_requested,
        (unsigned)ledger_largest_live_request,
        (unsigned)ledger_live_count,
        ledger_requested_accounting_exact ? "yes" : "no",
        large_probe_passed ? "PASS" : "FAIL",
        small_probe_passed ? "PASS" : "FAIL");
    TEXTURE_LOG(
        "texel OOM diagnostic: event=1 request=%u owner=0x%08x "
        "chain_depth=%u failure=%s part=memblocks "
        "memblock_00801000=%s uid=0x%08x get=0x%08x free=0x%08x "
        "base=0x%08x end=0x%08x "
        "memblock_00bd6000=%s uid=0x%08x get=0x%08x free=0x%08x "
        "base=0x%08x end=0x%08x",
        (unsigned)failed_request, (unsigned)owner_return_rva,
        owner_chain_depth, failure_reason, memblock_8m.status,
        (unsigned)memblock_8m.uid, (unsigned)memblock_8m.get_result,
        (unsigned)memblock_8m.free_result, (unsigned)memblock_8m.base,
        (unsigned)memblock_8m.end, memblock_max.status,
        (unsigned)memblock_max.uid, (unsigned)memblock_max.get_result,
        (unsigned)memblock_max.free_result, (unsigned)memblock_max.base,
        (unsigned)memblock_max.end);
}

#ifdef ISAAC_VITA_TEXTURE_MEMORY_ORACLE
void kage_vita_texel_oom_diagnostic_oracle_reset(void)
{
    atomic_flag_clear_explicit(&s_texel_oom_reported, memory_order_relaxed);
}
#endif
#endif

#ifdef ISAAC_VITA_TEXTURE_ALIGN8_POLICY
#ifndef ISAAC_VITA_TEXTURE_MEMORY_ORACLE
#include "guest.h"
#endif

#ifdef ISAAC_VITA_TEXTURE_MEMORY_ORACLE
#define TEXTURE_POLICY_LOG kage_vita_texture_memory_oracle_log
#else
#define TEXTURE_POLICY_LOG isaac_vita_log
#endif

static int texture_align8_apply_byte(uint8_t *flag, uint32_t guest_address)
{
    uint8_t previous = *flag;

    if (previous > 1U) {
        TEXTURE_POLICY_LOG(
            "KAGE texture align8 policy FAILED: addr=0x%08x value=%u",
            (unsigned)guest_address, (unsigned)previous);
        return 0;
    }
    *flag = 1U;
    TEXTURE_POLICY_LOG(
        "KAGE texture align8 policy: addr=0x%08x previous=%u active=1",
        (unsigned)guest_address, (unsigned)previous);
    return 1;
}

#ifdef ISAAC_VITA_TEXTURE_MEMORY_ORACLE
int kage_vita_texture_align8_policy_oracle_apply(uint8_t *flag,
                                                  int image_contains_flag)
{
    uint32_t address = GUEST_IMAGE_BASE +
                       KAGE_VITA_TEXTURE_ALIGN8_FLAG_RVA;

    if (!flag || !image_contains_flag) {
        TEXTURE_POLICY_LOG(
            "KAGE texture align8 policy FAILED: addr=0x%08x absent",
            (unsigned)address);
        return 0;
    }
    return texture_align8_apply_byte(flag, address);
}
#else
int kage_vita_texture_align8_policy_apply(void)
{
    uint32_t address = GUEST_IMAGE_BASE +
                       KAGE_VITA_TEXTURE_ALIGN8_FLAG_RVA;

    if (!guest_image_contains(address, 1U)) {
        TEXTURE_POLICY_LOG(
            "KAGE texture align8 policy FAILED: addr=0x%08x absent",
            (unsigned)address);
        return 0;
    }
    return texture_align8_apply_byte(
        (uint8_t *)(uintptr_t)address, address);
}
#endif
#endif
