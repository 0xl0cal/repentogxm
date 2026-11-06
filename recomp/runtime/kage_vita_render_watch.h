#ifndef KAGE_VITA_RENDER_WATCH_H
#define KAGE_VITA_RENDER_WATCH_H

/* Render residual watch list (profile builds only).
 *
 * The translated functions the render-60 plan leaves in the seeded
 * Downpour render after every selected lever: the KAGE sprite chain, the game
 * render logic around it, the Lua callback path that the chain view leaves
 * unrooted, and the translated GL wrappers.  One list, two consumers:
 *
 *   ISAAC_VITA_GUEST_SAMPLER_RESIDUAL       ph120.hotw: self / caller /
 *       inclusive / root counts of every entry, every window, whatever its
 *       rank (ph120.hot/hotc keep 12, hoti/hotr 36 per window).
 *   ISAAC_VITA_PROFILE_FUNCTION_ENTRY_CENSUS ph120.fe: translated body
 *       entries per entry and phase.  recomp/vita/vita_render_watch.py maps
 *       each RVA to its coverage function id in the guest_coverage.json of
 *       the corpus being compiled (configure time) and refuses an RVA that is
 *       not a coverage function.
 *
 * Order is the report order.  At most 64 entries (KAGE_VITA_RENDER_WATCH_MAX);
 * the list holds 64 since the render-60 route survey.
 * Names only where a report or source names the function; the rest are
 * callees of named ones seen in the 2026-10-25 seeded A2S chain records. */
#define KAGE_VITA_RENDER_WATCH_MAX 64U

#define KAGE_VITA_RENDER_WATCH(X)                                            \
    /* render roots */                                                       \
    X(0x0048bc50u) /* first chain root of the sampler */                     \
    X(0x004b0600u) /* hosts the inline KAGE present seam */                  \
    X(0x002ce770u)                                                           \
    X(0x00560eb0u) /* manual-hook root */                                    \
    X(0x005689d0u)                                                           \
    /* game render logic (measured: 2.20 ms) */                              \
    X(0x003ca670u) /* Room render pass */                                    \
    X(0x003cad40u) /* Room::Render */                                        \
    X(0x003dbfe0u)                                                           \
    X(0x00014a90u) /* entity render pass */                                  \
    X(0x00006dd0u)                                                           \
    X(0x001e9810u) /* Entity_Effect::Render */                               \
    X(0x00504610u) /* HUD */                                                 \
    X(0x00508710u) /* HUD */                                                 \
    X(0x00005190u) /* ANM2 render */                                         \
    X(0x000051e0u) /* ANM2 render */                                         \
    X(0x00005250u) /* ANM2::RenderLayer */                                   \
    /* KAGE sprite chain (measured: 3.64 ms) */                              \
    X(0x00003e50u) /* KAGE RenderFrame */                                    \
    X(0x00004760u)                                                           \
    X(0x00004b60u) /* GetDestQuad */                                         \
    X(0x00003540u)                                                           \
    X(0x00002170u)                                                           \
    X(0x00002290u)                                                           \
    X(0x00007790u) /* refcount */                                            \
    X(0x00007af0u) /* refcount */                                            \
    X(0x00007bb0u)                                                           \
    X(0x003a7d50u)                                                           \
    X(0x0055e770u)                                                           \
    X(0x0055f610u) /* Image::Render entry */                                 \
    X(0x0055f630u) /* Image::Render body */                                  \
    X(0x0025af60u)                                                           \
    X(0x0025b0f0u)                                                           \
    X(0x0055f990u) /* PushQuad */                                            \
    X(0x005603f0u) /* batch lookup */                                        \
    X(0x005607a0u) /* blend and batch select */                              \
    X(0x0031ed10u) /* vector assign (batch miss path) */                     \
    X(0x005ec358u) /* memmove import thunk */                                \
    X(0x00567620u)                                                           \
    /* translated GL wrappers (measured: 0.65 ms) */                         \
    X(0x0056d500u)                                                           \
    X(0x0056d8c0u)                                                           \
    X(0x00564f50u) /* clear wrapper */                                       \
    X(0x00562e00u)                                                           \
    X(0x00562ec0u)                                                           \
    X(0x00567430u) /* Shader SetUniformInt */                                \
    X(0x00565240u)                                                           \
    X(0x005671d0u)                                                           \
    /* Lua callback path (measured: unrooted 2.02 ms) */                     \
    X(0x0040eef0u) /* Lua mod-callback invoker */                            \
    X(0x003ffe10u) /* LuaEngine::PostRender */                               \
    X(0x003f8e30u) /* LuaBridge Userdata::getClass */                        \
    X(0x00439e80u)                                                           \
    X(0x00439e20u)                                                           \
    X(0x002daef0u)                                                           \
    X(0x000206b0u) /* HasCollectible chain */                                \
    /* route survey additions (52 -> 64: full, swap to add more) */         \
    /* render logic */                                                       \
    X(0x002dbe40u) /* GridEntity thunk, jmp 6dd0 (fe(6dd0) - fe(this)) */    \
    X(0x000078a0u) /* SmartPointer ctor */                                   \
    X(0x005ec3b2u) /* floor thunk */                                         \
    X(0x0028e560u) /* GetWaterClipInfo */                                    \
    /* KAGE offload stage 2 */                                               \
    X(0x00560260u) /* Image::flush */                                        \
    X(0x00567490u) /* Shader typed uniform set (vtable +0x20) */             \
    /* update denominators, ph120.fe upd */                                  \
    X(0x00014f80u) /* EntityList partition */                                \
    X(0x00016980u) /* callee of 14f80 */                                     \
    X(0x00016f10u) /* callee of 14f80 */                                     \
    X(0x001b79c0u) /* Entity_Effect::Init */                                 \
    X(0x00008be0u) /* anm2 cache hit path */                                 \
    X(0x00563120u) /* GetMountedFilePath */

#endif
