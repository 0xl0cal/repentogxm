#include "fixed_image.h"
#include "platform.h"

#include <kubridge.h>
#include <psp2/kernel/sysmem.h>

#include <stdint.h>
#include <string.h>

#ifndef GUEST_IMAGE_BASE
#define GUEST_IMAGE_BASE 0x98000000u
#endif

#define PAGE_SIZE 0x1000u
#define FIXED_ADDRESS_ATTR 0x1u

static SceUID g_image_uid = -1;
static void  *g_image_address;

void *isaac_vita_fixed_alloc(uint32_t address, uint32_t size)
{
    SceKernelAllocMemBlockKernelOpt opt;
    SceUID uid;
    void *base = NULL;
    uint32_t allocation_size;
    int result;

    if (g_image_uid >= 0) {
        isaac_vita_log("fixed image allocation requested twice");
        return NULL;
    }
    if (address != GUEST_IMAGE_BASE) {
        isaac_vita_log("refusing guest base 0x%08x; build expects 0x%08x",
                       (unsigned)address, (unsigned)GUEST_IMAGE_BASE);
        return NULL;
    }
    if (size == 0u || size > UINT32_MAX - (PAGE_SIZE - 1u)) {
        isaac_vita_log("invalid fixed image size %u", (unsigned)size);
        return NULL;
    }
    allocation_size = (size + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);

    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.attr = FIXED_ADDRESS_ATTR;
    opt.field_C = address;

    uid = kuKernelAllocMemBlock("isaac_pe_image",
                                SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                allocation_size, &opt);
    if (uid < 0) {
        isaac_vita_log("kuKernelAllocMemBlock base=0x%08x size=%u failed: 0x%08x",
                       (unsigned)address, (unsigned)allocation_size,
                       (unsigned)uid);
        return NULL;
    }

    result = sceKernelGetMemBlockBase(uid, &base);
    if (result < 0 || (uintptr_t)base != (uintptr_t)address) {
        isaac_vita_log("fixed block mismatch: get=0x%08x base=%p expected=0x%08x",
                       (unsigned)result, base, (unsigned)address);
        sceKernelFreeMemBlock(uid);
        return NULL;
    }

    memset(base, 0, allocation_size);
    g_image_uid = uid;
    g_image_address = base;
    isaac_vita_log("fixed PE image reserved: base=%p size=%u uid=0x%08x",
                   base, (unsigned)allocation_size, (unsigned)uid);
    return base;
}

void isaac_vita_fixed_free(void *address)
{
    int result;

    if (g_image_uid < 0)
        return;
    if (address != g_image_address) {
        isaac_vita_log("refusing mismatched fixed free: got=%p expected=%p",
                       address, g_image_address);
        return;
    }

    result = sceKernelFreeMemBlock(g_image_uid);
    isaac_vita_log("fixed PE image free: base=%p uid=0x%08x result=0x%08x",
                   g_image_address, (unsigned)g_image_uid, (unsigned)result);
    if (result >= 0) {
        g_image_uid = -1;
        g_image_address = NULL;
    }
}
