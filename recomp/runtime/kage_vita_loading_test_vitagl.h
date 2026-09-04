#ifndef KAGE_VITA_LOADING_TEST_VITAGL_H
#define KAGE_VITA_LOADING_TEST_VITAGL_H

#include <stdint.h>

/* Exact function surface used by kage_vita_loading.c.  The executable host
 * oracle supplies these functions; production includes VitaSDK headers. */
typedef uint8_t GLboolean;

#define GL_FALSE ((GLboolean)0)

int      sceKernelGetThreadId(void);
uint64_t sceKernelGetProcessTimeWide(void);
int      sceGxmDisplayQueueFinish(void);
int      sceClibPrintf(const char *format, ...);
void     vglSetDisplayCallback(void (*callback)(void *framebuffer));
void     vglSwapBuffers(GLboolean has_common_dialog);

#endif
