/* High-level KAGE platform boundary.
 *
 * Default execution remains a loud, deterministic stop.  Explicit resolver
 * inventory still executes the preserved x86 originals.  A third, explicit PC
 * mode crosses into a real WGL backend without making the guest emulate GLFW,
 * USER32, GDI or WGL one imported function at a time.  The same explicit mode
 * takes the game's native "audio unavailable" path at Sound::Manager::Initialize
 * instead of fabricating OpenAL handles or growing 24 temporary import shims. */
#include "manual_kage.h"
#include "gl_bridge.h"
#include "gl_pc_backend.h"
#include "kage_pc_backend.h"
#include "guest_coverage_generated.h"

/* Generated in the full runtime and supplied by a strict test double in the
 * focused PC smoke.  This is the game-owned render-target allocator called by
 * the original InitializeRenderDisplay tail. */
void sub_00565030(CPU *__restrict c);

/* Exact live singleton in this PE.  Checking it before reading offsets keeps
 * an opt-in smoke with a malformed ECX inside the controlled fault boundary. */
#define KAGE_MANAGER_RVA       0x007c7a30u
#define KAGE_MANAGER_ADDRESS   (GUEST_IMAGE_BASE + KAGE_MANAGER_RVA)
#define KAGE_FRAMEBUFFER_OFFSET 0x000000c8u
#define KAGE_RENDERBUFFER_OFFSET 0x000000ccu
#define KAGE_BACKEND_OFFSET    0x000000d8u
#define KAGE_WIDTH_OFFSET      0x000000e0u
#define KAGE_HEIGHT_OFFSET     0x000000e4u
#define KAGE_DEFAULT_WIDTH     960u
#define KAGE_DEFAULT_HEIGHT    540u

#define KAGE_GL_FRAMEBUFFER    0x00008d40u

/* OptionsConfig::SetVSync stores the requested value here before crossing
 * into the GLFW monitor/swap-control path.  In explicit PC mode the native
 * backend owns that policy, while this global remains the guest-visible
 * current state used by later option changes. */
#define OPTIONS_VSYNC_OFFSET   0x00000055u
#define KAGE_VSYNC_STATE       0x307c7b0du

/* Exact KAGE::Sound::Manager singleton and fields used by sub_0056dd70.  Its
 * caller ignores AL, and the original alcOpenDevice(NULL) failure path leaves
 * the manager uninitialised and continues startup. */
#define KAGE_SOUND_MANAGER_ADDRESS  0x307e7d40u
#define KAGE_SOUND_RUNNING          0x00000004u
#define KAGE_SOUND_SUSPENDED        0x00000005u
#define KAGE_SOUND_CONTEXT          0x00000030u
#define KAGE_SOUND_DEVICE           0x00000034u

/* Exact GetClientRect caches used by the preserved implementations.  The
 * width query publishes RECT.bottom before RECT.right; the height query
 * publishes RECT.right before RECT.bottom.  Several unrelated callers read
 * these four globals directly, so returning EAX alone is not equivalent. */
#define KAGE_WIDTH_CACHE       0x308071a0u
#define KAGE_HEIGHT_CACHE      0x308071a4u
#define KAGE_HEIGHT_COMPANION  0x308071acu
#define KAGE_WIDTH_COMPANION   0x308071a8u

static int use_original(CPU *__restrict c, guest_fn original)
{
    if (!g_guest_gl_inventory_mode)
        return 0;
    original(c);
    return 1;
}

int guest_kage_pc_handoff_ready(void)
{
    return kage_pc_backend_mode() != KAGE_PC_BACKEND_DISABLED &&
           kage_pc_backend_ready();
}

static int require_pc_mode(CPU *__restrict c, uint32_t rva,
                           const char *disabled_fault)
{
    if (kage_pc_backend_mode() != KAGE_PC_BACKEND_DISABLED)
        return 1;
    guest_fault(c, rva, disabled_fault);
    return 0;
}

static int manager_dimensions(CPU *__restrict c, uint32_t rva,
                              uint32_t *width, uint32_t *height)
{
    if (c->ecx != KAGE_MANAGER_ADDRESS) {
        guest_fault(c, rva, "KAGE PC backend: unexpected manager address");
        return 0;
    }
    *width = ld32(c->ecx + KAGE_WIDTH_OFFSET);
    *height = ld32(c->ecx + KAGE_HEIGHT_OFFSET);
    if (!*width)
        *width = KAGE_DEFAULT_WIDTH;
    if (!*height)
        *height = KAGE_DEFAULT_HEIGHT;
    return 1;
}

/* Cross the same generated stdcall adapters as translated game code.  This
 * keeps pointer-shaped arguments as guest addresses and makes stack cleanup a
 * checked part of the boundary instead of calling an untyped native FARPROC. */
static int typed_gl_call(CPU *__restrict c, uint32_t rva, const char *name,
                         const uint32_t *arguments, uint32_t argument_count)
{
    uint32_t saved = c->esp;
    uint32_t token = guest_gl_resolve(name);
    uint32_t index;

    if (!token) {
        guest_fault(c, rva, "KAGE PC backend: typed GL registry miss");
        return 0;
    }
    for (index = argument_count; index != 0u; --index)
        gpush(c, arguments[index - 1u]);
    gpush(c, 0xabc0f800u);
    if (!guest_gl_dispatch(c, token)) {
        (void)guest_stack_set(c, saved, rva);
        guest_fault(c, rva, "KAGE PC backend: typed GL dispatch miss");
        return 0;
    }
    if (c->fault) {
        (void)guest_stack_set(c, saved, rva);
        return 0;
    }
    if (c->esp != saved) {
        (void)guest_stack_set(c, saved, rva);
        guest_fault(c, rva, "KAGE PC backend: typed GL ABI drift");
        return 0;
    }
    return 1;
}

static int call_render_target_allocator(CPU *__restrict c, uint32_t rva,
                                        uint32_t manager)
{
    uint32_t saved = c->esp;

    gpush(c, 1u);                    /* exact force argument */
    gpush(c, 0xabc65030u);
    c->ecx = manager;
    sub_00565030(c);
    if (c->fault) {
        (void)guest_stack_set(c, saved, rva);
        return 0;
    }
    if (c->esp != saved) {
        (void)guest_stack_set(c, saved, rva);
        guest_fault(c, rva,
                    "KAGE PC backend: render-target allocator ABI drift");
        return 0;
    }
    return 1;
}

/* Exact non-GLFW tail of sub_00561830 (0x561a69..0x561ad3).  The manual
 * platform handoff replaces the GLFW window setup, but the game-owned FBO and
 * depth-renderbuffer lifecycle still has to run before later texture attach. */
static int initialize_render_targets(CPU *__restrict c, uint32_t rva,
                                     uint32_t manager)
{
    uint32_t framebuffer_address = manager + KAGE_FRAMEBUFFER_OFFSET;
    uint32_t renderbuffer_address = manager + KAGE_RENDERBUFFER_OFFSET;
    uint32_t arguments[2];

    arguments[0] = KAGE_GL_FRAMEBUFFER;
    arguments[1] = 0u;
    if (ld32(framebuffer_address) != 0u) {
        if (!typed_gl_call(c, rva, "glBindFramebuffer", arguments, 2u))
            return 0;
        arguments[0] = 1u;
        arguments[1] = framebuffer_address;
        if (!typed_gl_call(c, rva, "glDeleteFramebuffers", arguments, 2u))
            return 0;
    }

    arguments[0] = 1u;
    arguments[1] = framebuffer_address;
    if (!typed_gl_call(c, rva, "glGenFramebuffers", arguments, 2u))
        return 0;
    if (ld32(framebuffer_address) == 0u) {
        guest_fault(c, rva, "KAGE PC backend: glGenFramebuffers returned zero");
        return 0;
    }

    arguments[0] = KAGE_GL_FRAMEBUFFER;
    arguments[1] = ld32(framebuffer_address);
    if (!typed_gl_call(c, rva, "glBindFramebuffer", arguments, 2u) ||
        !call_render_target_allocator(c, rva, manager))
        return 0;
    if (ld32(renderbuffer_address) == 0u) {
        guest_fault(c, rva, "KAGE PC backend: renderbuffer allocator returned zero");
        return 0;
    }

    arguments[0] = KAGE_GL_FRAMEBUFFER;
    arguments[1] = 0u;
    return typed_gl_call(c, rva, "glBindFramebuffer", arguments, 2u);
}

static int destroy_render_targets(CPU *__restrict c, uint32_t rva,
                                  uint32_t manager)
{
    uint32_t framebuffer_address = manager + KAGE_FRAMEBUFFER_OFFSET;
    uint32_t renderbuffer_address = manager + KAGE_RENDERBUFFER_OFFSET;
    uint32_t arguments[2];
    int ok;

    arguments[0] = KAGE_GL_FRAMEBUFFER;
    arguments[1] = 0u;
    ok = typed_gl_call(c, rva, "glBindFramebuffer", arguments, 2u);
    if (ok && ld32(framebuffer_address) != 0u) {
        arguments[0] = 1u;
        arguments[1] = framebuffer_address;
        ok = typed_gl_call(c, rva, "glDeleteFramebuffers", arguments, 2u);
    }
    if (ok && ld32(renderbuffer_address) != 0u) {
        arguments[0] = 1u;
        arguments[1] = renderbuffer_address;
        ok = typed_gl_call(c, rva, "glDeleteRenderbuffers", arguments, 2u);
    }
    st32(framebuffer_address, 0u);
    st32(renderbuffer_address, 0u);
    return ok;
}

static int initialize_pc(CPU *__restrict c, uint32_t rva,
                         const char *disabled_fault)
{
    uint32_t manager = c->ecx;
    uint32_t width, height;
    if (!require_pc_mode(c, rva, disabled_fault) ||
        !manager_dimensions(c, rva, &width, &height))
        return 0;
    if (!kage_pc_backend_initialize(width, height)) {
        guest_fault(c, rva, kage_pc_backend_last_error());
        return 0;
    }
    if (!gl_pc_backend_install()) {
        const char *error = gl_pc_backend_last_error();
        gl_pc_backend_uninstall();
        kage_pc_backend_shutdown();
        guest_fault(c, rva, error);
        return 0;
    }
    if (!gl_pc_backend_complete()) {
        const char *error = gl_pc_backend_last_error();
        /* A real guest_fault longjmps.  Tear down the partial callback table
         * and its owning WGL context before crossing that boundary. */
        gl_pc_backend_uninstall();
        kage_pc_backend_shutdown();
        guest_fault(c, rva, error);
        return 0;
    }
    /* There is intentionally no fake GLFW object at +d8.  The backend state is
     * host-private; later code must cross a typed KAGE/GL boundary instead of
     * dereferencing a fabricated guest structure. */
    st32(manager + KAGE_BACKEND_OFFSET, 0u);
    if (!initialize_render_targets(c, rva, manager)) {
        st32(manager + KAGE_FRAMEBUFFER_OFFSET, 0u);
        st32(manager + KAGE_RENDERBUFFER_OFFSET, 0u);
        st32(manager + KAGE_BACKEND_OFFSET, 0u);
        gl_pc_backend_uninstall();
        kage_pc_backend_shutdown();
        return 0;
    }
    c->eax = (c->eax & 0xffffff00u) | 1u;
    (void)gpop(c);                    /* exact MSVC thiscall RET */
    return 1;
}

void sub_00560c60(CPU *__restrict c)
{
    guest_coverage_function(GUEST_COVERAGE_MANUAL_KAGE_INITIALIZE_ID);
    if (use_original(c, guest_original_00560c60))
        return;
    (void)initialize_pc(c, 0x00560c60u,
                        "KAGE graphics backend unavailable: Initialize");
}

void sub_00561830(CPU *__restrict c)
{
    guest_coverage_function(
        GUEST_COVERAGE_MANUAL_KAGE_INITIALIZE_RENDER_DISPLAY_ID);
    if (use_original(c, guest_original_00561830))
        return;
    (void)initialize_pc(
        c, 0x00561830u,
        "KAGE graphics backend unavailable: InitializeRenderDisplay");
}

void sub_00481280(CPU *__restrict c)
{
    uint32_t options;
    uint8_t requested;
    uint8_t actual;
    uint32_t refresh_hz;

    guest_coverage_function(GUEST_COVERAGE_MANUAL_KAGE_SET_VSYNC_ID);
    options = c->ecx;
    requested = ld8(guest_stack_address(
        c, c->esp + 4u, 1U, 0x00481280U));
    if (use_original(c, guest_original_00481280))
        return;
    if (!require_pc_mode(c, GUEST_KAGE_VSYNC_RVA,
                         GUEST_KAGE_VSYNC_FAULT))
        return;
    if (!options) {
        guest_fault(c, GUEST_KAGE_VSYNC_RVA,
                    "KAGE PC backend: null OptionsConfig in SetVSync");
        return;
    }
    st8(options + OPTIONS_VSYNC_OFFSET, requested);
    actual = requested;
    if (requested) {
        refresh_hz = kage_pc_backend_refresh_rate();
        if (!refresh_hz) {
            guest_fault(c, GUEST_KAGE_VSYNC_RVA,
                        kage_pc_backend_last_error());
            return;
        }
        if (!guest_kage_vsync_policy_allows(refresh_hz))
            actual = 0u;
    }
    if (actual != ld8(KAGE_VSYNC_STATE)) {
        /* The guest publishes its policy byte before calling the void GLFW
         * swap-control helper.  Preserve that ordering on a loud WGL fault. */
        st8(KAGE_VSYNC_STATE, actual);
        if (!kage_pc_backend_set_vsync(actual != 0u)) {
            guest_fault(c, GUEST_KAGE_VSYNC_RVA,
                        kage_pc_backend_last_error());
            return;
        }
    }
    (void)gpop(c);                  /* exact MSVC thiscall RET 4 */
    (void)guest_stack_adjust(c, 4U, 0x00481280U);
}

int guest_kage_vsync_policy_allows(uint32_t refresh_hz)
{
    uint32_t remainder;
    uint32_t distance;
    if (refresh_hz <= 1u)
        return 0;
    remainder = refresh_hz % 60u;
    distance = remainder <= 30u ? remainder : 60u - remainder;
    return distance <= 2u;
}

void sub_0056dd70(CPU *__restrict c)
{
    guest_coverage_function(GUEST_COVERAGE_MANUAL_KAGE_SOUND_INITIALIZE_ID);
    if (use_original(c, guest_original_0056dd70))
        return;
    if (!require_pc_mode(c, GUEST_KAGE_SOUND_INITIALIZE_RVA,
                         GUEST_KAGE_SOUND_INITIALIZE_FAULT))
        return;
    if (c->ecx != KAGE_SOUND_MANAGER_ADDRESS) {
        guest_fault(c, GUEST_KAGE_SOUND_INITIALIZE_RVA,
                    "KAGE PC audio boundary: unexpected manager address");
        return;
    }

    /* This is the exact externally visible state of the built-in
     * alcOpenDevice(NULL) failure path, made explicit at the engine seam. */
    st8(c->ecx + KAGE_SOUND_RUNNING, 0u);
    st8(c->ecx + KAGE_SOUND_SUSPENDED, 0u);
    st32(c->ecx + KAGE_SOUND_CONTEXT, 0u);
    st32(c->ecx + KAGE_SOUND_DEVICE, 0u);
    c->eax &= 0xffffff00u;          /* bool false in AL */
    (void)gpop(c);                  /* exact MSVC thiscall RET */
}

void sub_005700e0(CPU *__restrict c)
{
    guest_coverage_function(
        GUEST_COVERAGE_MANUAL_KAGE_GL_PROVIDER_RESOLVER_ID);
    /* Desktop GL keeps libepoxy's exact capability/provider selection. */
    guest_original_005700e0(c);
}

void sub_00560e30(CPU *__restrict c)
{
    int resources_ok;

    guest_coverage_function(GUEST_COVERAGE_MANUAL_KAGE_SHUTDOWN_ID);
    resources_ok = 1;
    if (use_original(c, guest_original_00560e30))
        return;
    if (!require_pc_mode(c, 0x00560e30u,
                         "KAGE graphics backend unavailable: Shutdown"))
        return;
    if (c->ecx == KAGE_MANAGER_ADDRESS && gl_pc_backend_installed())
        resources_ok = destroy_render_targets(c, 0x00560e30u, c->ecx);
    if (c->ecx == KAGE_MANAGER_ADDRESS) {
        st32(c->ecx + KAGE_FRAMEBUFFER_OFFSET, 0u);
        st32(c->ecx + KAGE_RENDERBUFFER_OFFSET, 0u);
        st32(c->ecx + KAGE_BACKEND_OFFSET, 0u);
    }
    gl_pc_backend_uninstall();
    kage_pc_backend_shutdown();
    if (!resources_ok)
        return;
    (void)gpop(c);
}

void sub_00560eb0(CPU *__restrict c)
{
    guest_coverage_function(GUEST_COVERAGE_MANUAL_KAGE_PRESENT_ID);
    if (use_original(c, guest_original_00560eb0))
        return;
    if (!require_pc_mode(c, 0x00560eb0u,
                         "KAGE graphics backend unavailable: Present"))
        return;
    if (!kage_pc_backend_ready()) {
        guest_fault(c, 0x00560eb0u,
                    "KAGE PC backend: Present requested before initialize");
        return;
    }
    /* Preserve the original flush/render-target logic.  Its generated PC
     * seam replaces only the final GLFW/SwapBuffers platform leaf. */
    guest_original_00560eb0(c);
}

void sub_00560f20(CPU *__restrict c)
{
    uint32_t width, height;

    guest_coverage_function(
        GUEST_COVERAGE_MANUAL_KAGE_GET_FRAMEBUFFER_WIDTH_ID);
    if (use_original(c, guest_original_00560f20))
        return;
    if (!require_pc_mode(
            c, 0x00560f20u,
            "KAGE graphics backend unavailable: GetFramebufferWidth"))
        return;
    if (!kage_pc_backend_ready()) {
        guest_fault(c, 0x00560f20u,
                    "KAGE PC backend: width requested before initialize");
        return;
    }
    width = kage_pc_backend_width();
    height = kage_pc_backend_height();
    /* guest_original_00560f20: [0x308071ac] first, then [0x308071a0]. */
    st32(KAGE_HEIGHT_COMPANION, height);
    st32(KAGE_WIDTH_CACHE, width);
    c->eax = width;
    (void)gpop(c);
}

void sub_00560f90(CPU *__restrict c)
{
    uint32_t width, height;

    guest_coverage_function(
        GUEST_COVERAGE_MANUAL_KAGE_GET_FRAMEBUFFER_HEIGHT_ID);
    if (use_original(c, guest_original_00560f90))
        return;
    if (!require_pc_mode(
            c, 0x00560f90u,
            "KAGE graphics backend unavailable: GetFramebufferHeight"))
        return;
    if (!kage_pc_backend_ready()) {
        guest_fault(c, 0x00560f90u,
                    "KAGE PC backend: height requested before initialize");
        return;
    }
    width = kage_pc_backend_width();
    height = kage_pc_backend_height();
    /* guest_original_00560f90: [0x308071a8] first, then [0x308071a4]. */
    st32(KAGE_WIDTH_COMPANION, width);
    st32(KAGE_HEIGHT_CACHE, height);
    c->eax = height;
    (void)gpop(c);
}
