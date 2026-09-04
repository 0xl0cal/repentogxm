#ifndef ISAAC_HOST_VITA_GL_H
#define ISAAC_HOST_VITA_GL_H

#include <stdint.h>

#include "guest.h"

/* Synthetic handles copied from the working PC guest boundary.  They are
 * deliberately outside both the relocated PE and the generated 0x7e......
 * GL-token family, so neither can be mistaken for guest code. */
#define ISAAC_VITA_GL_OPENGL32_TOKEN UINT32_C(0x7f000002)
#define ISAAC_VITA_GL_WGL_GET_PROC_TOKEN UINT32_C(0x7d100001)

#define ISAAC_VITA_GL_LOAD_LIBRARY_NAME \
    "KERNEL32.dll!LoadLibraryA"
#define ISAAC_VITA_GL_LOAD_LIBRARY_IAT_RVA       0x00606124U
#define ISAAC_VITA_GL_LOAD_LIBRARY_CALL_RVA      0x005990a7U
#define ISAAC_VITA_GL_LOAD_LIBRARY_RETURN_RVA    0x005990adU

/* Exact case-insensitive module whitelist used by epoxy.  All four names map
 * to one synthetic module because vitaGL owns the only GL implementation on
 * this target.  In particular, libEGL is not part of this family. */
#define ISAAC_VITA_GL_MODULE_ALIAS_COUNT          4U
#define ISAAC_VITA_GL_OPENGL32_NAME_RVA          0x00764a4cU
#define ISAAC_VITA_GL_OPENGL32_NAME              "OPENGL32"
#define ISAAC_VITA_GL_OPENGL32_DLL_NAME          "opengl32.dll"
#define ISAAC_VITA_GL_GLES2_NAME_RVA              0x00764a84U
#define ISAAC_VITA_GL_GLES2_NAME                  "libGLESv2.so.2"
#define ISAAC_VITA_GL_GLES1_NAME_RVA              0x00764a94U
#define ISAAC_VITA_GL_GLES1_NAME                  "libGLESv1_CM.so.1"

/* Frozen epoxy candidate order.  These are call-site/cache RVAs, not host
 * addresses; the generated guest rebases each through GUEST_IMAGE_BASE. */
#define ISAAC_VITA_GL_DESKTOP_MAIN_CALL_RVA       0x00570158U
#define ISAAC_VITA_GL_DESKTOP_MAIN_RETURN_RVA     0x0057015dU
#define ISAAC_VITA_GL_DESKTOP_CACHE_RVA           0x00802ff8U
#define ISAAC_VITA_GL_GLES1_MAIN_CALL_RVA         0x00573980U
#define ISAAC_VITA_GL_GLES1_MAIN_RETURN_RVA       0x00573985U
#define ISAAC_VITA_GL_GLES1_CACHE_RVA             0x00803000U
#define ISAAC_VITA_GL_GLES2_MAIN_CALL_RVA         0x005739bdU
#define ISAAC_VITA_GL_GLES2_MAIN_RETURN_RVA       0x005739c2U
#define ISAAC_VITA_GL_GLES2_CACHE_RVA             0x00803004U

#define ISAAC_VITA_GL_GET_PROC_NAME \
    "KERNEL32.dll!GetProcAddress"
#define ISAAC_VITA_GL_GET_PROC_IAT_RVA           0x00606118U
#define ISAAC_VITA_GL_GET_PROC_CALL_RVA          0x005990bcU
#define ISAAC_VITA_GL_GET_PROC_RETURN_RVA        0x005990c2U

/* Terminal GLES2 fallback after the main candidates.  It uses the same
 * module cache but has its own direct LoadLibraryA/GetProcAddress sites. */
#define ISAAC_VITA_GL_DIRECT_FALLBACK_FUNCTION_RVA 0x00599330U
#define ISAAC_VITA_GL_DIRECT_FALLBACK_CALL_RVA     0x00573a70U
#define ISAAC_VITA_GL_DIRECT_FALLBACK_RETURN_RVA   0x00573a75U
#define ISAAC_VITA_GL_DIRECT_FALLBACK_CACHE_RVA    0x00803004U
#define ISAAC_VITA_GL_DIRECT_LOAD_CALL_RVA         0x00599368U
#define ISAAC_VITA_GL_DIRECT_LOAD_RETURN_RVA       0x0059936eU
#define ISAAC_VITA_GL_DIRECT_GET_PROC_CALL_RVA     0x00599379U
#define ISAAC_VITA_GL_DIRECT_GET_PROC_RETURN_RVA   0x0059937fU

#define ISAAC_VITA_GL_FREE_LIBRARY_NAME \
    "KERNEL32.dll!FreeLibrary"
#define ISAAC_VITA_GL_FREE_LIBRARY_IAT_RVA       0x00606104U

#define ISAAC_VITA_GL_WGL_GET_PROC_NAME \
    "OPENGL32.dll!wglGetProcAddress"
#define ISAAC_VITA_GL_WGL_GET_PROC_IAT_RVA       0x006062fcU
#define ISAAC_VITA_GL_WGL_ENABLE_CALL_RVA        0x00561d78U
#define ISAAC_VITA_GL_WGL_ENABLE_RETURN_RVA      0x00561d7eU
#define ISAAC_VITA_GL_WGL_DISABLE_CALL_RVA       0x00561e01U
#define ISAAC_VITA_GL_WGL_DISABLE_RETURN_RVA     0x00561e07U
#define ISAAC_VITA_GL_SWAP_INTERVAL_NAME_RVA     0x0075d344U
#define ISAAC_VITA_GL_SWAP_INTERVAL_NAME         "wglSwapIntervalEXT"

#define ISAAC_VITA_GL_RESOLVER_FUNCTION_RVA      0x00599070U
#define ISAAC_VITA_GL_PROCEDURE_NAME_MAX         63U
#define ISAAC_VITA_GL_IMPORT_COUNT               4U

/* Common KERNEL32 names are claimed only when their arguments identify the
 * synthetic OPENGL32 module.  Returning zero leaves CPU state and call_count
 * untouched, allowing the existing startup/sync handlers to own their frames. */
int isaac_vita_gl_import(CPU *__restrict c, const char *name);
int isaac_vita_gl_import_counted(CPU *__restrict c, const char *name,
                                 unsigned *call_count);

/* Dynamic calls are counted before entering either the WGL resolver or the
 * typed GL adapter, so an unsupported-backend guest_fault remains visible. */
int isaac_vita_gl_dynamic(CPU *__restrict c, uint32_t token);
int isaac_vita_gl_dynamic_counted(CPU *__restrict c, uint32_t token,
                                  unsigned *call_count);

#endif
