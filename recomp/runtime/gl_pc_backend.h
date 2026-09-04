/* Explicit 32-bit PC binding for the generated guest OpenGL surface. */
#ifndef REPENTOGXM_GL_PC_BACKEND_H
#define REPENTOGXM_GL_PC_BACKEND_H

#include <stddef.h>

/* Requires a ready KAGE WGL context.  The call is deliberately explicit: no
 * ordinary entry/frontier run gets a graphics backend merely by linking this
 * file.  A partial native surface is installed with missing callbacks left
 * NULL, preserving the generic bridge's exact named fault. */
int         gl_pc_backend_install(void);
void        gl_pc_backend_uninstall(void);
int         gl_pc_backend_installed(void);
int         gl_pc_backend_complete(void);
size_t      gl_pc_backend_resolved_count(void);
size_t      gl_pc_backend_missing_count(void);
const char *gl_pc_backend_missing_symbol(size_t index);
const char *gl_pc_backend_last_error(void);

#endif
