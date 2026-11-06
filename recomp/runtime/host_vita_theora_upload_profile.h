#ifndef ISAAC_THEORA_UPLOAD_PROFILE_H
#define ISAAC_THEORA_UPLOAD_PROFILE_H
#include <stdint.h>
#if defined(ISAAC_VITA_THEORA_CADENCE_PROFILE) && ISAAC_VITA_THEORA_CADENCE_PROFILE
/* UINT64_MAX means outside a profiled video fetch. Native GL types stay in
 * gl_vita_backend.c; these are diagnostic metadata, not a foreign GL ABI. */
uint64_t isaac_vita_theora_subupload_begin(
    uint32_t width, uint32_t height, uint32_t format, uint32_t type);
void isaac_vita_theora_subupload_end(uint64_t begin);
int isaac_vita_theora_subupload_layout_needed(void);
void isaac_vita_theora_subupload_layout(
    uint32_t format, uint32_t width, uint32_t height, uint32_t texture_type);
#endif
#endif
