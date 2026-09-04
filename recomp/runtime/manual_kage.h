#ifndef MANUAL_KAGE_H
#define MANUAL_KAGE_H

#include "guest.h"

/* One source of truth for the high-level KAGE platform entry points.  The
 * generator reads this X-macro list too: changing the interface deliberately
 * invalidates generated objects, while changing manual_kage.c only requires a
 * runtime relink. */
#define GUEST_MANUAL_KAGE_FUNCTIONS(X) \
    X(00481280, 0x00481280u, SetVSync) \
    X(00560c60, 0x00560c60u, Initialize) \
    X(00560e30, 0x00560e30u, Shutdown) \
    X(00560eb0, 0x00560eb0u, Present) \
    X(00560f20, 0x00560f20u, GetFramebufferWidth) \
    X(00560f90, 0x00560f90u, GetFramebufferHeight) \
    X(00561830, 0x00561830u, InitializeRenderDisplay) \
    X(0056dd70, 0x0056dd70u, SoundInitialize) \
    X(005700e0, 0x005700e0u, GlProviderResolver)

#define GUEST_KAGE_INITIALIZE_RVA 0x00560c60u
#define GUEST_KAGE_INITIALIZE_FAULT \
    "KAGE graphics backend unavailable: Initialize"
#define GUEST_KAGE_RENDER_DISPLAY_RVA 0x00561830u
#define GUEST_KAGE_RENDER_DISPLAY_FAULT \
    "KAGE graphics backend unavailable: InitializeRenderDisplay"
#define GUEST_KAGE_SOUND_INITIALIZE_RVA 0x0056dd70u
#define GUEST_KAGE_SOUND_INITIALIZE_FAULT \
    "KAGE audio backend unavailable: Initialize"
#define GUEST_KAGE_VSYNC_RVA 0x00481280u
#define GUEST_KAGE_VSYNC_FAULT \
    "KAGE graphics backend unavailable: SetVSync"
#define GUEST_KAGE_GL_PROVIDER_RESOLVER_RVA 0x005700e0u
#define GUEST_KAGE_GL_PROVIDER_RESOLVER_FAULT \
    "KAGE Vita GL provider resolver: symbol outside typed registry"

#define GUEST_DECLARE_KAGE(symbol, rva, label) \
    void sub_##symbol(CPU *__restrict c); \
    void guest_original_##symbol(CPU *__restrict c);
GUEST_MANUAL_KAGE_FUNCTIONS(GUEST_DECLARE_KAGE)
#undef GUEST_DECLARE_KAGE

/* The original SetVSync keeps a requested setting but disables the session
 * state when the current integer refresh rate is more than 0.04 of a 60 Hz
 * multiple away.  Exposed for the focused platform oracle. */
int guest_kage_vsync_policy_allows(uint32_t refresh_hz);

#endif
