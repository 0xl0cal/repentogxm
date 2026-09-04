#ifndef KAGE_VITA_TEXTURE_MEMORY_H
#define KAGE_VITA_TEXTURE_MEMORY_H

#include <stddef.h>
#include <stdint.h>

/* Frozen-PE facts.  The failed PNG is 325x3552 RGBA.  KAGE's default
 * power-of-two backing is 512x4096 (8 MiB); its existing alternative rounds
 * each dimension to eight pixels, yielding 328x3552 (4,660,224 bytes). */
#define KAGE_VITA_TEXEL_OOM_REQUEST             8388608U
#define KAGE_VITA_TEXEL_ALIGN8_PROBE_REQUEST    4660224U

/* Original ImageBase is 0x00400000.  This zero-initialized byte is read by
 * ImagePcx, ImagePng, ImageRGBA, and ProceduralImageBase.  Nonzero selects
 * ceil(dimension / 8) * 8; zero selects the next power of two. */
#define KAGE_VITA_TEXTURE_ALIGN8_FLAG_ORIGINAL_VA 0x00bfd66aU
#define KAGE_VITA_TEXTURE_ALIGN8_FLAG_RVA \
    (KAGE_VITA_TEXTURE_ALIGN8_FLAG_ORIGINAL_VA - 0x00400000U)

#define KAGE_VITA_TEXEL_MALLOC_IAT_RETURN_RVA 0x0059a0abU
#define KAGE_VITA_TEXEL_WRAPPER_RETURN_RVA    0x0059a5a1U
#define KAGE_VITA_TEXEL_ADAPTER_RETURN_RVA    0x0059a5f3U
#define KAGE_VITA_TEXEL_LOADER_RETURN_0       0x005a0857U
#define KAGE_VITA_TEXEL_LOADER_RETURN_1       0x005a12b6U
#define KAGE_VITA_TEXEL_LOADER_RETURN_2       0x005a1850U
#define KAGE_VITA_TEXEL_LOADER_RETURN_3       0x005a1c79U
#define KAGE_VITA_TEXEL_LOADER_RETURN_4       0x005b5fcfU
#define KAGE_VITA_TEXEL_LOADER_RETURN_5       0x005b618cU
/* ImagePng::load_png_data.  Its RGBA backing is synchronously consumed by
 * glTexImage2D and then freed before this loader returns. */
#define KAGE_VITA_TEXEL_PNG_LOADER_RETURN \
    KAGE_VITA_TEXEL_LOADER_RETURN_1

#define KAGE_VITA_TEXEL_PROBE_LARGE_REQUEST 4660224U
#define KAGE_VITA_TEXEL_PROBE_SMALL_REQUEST 2097152U

/* Two bounded feasibility probes cover both the former 8 MiB policy and the
 * exact maximum align8 RGBA asset in the frozen 9,194-PNG census.  Adding one
 * page to 8 MiB changes Vita3K's size-derived alignment to 4 KiB.  The maximum
 * asset request 0x00bd6000 has a 0x2000 low bit, so its retained upper bound is
 * 0x00bd8000.  Each block is freed before the next allocation. */
#define KAGE_VITA_TEXEL_MEMBLOCK_8M_BYTES          0x00801000U
#define KAGE_VITA_TEXEL_MEMBLOCK_8M_ALIGNMENT      0x00001000U
#define KAGE_VITA_TEXEL_MEMBLOCK_8M_RETAINED_MAX   0x00802000U
#define KAGE_VITA_TEXEL_MEMBLOCK_MAX_BYTES         0x00bd6000U
#define KAGE_VITA_TEXEL_MEMBLOCK_MAX_ALIGNMENT     0x00002000U
#define KAGE_VITA_TEXEL_MEMBLOCK_MAX_RETAINED_MAX  0x00bd8000U
#define KAGE_VITA_TEXEL_MEMBLOCK_PAGE_BYTES        0x00001000U

/* Diagnostic hardware A/B only.  The 8 MiB request needs one additional
 * page so Vita3K and hardware both retain a page-aligned USER_RW span, but
 * that padding is not guest-visible capacity.  Production keeps the frozen
 * 0x00bd6000 maximum unless the source-scoped diagnostic definition is set
 * on host_vita_texel_scratch.c. */
#if defined(ISAAC_VITA_TEXEL_SCRATCH_8M)
#define KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES \
    KAGE_VITA_TEXEL_MEMBLOCK_8M_BYTES
#define KAGE_VITA_TEXEL_SCRATCH_REQUEST_MAX_BYTES \
    KAGE_VITA_TEXEL_OOM_REQUEST
#else
#define KAGE_VITA_TEXEL_SCRATCH_MEMBLOCK_BYTES \
    KAGE_VITA_TEXEL_MEMBLOCK_MAX_BYTES
#define KAGE_VITA_TEXEL_SCRATCH_REQUEST_MAX_BYTES \
    KAGE_VITA_TEXEL_MEMBLOCK_MAX_BYTES
#endif

#define KAGE_VITA_TEXEL_LINK_END               0x82ad1560U
#define KAGE_VITA_TEXEL_HEAP_RETAINED_END      0x87cd2000U
#define KAGE_VITA_TEXEL_GUEST_IMAGE_BASE       0x98000000U
#define KAGE_VITA_TEXEL_GUEST_IMAGE_END        0x9885f000U

#ifdef ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC
void kage_vita_texel_oom_diagnostic(
    size_t failed_request, uint32_t owner_return_rva,
    unsigned owner_chain_depth, const char *failure_reason,
    size_t ledger_live_requested,
    size_t ledger_largest_live_request, size_t ledger_live_count,
    int ledger_requested_accounting_exact, uint32_t guest_stack_floor,
    uint32_t guest_stack_ceiling);
#endif

#ifdef ISAAC_VITA_TEXTURE_ALIGN8_POLICY
int kage_vita_texture_align8_policy_apply(void);
#endif

#ifdef ISAAC_VITA_TEXTURE_MEMORY_ORACLE
void kage_vita_texel_oom_diagnostic_oracle_reset(void);
int kage_vita_texture_align8_policy_oracle_apply(uint8_t *flag,
                                                  int image_contains_flag);
#endif

#endif
