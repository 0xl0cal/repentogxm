#ifndef KAGE_VITA_BACKEND_TEST_VITAGL_H
#define KAGE_VITA_BACKEND_TEST_VITAGL_H

#include <stddef.h>
#include <stdint.h>

/* Exact Vita/GL surface used by kage_vita_backend.c.  The executable host
 * oracle supplies these functions; production includes VitaSDK headers. */
typedef uint8_t GLboolean;
typedef int32_t GLsizei;
typedef uint32_t GLenum;
typedef uint8_t GLubyte;

#define GL_FALSE ((GLboolean)0)
#define GL_TRUE  ((GLboolean)1)
#define GL_VERSION ((GLenum)0x1f02u)

#define SHARK_OPT_UNSAFE 0
#define SHARK_ENABLE 1
#define SCE_GXM_MULTISAMPLE_NONE 0

typedef enum vglMemType {
    VGL_MEM_VRAM = 0,
    VGL_MEM_RAM = 1,
    VGL_MEM_PHYCONT = 2,
    VGL_MEM_BUDGET = 3,
    VGL_MEM_EXTERNAL = 4,
    VGL_MEM_ALL = 5
} vglMemType;

typedef int32_t SceUID;
typedef struct SceKernelFreeMemorySizeInfo {
    int size;
    int size_user;
    int size_cdram;
    int size_phycont;
} SceKernelFreeMemorySizeInfo;

typedef struct SceGxmRenderTargetParams {
    uint32_t flags;
    uint16_t width;
    uint16_t height;
    uint16_t scenesPerFrame;
    uint16_t multisampleMode;
    uint32_t multisampleLocations;
    SceUID driverMemBlock;
} SceGxmRenderTargetParams;

/* Deterministic public-API mirrors for the host and softfp behavior oracles.
 * The GXM query intentionally rejects anything except the production params. */
static inline int sceKernelGetFreeMemorySize(
    SceKernelFreeMemorySizeInfo *info)
{
    if (!info || info->size != (int)sizeof *info)
        return (int)0x80020005u;
    /* Exercise the formatter's absolute 32-bit bound as well as the query. */
    info->size_user = -1;
    info->size_cdram = -1;
    info->size_phycont = -1;
    return 0;
}

static inline size_t vglMemFree(vglMemType type)
{
    (void)type;
    return UINT32_MAX;
}

static inline size_t vglMemTotal(vglMemType type)
{
    (void)type;
    return UINT32_MAX;
}

static inline int sceGxmGetRenderTargetMemSize(
    const SceGxmRenderTargetParams *params, unsigned int *driver_mem_size)
{
    if (!params || !driver_mem_size || params->flags != 0u ||
            params->width != 1024u || params->height != 1024u ||
            params->scenesPerFrame != 8u ||
            params->multisampleMode != SCE_GXM_MULTISAMPLE_NONE ||
            params->multisampleLocations != 0u ||
            params->driverMemBlock != (SceUID)-1)
        return (int)0x805b0004u;
    *driver_mem_size = UINT32_MAX;
    return 0;
}

int      sceClibPrintf(const char *format, ...);
uint64_t sceKernelGetProcessTimeWide(void);
void     vglSetupRuntimeShaderCompiler(
             int optimization, int vertex, int fragment, int compiler);
void     vglSetupDisplayRenderTarget(uint8_t scenes_per_frame);
GLboolean vglInitExtended(
              int legacy_pool, int width, int height,
              int ram_threshold, int multisample_mode);
GLboolean vglInitWithCustomThreshold(
              int legacy_pool, int width, int height,
              int ram_threshold, int cdram_threshold,
              int phycont_threshold, int cdlg_threshold,
              int multisample_mode);
const GLubyte *glGetString(GLenum name);
void     glViewport(int x, int y, GLsizei width, GLsizei height);
void     vglWaitVblankStart(GLboolean enabled);
void     vglSwapBuffers(GLboolean has_common_dialog);

#endif
