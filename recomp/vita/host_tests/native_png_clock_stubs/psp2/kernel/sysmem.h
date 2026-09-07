#ifndef ISAAC_NP_TEST_SYSMEM_H
#define ISAAC_NP_TEST_SYSMEM_H
#include <stdint.h>
typedef int32_t SceUID;
typedef uint32_t SceSize;
/* Fixture-only discriminator, never passed to the OS or used by production. */
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RW 1
SceUID sceKernelAllocMemBlock(const char *, int, SceSize, const void *);
int sceKernelGetMemBlockBase(SceUID, void **);
int sceKernelFreeMemBlock(SceUID);
#endif
