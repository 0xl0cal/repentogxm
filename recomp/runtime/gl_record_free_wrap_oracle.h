/* Host oracle stand-ins for gl_record_free_wrap.c: the SceUID type and the
 * two calls the wrap makes, both recorded by gl_record_free_wrap_oracle.c. */
#ifndef GL_RECORD_FREE_WRAP_ORACLE_H
#define GL_RECORD_FREE_WRAP_ORACLE_H

#include <stdint.h>

typedef int32_t SceUID;   /* as the in-tree stand-in kage_vita_backend_test_vitagl.h:31 */

void isaac_gl_record_memblock_freeing(void);

#endif
