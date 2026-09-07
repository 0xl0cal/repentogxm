#ifndef ISAAC_NATIVE_RESOURCE_PROFILE_INTERNAL_H
#define ISAAC_NATIVE_RESOURCE_PROFILE_INTERNAL_H

#include "isaac_native_resource_profile.h"
#if !defined(HAVE_SINGLE_THREADED_GC) || defined(HAVE_PTHREAD) || \
    defined(HAVE_SHARED_RENDERTARGETS) || defined(DEPTH_STENCIL_HACK)
#error "Isaac resource profile requires the pinned single-owner, nonshared RT/depth recipe"
#endif

#ifdef __cplusplus
extern "C" {
#endif
void isaacNativeResourceEvent(unsigned kind, uint32_t w, uint32_t h,
                              uint64_t elapsed, int failed);
void isaacNativeResourceSceneBegin(uint32_t w, uint32_t h, int result);
void isaacNativeResourceSceneEnd(uint32_t elapsed);
void isaacNativeResourceDepth(void *data, uint32_t w, uint32_t h,
                              uint64_t elapsed, int failed);
void isaacNativeResourceGcObject(void);
void isaacNativeResourceRecovery(unsigned kind, uint32_t bytes,
                                 uint64_t elapsed, int failed);
void *isaacNativeResourceRetireRT(void *rt);
void *isaacNativeResourceRetireDepth(void *data);
void isaacNativeResourceFinish(SceGxmContext *context);
int isaacNativeResourceCreateRT(const SceGxmRenderTargetParams *params,
                                SceGxmRenderTarget **target);
int isaacNativeResourceDestroyRT(SceGxmRenderTarget *target);
#ifdef __cplusplus
}
#endif

/* SDK declarations have already been read by shared.h. Only calls in the
 * optional native build are redirected; wrappers preserve return values. */
#define sceGxmFinish isaacNativeResourceFinish
#define sceGxmCreateRenderTarget isaacNativeResourceCreateRT
#define sceGxmDestroyRenderTarget isaacNativeResourceDestroyRT
#endif
