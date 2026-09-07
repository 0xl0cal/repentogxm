/* Mocks only owner dependencies. Included by the actual-mask backend fixture. */
#define GL_FALSE 0u
static unsigned s_attrib_owner_swaps, s_attrib_owner_bad;
static uint16_t s_attrib_owner_expected;
static int s_active, s_overlay_active;
static uint32_t s_overlay_phase, s_overlay_swaps;
#define CONTINUE_OVERLAY_ACTIVE 0x80000000u
#define CONTINUE_OVERLAY_KIND_RUN 0x40000000u
#define CONTINUE_OVERLAY_STAGE_MASK 0xffu
#define CONTINUE_OVERLAY_PHASE_SHIFT 16u
#define CONTINUE_PROFILE_FAULT_OVERLAY 1u
#define KAGE_VITA_STALL_STOP() ((void)0)
#define KAGE_VITA_FULLSPEED_DEACTIVATE() ((void)0)
#define KAGE_VITA_STABLE30_DEACTIVATE() ((void)0)
#define KAGE_VITA_STALL_TRACE_SYNC(a,b) ((void)(b))
#define kage_vita_loading_finish() ((void)0)
#define kage_vita_input_deactivate() ((void)0)
#define kage_vita_loading_active() 0
static void continue_profile_fault(uint32_t *faults, uint32_t bit) { *faults |= bit; }
static void continue_overlay_store(uint32_t snapshot) { (void)snapshot; }
static void continue_overlay_callback(void *buffer) { (void)buffer; }
static void vglSetDisplayCallback(void (*callback)(void *)) { (void)callback; }
static void vglSwapBuffers(GLboolean dialog) {
    (void)dialog;
    ++s_attrib_owner_swaps;
    if (s_native_attrib_mask != s_attrib_owner_expected || gl_vita_backend_attrib_pending())
        ++s_attrib_owner_bad;
}
#include "gl_vita_attrib_boundaries_generated.inc"
#undef kage_vita_loading_finish
#undef kage_vita_input_deactivate
#undef kage_vita_loading_active
#undef KAGE_VITA_STALL_STOP
#undef KAGE_VITA_FULLSPEED_DEACTIVATE
#undef KAGE_VITA_STABLE30_DEACTIVATE
#undef KAGE_VITA_STALL_TRACE_SYNC
