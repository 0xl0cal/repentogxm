/* Link-only fake-sysmem probe.  The ARM ELF is inspected, never executed. */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <psp2/kernel/sysmem.h>

#include "host_vita_ogg_emergency.h"

_Alignas(ISAAC_VITA_OGG_MEMBLOCK_PAGE_BYTES)
static unsigned char s_probe_block[ISAAC_VITA_OGG_OPEN_BACKING_BYTES];
static int s_probe_live;

SceUID sceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type,
                              SceSize size,
                              SceKernelAllocMemBlockOpt *option)
{
    (void)name;
    (void)option;
    if (s_probe_live || type != SCE_KERNEL_MEMBLOCK_TYPE_USER_RW ||
        size != ISAAC_VITA_OGG_OPEN_BACKING_BYTES)
        return -1;
    s_probe_live = 1;
    return 0x4567;
}

int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    if (uid != 0x4567 || !base || !s_probe_live)
        return -1;
    *base = s_probe_block;
    return 0;
}

int sceKernelFreeMemBlock(SceUID uid)
{
    if (uid != 0x4567 || !s_probe_live)
        return -1;
    s_probe_live = 0;
    return 0;
}

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    va_end(arguments);
}

int main(void)
{
    isaac_vita_ogg_emergency_decision decision;
    void *pointer;

    if (isaac_vita_ogg_emergency_malloc(
            ISAAC_VITA_OGG_OPEN_ALLOCATION_RETURN_RVA,
            ISAAC_VITA_OGG_OPEN_BACKING_BYTES,
            0x84000000U, 0x84400000U, &decision) !=
                ISAAC_VITA_OGG_EMERGENCY_HANDLED ||
        !decision.pointer)
        return 1;
    pointer = decision.pointer;
    if (isaac_vita_ogg_emergency_free(pointer, &decision) !=
            ISAAC_VITA_OGG_EMERGENCY_HANDLED)
        return 2;
    return 0;
}
