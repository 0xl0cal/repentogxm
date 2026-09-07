/* Built only by the sparse native recipe; never linked into FULL/OFF. */
#include "isaac_sdk_sparse_profile_internal.h"
#include <limits.h>
#include <string.h>
#if defined(ISAAC_SDK_SPARSE_HOST_TEST)
extern uint64_t sceKernelGetProcessTimeWide(void);
#else
#include <psp2/kernel/processmgr.h>
#endif

IsaacSdkSparseProfile isaac_sdk_sparse;

int isaacSdkSparseSelectCanonical(void)
{
    uint32_t ordinal = isaac_sdk_sparse.canonical_seen++;
    int selected = (ordinal & 31u) == isaac_sdk_sparse.residue;
    if (selected) ++isaac_sdk_sparse.canonical_selected;
    return selected;
}

uint64_t isaacSdkSparseClock(void)
{
    ++isaac_sdk_sparse.clock_reads;
    return sceKernelGetProcessTimeWide();
}

void isaacSdkSparseAdd(unsigned kind, uint64_t start, uint64_t end)
{
    IsaacSdkSparseBucket *bucket = &isaac_sdk_sparse.bucket[kind];
    uint64_t elapsed;
    ++bucket->calls;
    if (end < start) {
        ++isaac_sdk_sparse.bad_clock;
        return;
    }
    elapsed = end - start;
    if (elapsed > UINT32_MAX) {
        elapsed = UINT32_MAX;
        ++isaac_sdk_sparse.saturation;
    }
    if (elapsed > UINT32_MAX - bucket->us) {
        bucket->us = UINT32_MAX;
        ++isaac_sdk_sparse.saturation;
    } else {
        bucket->us += (uint32_t)elapsed;
    }
    if (elapsed > bucket->max_us) bucket->max_us = (uint32_t)elapsed;
}

void vglIsaacSdkSparseTake(IsaacSdkSparseProfile *out, uint32_t next_window)
{
    if (out) {
        uint64_t start = isaacSdkSparseClock();
        isaacSdkSparseAdd(ISAAC_SDK_CONTROL, start, isaacSdkSparseClock());
        *out = isaac_sdk_sparse;
    }
    memset(&isaac_sdk_sparse, 0, sizeof isaac_sdk_sparse);
    isaac_sdk_sparse.residue = next_window & 31u;
}
