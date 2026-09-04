/* Link-only fake-sysmem probe.  The resulting ARM ELF is inspected, never
 * executed; runtime evidence still requires Vita3K or hardware. */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <psp2/kernel/sysmem.h>

#include "host_vita_anm2_scratch.h"

_Alignas(ISAAC_VITA_ANM2_MEMBLOCK_PAGE_BYTES)
static unsigned char s_probe_block[ISAAC_VITA_ANM2_MEMBLOCK_BYTES];
static int s_probe_live;

SceUID sceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type,
                              SceSize size,
                              SceKernelAllocMemBlockOpt *option)
{
    (void)name;
    (void)option;
    if (s_probe_live || type != SCE_KERNEL_MEMBLOCK_TYPE_USER_RW ||
        size != ISAAC_VITA_ANM2_MEMBLOCK_BYTES)
        return -1;
    s_probe_live = 1;
    return 0x1234;
}

int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    if (uid != 0x1234 || !base || !s_probe_live)
        return -1;
    *base = s_probe_block;
    return 0;
}

int sceKernelFreeMemBlock(SceUID uid)
{
    if (uid != 0x1234 || !s_probe_live)
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
    static const uint32_t owners[ISAAC_VITA_ANM2_SEGMENT_COUNT] = {
        ISAAC_VITA_ANM2_OWNER_RETURN_0,
        ISAAC_VITA_ANM2_OWNER_RETURN_1,
        ISAAC_VITA_ANM2_OWNER_RETURN_2,
        ISAAC_VITA_ANM2_OWNER_RETURN_3,
        ISAAC_VITA_ANM2_OWNER_RETURN_4,
        ISAAC_VITA_ANM2_OWNER_RETURN_5
    };
    isaac_vita_anm2_scratch_decision decision;
    void *pointers[ISAAC_VITA_ANM2_SEGMENT_COUNT];
    unsigned index;

    for (index = 0U; index < ISAAC_VITA_ANM2_SEGMENT_COUNT; ++index) {
        size_t size = index < 4U
            ? ISAAC_VITA_ANM2_SMALL_SEGMENT_BYTES
            : ISAAC_VITA_ANM2_LARGE_SEGMENT_BYTES;
        if (isaac_vita_anm2_scratch_malloc(
                owners[index], size, 0x82000000U, 0x82400000U,
                &decision) !=
                    ISAAC_VITA_ANM2_SCRATCH_HANDLED ||
            !decision.pointer)
            return 1;
        pointers[index] = decision.pointer;
    }
    for (index = 0U; index < ISAAC_VITA_ANM2_SEGMENT_COUNT; ++index) {
        if (isaac_vita_anm2_scratch_free(
                pointers[index], &decision) !=
            ISAAC_VITA_ANM2_SCRATCH_HANDLED)
            return 2;
    }
    return 0;
}
