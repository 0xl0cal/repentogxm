#ifndef KAGE_VITA_CONTINUE_PROFILE_TEST_PLATFORM_H
#define KAGE_VITA_CONTINUE_PROFILE_TEST_PLATFORM_H

#include <stdint.h>

typedef uint8_t GLboolean;

#define GL_FALSE ((GLboolean)0)

int      sceKernelGetThreadId(void);
uint64_t sceKernelGetProcessTimeWide(void);
int      sceClibPrintf(const char *format, ...);

#if defined(ISAAC_VITA_CONTINUE_OVERLAY)
int      sceGxmDisplayQueueFinish(void);
void     vglSetDisplayCallback(void (*callback)(void *framebuffer));
void     vglSwapBuffers(GLboolean has_common_dialog);
int      kage_vita_loading_active(void);
#endif

#endif
