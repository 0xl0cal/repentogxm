/* High-level KAGE implementation for Vita/vitaGL.
 *
 * The wrappers retain the measured x86 thiscall/return contracts while
 * replacing GLFW/WGL at one engine boundary.  No guest GLFW object is forged:
 * manager +0xd8 stays zero, and generated caller-side handoff code skips the
 * private GLFW dereference only after this backend is genuinely ready. */
#include "manual_kage.h"

#include <stdint.h>

#ifndef ISAAC_VITA_AUDIO_MANUAL_ORACLE
#include <vitaGL.h>
#endif

#include "gl_bridge.h"
#include "gl_vita_backend.h"
#include "guest_coverage_generated.h"
#include "kage_vita_backend.h"
#include "manual_kage_vita_vsync.h"
#include "platform.h"

#ifndef ISAAC_VITA_AUDIO
#define ISAAC_VITA_AUDIO 0
#endif

#if ISAAC_VITA_AUDIO
#include "host_vita_audio.h"
#include "host_vita_heap.h"
#include "host_vita_sync.h"
#endif

#define KAGE_MANAGER_ADDRESS \
    (GUEST_IMAGE_BASE + 0x007c7a30u)
#define KAGE_FRAMEBUFFER_OFFSET  0x000000c8u
#define KAGE_RENDERBUFFER_OFFSET 0x000000ccu
#define KAGE_BACKEND_OFFSET      0x000000d8u
#define KAGE_WIDTH_OFFSET        0x000000e0u
#define KAGE_HEIGHT_OFFSET       0x000000e4u
#define KAGE_DEFAULT_WIDTH       960u
#define KAGE_DEFAULT_HEIGHT      540u

#define KAGE_GL_FRAMEBUFFER      0x00008d40u
#define KAGE_GL_RENDERBUFFER     0x00008d41u
#define KAGE_GL_DEPTH_ATTACHMENT 0x00008d00u
#define KAGE_GL_DEPTH_COMPONENT24 0x000081a6u

#define OPTIONS_VSYNC_OFFSET 0x00000055u
#define KAGE_VSYNC_STATE \
    (GUEST_IMAGE_BASE + 0x007c7b0du)

#define KAGE_SOUND_MANAGER_ADDRESS \
    (GUEST_IMAGE_BASE + 0x007e7d40u)
#define KAGE_SOUND_RUNNING   0x00000004u
#define KAGE_SOUND_SUSPENDED 0x00000005u
#define KAGE_SOUND_POOL      0x00000008u
#define KAGE_SOUND_MUTEX_A   0x00000018u
#define KAGE_SOUND_MUTEX_B   0x00000024u
#define KAGE_SOUND_CONTEXT   0x00000030u
#define KAGE_SOUND_DEVICE    0x00000034u
#define KAGE_SOUND_THREAD_STATE 0x00000340u
#define KAGE_SOUND_MUTEX_STORAGE_SIZE 0x0000001cu
#define KAGE_SOUND_POOL_ALLOCATION_SIZE 0x00000184u
#define KAGE_SOUND_POOL_COUNT 32u
#define KAGE_SOUND_FRAME_CALLBACK_GLOBAL \
    (GUEST_IMAGE_BASE + 0x00804114u)
#define KAGE_SOUND_FRAME_CALLBACK \
    (GUEST_IMAGE_BASE + 0x0056f8d0u)

#define KAGE_WIDTH_CACHE \
    (GUEST_IMAGE_BASE + 0x008071a0u)
#define KAGE_HEIGHT_CACHE \
    (GUEST_IMAGE_BASE + 0x008071a4u)
#define KAGE_WIDTH_COMPANION \
    (GUEST_IMAGE_BASE + 0x008071a8u)
#define KAGE_HEIGHT_COMPANION \
    (GUEST_IMAGE_BASE + 0x008071acu)

#ifndef ISAAC_VITA_AUDIO_MANUAL_ORACLE
static int use_original(CPU *__restrict c, guest_fn original)
{
    if (!g_guest_gl_inventory_mode)
        return 0;
    original(c);
    return 1;
}

int guest_kage_pc_handoff_ready(void)
{
    return kage_vita_backend_ready();
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

static int manager_dimensions(CPU *__restrict c, uint32_t rva,
                              uint32_t *width, uint32_t *height)
{
    if (c->ecx != KAGE_MANAGER_ADDRESS) {
        guest_fault(c, rva, "KAGE Vita backend: unexpected manager address");
        return 0;
    }
    *width = ld32(c->ecx + KAGE_WIDTH_OFFSET);
    *height = ld32(c->ecx + KAGE_HEIGHT_OFFSET);
    if (!*width) {
        *width = KAGE_DEFAULT_WIDTH;
        st32(c->ecx + KAGE_WIDTH_OFFSET, *width);
    }
    if (!*height) {
        *height = KAGE_DEFAULT_HEIGHT;
        st32(c->ecx + KAGE_HEIGHT_OFFSET, *height);
    }
    return 1;
}

static uint32_t render_target_extent(uint32_t requested)
{
    uint32_t extent = 8u;
    while (extent < requested)
        extent <<= 1u;
    return extent;
}

static void destroy_render_targets(uint32_t manager)
{
    GLuint framebuffer = (GLuint)ld32(manager + KAGE_FRAMEBUFFER_OFFSET);
    GLuint renderbuffer = (GLuint)ld32(manager + KAGE_RENDERBUFFER_OFFSET);

    glBindFramebuffer((GLenum)KAGE_GL_FRAMEBUFFER, 0u);
#if defined(ISAAC_VITA_FIRST_FRAME_PROBE)
    gl_vita_backend_first_frame_set_manager_framebuffer(0u);
#endif
    if (framebuffer)
        glDeleteFramebuffers(1, &framebuffer);
    if (renderbuffer) {
        glDeleteRenderbuffers(1, &renderbuffer);
        gl_vita_backend_unregister_renderbuffer((uint32_t)renderbuffer);
    }
    st32(manager + KAGE_FRAMEBUFFER_OFFSET, 0u);
    st32(manager + KAGE_RENDERBUFFER_OFFSET, 0u);
}

static int initialize_render_targets(CPU *__restrict c, uint32_t rva,
                                     uint32_t manager,
                                     uint32_t width, uint32_t height)
{
    GLuint framebuffer = 0u;
    GLuint renderbuffer = 0u;
    uint32_t render_width = render_target_extent(width);
    uint32_t render_height = render_target_extent(height);

    destroy_render_targets(manager);
    glGenFramebuffers(1, &framebuffer);
    if (!framebuffer) {
        guest_fault(c, rva,
                    "KAGE Vita backend: glGenFramebuffers returned zero");
        return 0;
    }
    st32(manager + KAGE_FRAMEBUFFER_OFFSET, (uint32_t)framebuffer);
    glBindFramebuffer((GLenum)KAGE_GL_FRAMEBUFFER, framebuffer);

    glGenRenderbuffers(1, &renderbuffer);
    if (!renderbuffer) {
        destroy_render_targets(manager);
        guest_fault(c, rva,
                    "KAGE Vita backend: glGenRenderbuffers returned zero");
        return 0;
    }
    st32(manager + KAGE_RENDERBUFFER_OFFSET, (uint32_t)renderbuffer);
    glBindRenderbuffer((GLenum)KAGE_GL_RENDERBUFFER, renderbuffer);
    glRenderbufferStorage(
        (GLenum)KAGE_GL_RENDERBUFFER,
        (GLenum)KAGE_GL_DEPTH_COMPONENT24,
        (GLsizei)render_width,
        (GLsizei)render_height);
    if (!gl_vita_backend_register_renderbuffer(
            (uint32_t)renderbuffer, KAGE_GL_DEPTH_COMPONENT24,
            (int32_t)render_width, (int32_t)render_height)) {
        destroy_render_targets(manager);
        guest_fault(c, rva, gl_vita_backend_last_error());
        return 0;
    }
    glFramebufferRenderbuffer(
        (GLenum)KAGE_GL_FRAMEBUFFER,
        (GLenum)KAGE_GL_DEPTH_ATTACHMENT,
        (GLenum)KAGE_GL_RENDERBUFFER,
        renderbuffer);
    glBindFramebuffer((GLenum)KAGE_GL_FRAMEBUFFER, 0u);
#if defined(ISAAC_VITA_FIRST_FRAME_PROBE)
    gl_vita_backend_first_frame_set_manager_framebuffer(
        (uint32_t)framebuffer);
#endif
    return 1;
}

static int initialize_vita(CPU *__restrict c, uint32_t rva,
                           const char *fault_message)
{
    uint32_t manager = c->ecx;
    uint32_t width;
    uint32_t height;

    if (!manager_dimensions(c, rva, &width, &height))
        return 0;
    if (!kage_vita_backend_initialize(width, height)) {
        guest_fault(c, rva, kage_vita_backend_last_error());
        return 0;
    }
    if (!gl_vita_backend_install()) {
        kage_vita_backend_deactivate();
        guest_fault(c, rva, fault_message);
        return 0;
    }

    /* Native backend state stays host-private. */
    st32(manager + KAGE_BACKEND_OFFSET, 0u);
    if (!initialize_render_targets(c, rva, manager, width, height)) {
        gl_vita_backend_uninstall();
        kage_vita_backend_deactivate();
        return 0;
    }

    isaac_vita_log(
        "KAGE VITA READY: manager=0x%08x fbo=%u rbo=%u backend=0x%08x "
        "logical=%ux%u callbacks=%u/%u",
        (unsigned)manager,
        (unsigned)ld32(manager + KAGE_FRAMEBUFFER_OFFSET),
        (unsigned)ld32(manager + KAGE_RENDERBUFFER_OFFSET),
        (unsigned)ld32(manager + KAGE_BACKEND_OFFSET),
        (unsigned)width, (unsigned)height,
        (unsigned)gl_vita_backend_resolved_count(),
        (unsigned)(gl_vita_backend_resolved_count() +
                   gl_vita_backend_missing_count()));

    c->eax = (c->eax & 0xffffff00u) | 1u;
    (void)gpop(c);                    /* exact MSVC thiscall RET */
    return 1;
}

void sub_00560c60(CPU *__restrict c)
{
    guest_coverage_function(GUEST_COVERAGE_MANUAL_KAGE_INITIALIZE_ID);
    if (use_original(c, guest_original_00560c60))
        return;
    (void)initialize_vita(c, GUEST_KAGE_INITIALIZE_RVA,
                          GUEST_KAGE_INITIALIZE_FAULT);
}

void sub_00561830(CPU *__restrict c)
{
    guest_coverage_function(
        GUEST_COVERAGE_MANUAL_KAGE_INITIALIZE_RENDER_DISPLAY_ID);
    if (use_original(c, guest_original_00561830))
        return;
    (void)initialize_vita(c, GUEST_KAGE_RENDER_DISPLAY_RVA,
                          GUEST_KAGE_RENDER_DISPLAY_FAULT);
}

void sub_00481280(CPU *__restrict c)
{
    uint32_t options = c->ecx;
    uint8_t requested = ld8(guest_stack_address(
        c, c->esp + 4u, 1U, 0x00481280U));
    uint8_t stored_requested;
    uint8_t prior_actual;
    uint8_t actual;
    int prior_backend_actual;
    int backend_changed;
    int reconciled;

    guest_coverage_function(GUEST_COVERAGE_MANUAL_KAGE_SET_VSYNC_ID);
    if (use_original(c, guest_original_00481280))
        return;
    if (!options || !kage_vita_backend_ready()) {
        guest_fault(c, GUEST_KAGE_VSYNC_RVA,
                    "KAGE Vita backend: invalid SetVSync state");
        return;
    }
    prior_actual = ld8(KAGE_VSYNC_STATE);
    actual = prior_actual;
    prior_backend_actual = kage_vita_backend_vsync_enabled();
    reconciled = guest_kage_vita_vsync_reconcile(
        requested, &stored_requested, &actual, prior_backend_actual,
        kage_vita_backend_set_vsync, &backend_changed);

    /* The setting is the user's requested value, not the platform's actual
     * swap policy.  Preserve it even if the backend reports a loud failure. */
    st8(options + OPTIONS_VSYNC_OFFSET, stored_requested);
    if (!reconciled) {
        isaac_vita_log(
            "KAGE VITA VSYNC: requested=%u actual=%u backend_actual=%u "
            "policy=software-60hz result=FAILED",
            (unsigned)stored_requested, (unsigned)prior_actual,
            (unsigned)(prior_backend_actual != 0));
        guest_fault(c, GUEST_KAGE_VSYNC_RVA,
                    kage_vita_backend_last_error());
        return;
    }
    if (actual != prior_actual)
        st8(KAGE_VSYNC_STATE, actual);
    isaac_vita_log(
        "KAGE VITA VSYNC: requested=%u actual=%u backend_actual=%u "
        "policy=software-60hz backend_changed=%u",
        (unsigned)stored_requested, (unsigned)actual,
        (unsigned)(kage_vita_backend_vsync_enabled() != 0),
        (unsigned)(backend_changed != 0));
    (void)gpop(c);                    /* exact MSVC thiscall RET 4 */
    (void)guest_stack_adjust(c, 4U, 0x00481280U);
}
#endif

static void sound_initialize_return(CPU *__restrict c, int success)
{
    c->eax = (c->eax & 0xffffff00u) | (success ? 1u : 0u);
    (void)gpop(c);                    /* exact MSVC thiscall RET */
}

#if ISAAC_VITA_AUDIO
static int sound_native_guest_pointer(void *pointer, uint32_t *guest_pointer)
{
    uintptr_t value = (uintptr_t)pointer;

    if (value > UINT32_MAX)
        return 0;
    *guest_pointer = (uint32_t)value;
    return 1;
}

static int sound_mutex_initialize(CPU *__restrict c, uint32_t object)
{
    void *storage;
    uint32_t guest_storage;

    if (ld8(object + 4u) || ld32(object + 8u))
        return 0;
    storage = isaac_vita_guest_malloc(KAGE_SOUND_MUTEX_STORAGE_SIZE);
    if (!storage || isaac_vita_guest_heap_terminal()) {
        if (storage)
            (void)isaac_vita_guest_free(storage);
        return 0;
    }
    if (!sound_native_guest_pointer(storage, &guest_storage)) {
        (void)isaac_vita_guest_free(storage);
        return 0;
    }

    /* sub_00562d90 allocates 0x1c, clears its trailing state byte, then calls
     * InitializeCriticalSection.  The host sync helper writes the same opaque
     * 24-byte recursive-mutex payload consumed by Enter/Leave/Delete. */
    st8(guest_storage + 0x18u, 0u);
    if (!isaac_vita_sync_cs_initialize_plain(c, guest_storage)) {
        (void)isaac_vita_guest_free(storage);
        return 0;
    }
    st32(object + 8u, guest_storage);
    st8(object + 4u, 1u);
    return 1;
}

static void sound_mutex_release(CPU *__restrict c, uint32_t object)
{
    uint32_t storage = ld32(object + 8u);

    if (ld8(object + 4u) && storage) {
        if (!isaac_vita_sync_cs_delete(c, storage))
            return;
        (void)isaac_vita_guest_free((void *)(uintptr_t)storage);
    }
    st32(object + 8u, 0u);
    st8(object + 4u, 0u);
}
#endif

void sub_0056dd70(CPU *__restrict c)
{
    guest_coverage_function(GUEST_COVERAGE_MANUAL_KAGE_SOUND_INITIALIZE_ID);
#if !ISAAC_VITA_AUDIO
    if (use_original(c, guest_original_0056dd70))
        return;
#endif
    if (c->ecx != KAGE_SOUND_MANAGER_ADDRESS) {
        guest_fault(c, GUEST_KAGE_SOUND_INITIALIZE_RVA,
                    "KAGE Vita audio boundary: unexpected manager address");
        return;
    }
#if ISAAC_VITA_AUDIO
    {
        uint32_t manager = c->ecx;
        uint32_t al_error = 0u;
        void *pool_block = NULL;
        uint32_t guest_pool = 0u;
        const char *stage = "openal";
        int manager_acquired = 0;
        int mutex_a_acquired = 0;
        int mutex_b_acquired = 0;
        int status = isaac_vita_audio_manager_initialize(manager, &al_error);

        if (status == ISAAC_VITA_AUDIO_MANAGER_ALREADY_ACTIVE)
            goto sound_initialize_already_active;
        if (status != ISAAC_VITA_AUDIO_MANAGER_OK)
            goto sound_initialize_failed;
        manager_acquired = 1;

        stage = "mutex-a";
        if (!sound_mutex_initialize(c, manager + KAGE_SOUND_MUTEX_A))
            goto sound_initialize_failed;
        mutex_a_acquired = 1;
        stage = "mutex-b";
        if (!sound_mutex_initialize(c, manager + KAGE_SOUND_MUTEX_B))
            goto sound_initialize_failed;
        mutex_b_acquired = 1;

        stage = "pool";
        pool_block = isaac_vita_guest_calloc(
            1u, KAGE_SOUND_POOL_ALLOCATION_SIZE);
        if (!pool_block || isaac_vita_guest_heap_terminal() ||
                !sound_native_guest_pointer(pool_block, &guest_pool))
            goto sound_initialize_failed;
        st32(guest_pool, KAGE_SOUND_POOL_COUNT);
        st32(manager + KAGE_SOUND_POOL, guest_pool + 4u);

        /* The original starts sub_0056e730 through _beginthreadex here.  The
         * translated CPU/dispatcher is single-thread-owned; sub_00598d60
         * calls the same sub_0056f040 update once per main-loop iteration,
         * and the pinned room-instantiation safe point covers long Update
         * work cooperatively.  OpenAL Soft keeps its native mixer thread, but
         * no thread re-enters guest code. */
        stage = "guest-thread-state";
        if (ld32(manager + KAGE_SOUND_THREAD_STATE) != 0u)
            goto sound_initialize_failed;
        st32(KAGE_SOUND_FRAME_CALLBACK_GLOBAL,
             KAGE_SOUND_FRAME_CALLBACK);
        st16(manager + KAGE_SOUND_RUNNING, 1u);
        isaac_vita_log(
            "KAGE VITA AUDIO INIT: status=ready stage=complete "
            "device=0x%08x context=0x%08x al=0x%08x "
            "manager_sources=%u device_sources=%u stream_headroom=%u "
            "buffers=%u pump=main-loop+room-cooperative guest_thread=off",
            (unsigned)ld32(manager + KAGE_SOUND_DEVICE),
            (unsigned)ld32(manager + KAGE_SOUND_CONTEXT),
            (unsigned)al_error,
            (unsigned)ISAAC_VITA_AUDIO_MANAGER_SOURCE_COUNT,
            (unsigned)ISAAC_VITA_AUDIO_DEVICE_SOURCE_COUNT,
            (unsigned)ISAAC_VITA_AUDIO_STREAM_SOURCE_HEADROOM,
            (unsigned)ISAAC_VITA_AUDIO_MANAGER_BUFFER_COUNT);
        sound_initialize_return(c, 1);
        return;

sound_initialize_failed:
        if (guest_pool) {
            st32(manager + KAGE_SOUND_POOL, 0u);
            (void)isaac_vita_guest_free(pool_block);
        } else if (pool_block) {
            (void)isaac_vita_guest_free(pool_block);
        }
        if (mutex_b_acquired)
            sound_mutex_release(c, manager + KAGE_SOUND_MUTEX_B);
        if (mutex_a_acquired)
            sound_mutex_release(c, manager + KAGE_SOUND_MUTEX_A);
        if (manager_acquired)
            isaac_vita_audio_manager_close(manager);
        st8(manager + KAGE_SOUND_RUNNING, 0u);
        st8(manager + KAGE_SOUND_SUSPENDED, 0u);
        if (isaac_vita_guest_heap_terminal()) {
            guest_fault(c, manager, "KAGE sound guest heap is terminal");
            return;
        }
        isaac_vita_log(
            "KAGE VITA AUDIO INIT: status=%s stage=%s "
            "device=0x%08x context=0x%08x al=0x%08x "
            "sources=0 buffers=0 pump=off guest_thread=off",
            status == ISAAC_VITA_AUDIO_MANAGER_OK ?
                "guest-state-failed" :
                isaac_vita_audio_manager_status_name(status),
            stage,
            (unsigned)ld32(manager + KAGE_SOUND_DEVICE),
            (unsigned)ld32(manager + KAGE_SOUND_CONTEXT),
            (unsigned)al_error);
        sound_initialize_return(c, 0);
        return;

sound_initialize_already_active:
        /* Duplicate SoundInitialize is rejected without treating resources
         * owned by the first call as failure-path acquisitions.  In
         * particular, do not release its mutexes/pool or close its partial
         * OpenAL owner. */
        isaac_vita_log(
            "KAGE VITA AUDIO INIT: status=already-active stage=openal "
            "device=0x%08x context=0x%08x al=0x%08x "
            "ownership=unchanged",
            (unsigned)ld32(manager + KAGE_SOUND_DEVICE),
            (unsigned)ld32(manager + KAGE_SOUND_CONTEXT),
            (unsigned)al_error);
        sound_initialize_return(c, 0);
        return;
    }
#else
    /* A/B fallback: preserve the measured alcOpenDevice(NULL) failure state
     * byte-for-byte and keep every OpenAL library out of the link. */
    st8(c->ecx + KAGE_SOUND_RUNNING, 0u);
    st8(c->ecx + KAGE_SOUND_SUSPENDED, 0u);
    st32(c->ecx + KAGE_SOUND_CONTEXT, 0u);
    st32(c->ecx + KAGE_SOUND_DEVICE, 0u);
    isaac_vita_log(
        "KAGE VITA AUDIO INIT: status=disabled stage=compile-option "
        "device=0x00000000 context=0x00000000 al=unavailable "
        "sources=0 buffers=0 pump=off guest_thread=off");
    sound_initialize_return(c, 0);
#endif
}

#ifndef ISAAC_VITA_AUDIO_MANUAL_ORACLE
void sub_005700e0(CPU *__restrict c)
{
    char procedure[GUEST_GL_PROCEDURE_NAME_MAX + 1U];

    guest_coverage_function(
        GUEST_COVERAGE_MANUAL_KAGE_GL_PROVIDER_RESOLVER_ID);
    /* Inventory must execute the exact provider algorithm, and a pre-KAGE
     * call has no native context which could honour the resulting token. */
    if (use_original(c, guest_original_005700e0) ||
            !kage_vita_backend_ready() || !gl_vita_backend_installed()) {
        if (!g_guest_gl_inventory_mode)
            guest_original_005700e0(c);
        return;
    }

    if (!guest_gl_resolve_provider(c, procedure)) {
        /* Intentional policy: once the Vita backend owns KAGE, every one of
         * libepoxy's other ~3,009 registry names is unsupported at this typed
         * boundary.  Fault here with the canonical spelling instead of
         * returning token zero and allowing a later call-through-zero abort. */
        isaac_vita_log(
            "KAGE VITA GL RESOLVE FAIL: name=%s address=0x%08x",
            procedure[0] ? procedure : "<invalid>", (unsigned)c->ecx);
        guest_fault(c, GUEST_KAGE_GL_PROVIDER_RESOLVER_RVA,
                    GUEST_KAGE_GL_PROVIDER_RESOLVER_FAULT);
    }
}

void sub_00560e30(CPU *__restrict c)
{
    guest_coverage_function(GUEST_COVERAGE_MANUAL_KAGE_SHUTDOWN_ID);
    if (use_original(c, guest_original_00560e30))
        return;
    if (c->ecx != KAGE_MANAGER_ADDRESS || !kage_vita_backend_ready()) {
        guest_fault(c, 0x00560e30u,
                    "KAGE Vita backend: invalid Shutdown state");
        return;
    }
    destroy_render_targets(c->ecx);
    st32(c->ecx + KAGE_BACKEND_OFFSET, 0u);
    gl_vita_backend_uninstall();
    kage_vita_backend_deactivate();
    (void)gpop(c);
}

void sub_00560eb0(CPU *__restrict c)
{
    guest_coverage_function(GUEST_COVERAGE_MANUAL_KAGE_PRESENT_ID);
    if (use_original(c, guest_original_00560eb0))
        return;
    if (!kage_vita_backend_ready()) {
        guest_fault(c, 0x00560eb0u,
                    "KAGE Vita backend: Present requested before initialize");
        return;
    }
    /* The preserved body owns flush/render-target logic.  Its generated final
     * platform leaf calls kage_pc_backend_present(), which the Vita hook maps
     * to vglSwapBuffers without a GLFW window. */
    guest_original_00560eb0(c);
}

void sub_00560f20(CPU *__restrict c)
{
    uint32_t width;
    uint32_t height;

    guest_coverage_function(
        GUEST_COVERAGE_MANUAL_KAGE_GET_FRAMEBUFFER_WIDTH_ID);
    if (use_original(c, guest_original_00560f20))
        return;
    if (!kage_vita_backend_ready()) {
        guest_fault(c, 0x00560f20u,
                    "KAGE Vita backend: width requested before initialize");
        return;
    }
    width = kage_vita_backend_width();
    height = kage_vita_backend_height();
    st32(KAGE_HEIGHT_COMPANION, height);
    st32(KAGE_WIDTH_CACHE, width);
    c->eax = width;
    (void)gpop(c);
}

void sub_00560f90(CPU *__restrict c)
{
    uint32_t width;
    uint32_t height;

    guest_coverage_function(
        GUEST_COVERAGE_MANUAL_KAGE_GET_FRAMEBUFFER_HEIGHT_ID);
    if (use_original(c, guest_original_00560f90))
        return;
    if (!kage_vita_backend_ready()) {
        guest_fault(c, 0x00560f90u,
                    "KAGE Vita backend: height requested before initialize");
        return;
    }
    width = kage_vita_backend_width();
    height = kage_vita_backend_height();
    st32(KAGE_WIDTH_COMPANION, width);
    st32(KAGE_HEIGHT_CACHE, height);
    c->eax = height;
    (void)gpop(c);
}
#endif
