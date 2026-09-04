#ifndef ISAAC_ANM2_ORACLE_PSP2_KERNEL_SYSMEM_H
#define ISAAC_ANM2_ORACLE_PSP2_KERNEL_SYSMEM_H

#include <stdint.h>

typedef int32_t SceUID;
typedef uint32_t SceSize;

typedef enum SceKernelMemBlockType {
    SCE_KERNEL_MEMBLOCK_TYPE_USER_RW = 0x0c20d060
} SceKernelMemBlockType;

typedef struct SceKernelAllocMemBlockOpt {
    SceSize size;
    uint32_t attr;
    SceSize alignment;
    uint32_t uidBaseBlock;
    const char *strBaseBlockName;
    int flags;
    int reserved[10];
} SceKernelAllocMemBlockOpt;

SceUID sceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type,
                              SceSize size,
                              SceKernelAllocMemBlockOpt *option);
int sceKernelGetMemBlockBase(SceUID uid, void **base);
int sceKernelFreeMemBlock(SceUID uid);

#endif
