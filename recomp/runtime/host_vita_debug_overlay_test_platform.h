/* Host oracle stand-ins for the SDK surface host_vita_debug_overlay.c uses.
 * Field names and widths follow vita-headers (psp2/kernel/sysmem.h,
 * psp2common/kernel/threadmgr.h, psp2common/kernel/modulemgr.h, vitaGL.h,
 * newlib malloc.h); only the fields the overlay reads are kept in front of
 * padding, so a renamed field fails to compile here and on the device. */
#ifndef HOST_VITA_DEBUG_OVERLAY_TEST_PLATFORM_H
#define HOST_VITA_DEBUG_OVERLAY_TEST_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

typedef int32_t SceUID;
typedef uint32_t SceSize;
typedef uint32_t SceUInt32;
typedef int32_t SceInt32;
typedef uint64_t SceUInt64;
typedef uint64_t SceKernelSysClock;
typedef uint32_t SceKernelMemBlockType;
typedef struct SceKernelAllocMemBlockOpt { SceSize size; uint8_t pad[0x3c]; }
    SceKernelAllocMemBlockOpt;
typedef int (*SceKernelThreadEntry)(SceSize args, void *argp);
typedef struct SceKernelThreadOptParam { SceSize size; uint8_t pad[0x18]; }
    SceKernelThreadOptParam;

typedef struct SceKernelFreeMemorySizeInfo {
    int size;
    int size_user;
    int size_cdram;
    int size_phycont;
} SceKernelFreeMemorySizeInfo;

typedef struct SceKernelMemBlockInfo {
    SceSize size;
    void *mappedBase;
    SceSize mappedSize;
    int memoryType;
    SceUInt32 access;
    SceKernelMemBlockType type;
} SceKernelMemBlockInfo;

typedef struct SceKernelThreadInfo {
    SceSize size;
    SceUID processId;
    char name[32];
    SceUInt32 attr;
    SceUInt32 status;
    SceKernelThreadEntry entry;
    void *stack;
    SceInt32 stackSize;
    SceInt32 initPriority;
    SceInt32 currentPriority;
    SceInt32 initCpuAffinityMask;
    SceInt32 currentCpuAffinityMask;
    SceInt32 currentCpuId;
    SceInt32 lastExecutedCpuId;
    SceUInt32 waitType;
    SceUID waitId;
    SceInt32 exitStatus;
    SceKernelSysClock runClocks;
    SceUInt32 intrPreemptCount;
    SceUInt32 threadPreemptCount;
    SceUInt32 threadReleaseCount;
    SceInt32 changeCpuCount;
    SceInt32 fNotifyCallback;
    SceInt32 reserved;
} SceKernelThreadInfo;

typedef struct SceKernelSystemInfo {
    SceSize size;
    SceUInt32 activeCpuMask;
    struct {
        SceKernelSysClock idleClock;
        SceUInt32 comesOutOfIdleCount;
        SceUInt32 threadSwitchCount;
    } cpuInfo[4];
} SceKernelSystemInfo;

typedef struct SceKernelSegmentInfo {
    SceSize size;
    SceUInt32 perms;
    void *vaddr;
    SceSize memsz;
    SceSize filesz;
    SceUInt32 res;
} SceKernelSegmentInfo;

typedef struct SceKernelModuleInfo {
    SceSize size;
    SceUID modid;
    uint8_t pad[0x154 - 8];
    SceKernelSegmentInfo segments[4];
    SceUInt32 state;
} SceKernelModuleInfo;

typedef enum vglMemType {
    VGL_MEM_VRAM = 0,
    VGL_MEM_RAM,
    VGL_MEM_PHYCONT,
    VGL_MEM_BUDGET,
    VGL_MEM_EXTERNAL,
    VGL_MEM_ALL
} vglMemType;

struct mallinfo {
    size_t arena;
    size_t ordblks;
    size_t smblks;
    size_t hblks;
    size_t hblkhd;
    size_t usmblks;
    size_t fsmblks;
    size_t uordblks;
    size_t fordblks;
    size_t keepcost;
};

uint64_t sceKernelGetProcessTimeWide(void);
int sceKernelDelayThread(SceUInt32 delay);
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *info);
int sceKernelGetMemBlockBase(SceUID uid, void **base);
int sceKernelGetMemBlockInfoByAddr(void *base, SceKernelMemBlockInfo *info);
int sceKernelGetThreadId(void);
int sceKernelGetThreadInfo(SceUID thid, SceKernelThreadInfo *info);
int sceKernelGetSystemInfo(SceKernelSystemInfo *info);
SceUID sceKernelGetModuleIdByAddr(void *addr);
int sceKernelGetModuleInfo(SceUID uid, SceKernelModuleInfo *info);
size_t vglMemFree(vglMemType type);
size_t vglMemTotal(vglMemType type);
void vglSetDisplayCallback(void (*callback)(void *framebuffer));
struct mallinfo mallinfo(void);

#endif
