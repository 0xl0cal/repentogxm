#ifndef ISAAC_VITA_FIXED_IMAGE_H
#define ISAAC_VITA_FIXED_IMAGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime platform hooks used only for the relocated PE image. */
void *isaac_vita_fixed_alloc(uint32_t address, uint32_t size);
void  isaac_vita_fixed_free(void *address);

#ifdef __cplusplus
}
#endif

#endif
