#ifndef ISAAC_GL_OWNER_CHECK_H
#define ISAAC_GL_OWNER_CHECK_H
#include <stdint.h>
#if defined(ISAAC_VITA_GL_OWNER_CHECK) && ISAAC_VITA_GL_OWNER_CHECK
/* The render thread claims vitaGL ownership before its first call; report()
 * logs a summary when the violation count changed (call once per present). */
void isaac_gl_owner_claim(void);
void isaac_gl_owner_report(void);
uint32_t isaac_gl_owner_violations(void);
#endif
#endif
