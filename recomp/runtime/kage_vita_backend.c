/* Single process-wide vitaGL context for the recompiled KAGE boundary.
 *
 * vitaGL has no teardown API and suppresses a second initialization itself.
 * Keep the context alive after KAGE Shutdown, but mark the current KAGE
 * session inactive.  A later Initialize reuses the real context instead of
 * trying to manufacture or recreate a GLFW object. */
#include "kage_vita_backend.h"
#include "kage_vita_deep_profile.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(ISAAC_KAGE_VITA_BACKEND_ORACLE)
#include "kage_vita_backend_test_vitagl.h"
#if defined(ISAAC_VITA_IO_PROFILE)
#define KAGE_VITA_MEMORY_LOG sceClibPrintf
#endif
#else
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>

#if defined(ISAAC_VITA_IO_PROFILE)
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
void isaac_vita_log(const char *format, ...);
#define KAGE_VITA_MEMORY_LOG isaac_vita_log
#endif
#endif
/* Per-window records on the game thread (present heartbeat, vsync policy)
 * go to the logger thread under ISAAC_VITA_LOG_ASYNC; boot-time banners
 * (vitaGL ready, splash, first present) stay synchronous. */
#if defined(ISAAC_VITA_LOG_ASYNC)
# include "host_vita_log_async.h"
#else
# define ISAAC_VITA_LOG_PRINTF sceClibPrintf
#endif

#include "host_vita_import_id.h"
#include "gl_vita_backend.h"
#include "kage_vita_input.h"
#include "kage_vita_loading.h"
#include "kage_vita_fullspeed_scheduler.h"
#include "kage_vita_stable30.h"
#include "kage_vita_phase_profile.h"
#include "kage_vita_stall_probe.h"
#if defined(ISAAC_VITA_SIM_CADENCE_RECEIPT)
# include "kage_vita_sim_cadence_receipt.h"
#endif
#if defined(ISAAC_VITA_RAW_GXM_PROBE)
# include "kage_vita_raw_gxm_probe.h"
#endif
#if defined(ISAAC_VITA_SCREENSHOT_PROBE)
# include "kage_vita_screenshot_probe.h"
#endif
#if defined(ISAAC_VITA_FIRST_FRAME_PROBE)
void kage_vita_first_frame_note_present_return(uint32_t present_count);
#endif

#define KAGE_VITA_LOGICAL_WIDTH   960u
#define KAGE_VITA_LOGICAL_HEIGHT  540u
#if defined(ISAAC_VITA_DISPLAY_RASTER_720)
/* Scaled display raster.  The build passes WIDTH/HEIGHT/NUM/DEN (720x408 at
 * 3/4 or 480x272 at 1/2); the bare legacy gate (host oracles) means 720x408.
 * The viewport is the logical 960x540 scaled by NUM/DEN and centred
 * vertically inside the raster: one spare row at 720 and 480, two at 960. */
# if !defined(ISAAC_VITA_DISPLAY_RASTER_WIDTH)
#  define ISAAC_VITA_DISPLAY_RASTER_WIDTH  720
#  define ISAAC_VITA_DISPLAY_RASTER_HEIGHT 408
#  define ISAAC_VITA_DISPLAY_RASTER_NUM    3
#  define ISAAC_VITA_DISPLAY_RASTER_DEN    4
# endif
#define KAGE_VITA_DISPLAY_WIDTH   ISAAC_VITA_DISPLAY_RASTER_WIDTH
#define KAGE_VITA_DISPLAY_HEIGHT  ISAAC_VITA_DISPLAY_RASTER_HEIGHT
#define KAGE_VITA_VIEWPORT_WIDTH \
    (960 * ISAAC_VITA_DISPLAY_RASTER_NUM / ISAAC_VITA_DISPLAY_RASTER_DEN)
#define KAGE_VITA_VIEWPORT_HEIGHT \
    (540 * ISAAC_VITA_DISPLAY_RASTER_NUM / ISAAC_VITA_DISPLAY_RASTER_DEN)
#define KAGE_VITA_VIEWPORT_Y \
    ((KAGE_VITA_DISPLAY_HEIGHT - KAGE_VITA_VIEWPORT_HEIGHT) / 2)
#else
#define KAGE_VITA_DISPLAY_WIDTH   960
#define KAGE_VITA_DISPLAY_HEIGHT  544
#define KAGE_VITA_VIEWPORT_Y      2
#endif
/* Stringified raster for the fixed-format hardware records below; no printf
 * argument changes between rasters. */
#define KAGE_VITA_STRINGIFY_(value) #value
#define KAGE_VITA_STRINGIFY(value) KAGE_VITA_STRINGIFY_(value)
#define KAGE_VITA_DISPLAY_WIDTH_TEXT \
    KAGE_VITA_STRINGIFY(KAGE_VITA_DISPLAY_WIDTH)
#define KAGE_VITA_DISPLAY_HEIGHT_TEXT \
    KAGE_VITA_STRINGIFY(KAGE_VITA_DISPLAY_HEIGHT)
/* USER_RW left outside vitaGL's RAM pool.  16 MiB is enough for the
 * audio-OFF build; with the 12.4 MiB OpenAL pool reserved ahead of vitaGL the
 * remaining window failed a 128 KiB thread-stack allocation at Continue
 * (2026-09-03, perf:wf-flags-v4-audio: "Cannot allocate physical memory
 * from [ScePhyMemPartGame] REQUEST=0x00020000", coredump).  CMake sets it
 * from ISAAC_VITA_VITAGL_RAM_RESERVE_MB (32 when audio is on). */
#ifndef KAGE_VITA_RAM_THRESHOLD
#define KAGE_VITA_RAM_THRESHOLD   0x01000000
#endif
#define KAGE_VITA_CDRAM_THRESHOLD 0x00800000
#define KAGE_VITA_PHYCONT_THRESHOLD 0
/* vitaGL's pinned private ceiling; passing the ceiling preserves a zero
 * common-dialog pool while leaving 8 MiB of CDRAM for three 2 MiB scanouts. */
#define KAGE_VITA_CDIALOG_THRESHOLD 0x008c6000
#define KAGE_VITA_RENDER_TARGET_SCENES 8u
/* What the stock hardware-reference profile actually requested from vitaGL:
 * eight when ISAAC_VITA_STOCK_DISPLAY_RT_SCENES is set, otherwise vitaGL's
 * default of one.  Logged so the hardware record names the value in effect. */
#if defined(ISAAC_VITA_STOCK_DISPLAY_RT_SCENES)
# define KAGE_VITA_STOCK_DISPLAY_RT_SCENES_VALUE KAGE_VITA_RENDER_TARGET_SCENES
#else
# define KAGE_VITA_STOCK_DISPLAY_RT_SCENES_VALUE 1u
#endif
/* ISAAC_VITA_STOCK_FBO_RT_SCENES=N (2..8): scenes per frame for the GXM render
 * targets stock vitaGL creates lazily for framebuffer objects, through the
 * 0007-isaac-fbo-rt-scenes.patch hook.  vitaGL sizes them for one scene, and
 * the game runs three offscreen passes per frame on one manager FBO, so with
 * one scene slot the sceGxmEndScene of pass k waits until pass k-1 has
 * finished on the GPU (prof-v12 ph120.gt: the glClear c bucket is that
 * EndScene wait, 12.7 ms/frame steady, 72 ms in bursts).  The hook is not
 * declared in vitaGL.h (the 0004 shader cache hashes that header), returns
 * the value in effect (out-of-range requests are ignored by the patch), and
 * the banner prints that returned value so the hardware record names what
 * vitaGL will really use.  A 1024x1024 target at 8 scenes costs ~0.95 MB more
 * GXM driver memory than at 1. */
#if defined(ISAAC_VITA_STOCK_FBO_RT_SCENES)
# if (ISAAC_VITA_STOCK_FBO_RT_SCENES) < 1 || (ISAAC_VITA_STOCK_FBO_RT_SCENES) > 8
#  error "ISAAC_VITA_STOCK_FBO_RT_SCENES must be 1..8 (MAX_SCENES_PER_FRAME)"
# endif
uint8_t vglIsaacSetupFboRenderTargetScenes(uint8_t size);
static uint8_t s_stock_fbo_rt_scenes = 1u;
# define KAGE_VITA_STOCK_FBO_RT_SCENES_BANNER " fbo-rt-scenes=%u"
# define KAGE_VITA_STOCK_FBO_RT_SCENES_ARG (unsigned)s_stock_fbo_rt_scenes,
#else
# define KAGE_VITA_STOCK_FBO_RT_SCENES_BANNER ""
# define KAGE_VITA_STOCK_FBO_RT_SCENES_ARG
#endif
/* ISAAC_VITA_STOCK_FBO_VALID_REGION=1 (observe) | 2 (on): the mode hook of
 * 0009-isaac-fbo-valid-region.patch.  The game's offscreen attachments are
 * pow2 textures larger than what it draws (1024x1024 for the 960x540
 * surface) and stock vitaGL begins every FBO scene with a NULL
 * SceGxmValidRegion, so the padding is cleared, drawn and stored on every
 * offscreen pass.  Observe mode only counts the FBO scenes whose logical rect
 * (the guest's last glViewport) is known and smaller than the attachment and
 * the guest viewport violations apply mode would clip; apply mode passes that
 * rect as the valid region and clamps the tile-clipper region to it (NULL if
 * stale, unanchored or oversized).  The hook is not declared in vitaGL.h (the
 * 0004 shader cache hashes that header), returns the mode in effect and the
 * banner prints it so the hardware record names what vitaGL really does.
 * The counters come back through kage_vita_phase_profile.c (ph120.gx
 * vr(s,p,u,a,e,v,c), ph120.vz). */
#if defined(ISAAC_VITA_STOCK_FBO_VALID_REGION)
# if (ISAAC_VITA_STOCK_FBO_VALID_REGION) < 1 || (ISAAC_VITA_STOCK_FBO_VALID_REGION) > 2
#  error "ISAAC_VITA_STOCK_FBO_VALID_REGION must be 1 (observe) or 2 (on)"
# endif
uint8_t vglIsaacSetupFboValidRegion(uint8_t apply);
static uint8_t s_stock_fbo_valid_region_apply = 0u;
# define KAGE_VITA_STOCK_FBO_VALID_REGION_BANNER " fbo-valid-region=%s"
# define KAGE_VITA_STOCK_FBO_VALID_REGION_ARG \
    (s_stock_fbo_valid_region_apply ? "on" : "observe"),
#else
# define KAGE_VITA_STOCK_FBO_VALID_REGION_BANNER ""
# define KAGE_VITA_STOCK_FBO_VALID_REGION_ARG
#endif

#if defined(ISAAC_VITA_IO_PROFILE)
#define KAGE_VITA_RESERVED_RT_WIDTH  1024u
#define KAGE_VITA_RESERVED_RT_HEIGHT 1024u
#define KAGE_VITA_RESERVED_RT_SCENES KAGE_VITA_RENDER_TARGET_SCENES

#define KAGE_VITA_MEMORY_SNAPSHOT_FIRST_FBO 0x1u
#define KAGE_VITA_MEMORY_SNAPSHOT_POST_INIT 0x2u
#ifndef ISAAC_VITA_IO_PROFILE_BUILD_ID
#define ISAAC_VITA_IO_PROFILE_BUILD_ID "memory:unstamped"
#endif

/* The pinned overlay names ordinal 2 VGL_MEM_PHYCONT; the older ambient SDK
 * names the same public ABI slot VGL_MEM_SLOW.  Derive it from the asserted
 * neighbours so both static test header surfaces describe the same pool. */
#define KAGE_VITA_VGL_MEM_PHYCONT ((vglMemType)(VGL_MEM_RAM + 1))
_Static_assert(VGL_MEM_VRAM + 1 == VGL_MEM_RAM &&
               VGL_MEM_RAM + 2 == VGL_MEM_BUDGET,
               "vitaGL memory-domain ABI changed");

_Static_assert(KAGE_VITA_MEMORY_SNAPSHOT_MAX_BODY < 384u,
               "Vita memory snapshot exceeds isaac_vita_log body");
#endif

enum kage_vita_context_state {
    KAGE_VITA_CONTEXT_COLD = 0,
    KAGE_VITA_CONTEXT_INITIALIZING = 1,
    KAGE_VITA_CONTEXT_READY = 2,
    KAGE_VITA_CONTEXT_FAILED = 3
};

static volatile int s_context_state;
static volatile int s_active;
static uint32_t s_width;
static uint32_t s_height;
static uint64_t s_time_origin;
static unsigned s_present_count;
static unsigned s_vsync_change_count;
static int s_vsync_enabled;
static const char *s_gl_version;
static char s_error[160];

#if defined(ISAAC_VITA_IO_PROFILE)
static volatile unsigned s_memory_snapshot_mask;

typedef struct kage_vita_memory_snapshot {
    int kernel_result;
    unsigned kernel_user;
    unsigned kernel_cdram;
    unsigned kernel_phycont;
    unsigned vgl_ram_free;
    unsigned vgl_ram_total;
    unsigned vgl_vram_free;
    unsigned vgl_vram_total;
    unsigned vgl_phycont_free;
    unsigned vgl_phycont_total;
    unsigned vgl_budget_free;
    unsigned vgl_budget_total;
    int render_target_query_result;
    unsigned render_target_driver_bytes;
} kage_vita_memory_snapshot;

static int kage_vita_claim_memory_snapshot(unsigned bit)
{
    return (__sync_fetch_and_or(&s_memory_snapshot_mask, bit) & bit) == 0u;
}

static void kage_vita_sample_memory(kage_vita_memory_snapshot *snapshot)
{
    SceKernelFreeMemorySizeInfo kernel_info;
    SceGxmRenderTargetParams render_target_params;

    memset(snapshot, 0, sizeof *snapshot);
    memset(&kernel_info, 0, sizeof kernel_info);
    kernel_info.size = sizeof kernel_info;
    snapshot->kernel_result = sceKernelGetFreeMemorySize(&kernel_info);
    snapshot->kernel_user = (unsigned)kernel_info.size_user;
    snapshot->kernel_cdram = (unsigned)kernel_info.size_cdram;
    snapshot->kernel_phycont = (unsigned)kernel_info.size_phycont;

    /* These are the four allocator domains owned by this vitaGL build.
     * Each pair is free/total; EXTERNAL is omitted because vitaGL documents
     * and implements its free-space query as zero rather than newlib state. */
    snapshot->vgl_ram_free = (unsigned)vglMemFree(VGL_MEM_RAM);
    snapshot->vgl_ram_total = (unsigned)vglMemTotal(VGL_MEM_RAM);
    snapshot->vgl_vram_free = (unsigned)vglMemFree(VGL_MEM_VRAM);
    snapshot->vgl_vram_total = (unsigned)vglMemTotal(VGL_MEM_VRAM);
    snapshot->vgl_phycont_free =
        (unsigned)vglMemFree(KAGE_VITA_VGL_MEM_PHYCONT);
    snapshot->vgl_phycont_total =
        (unsigned)vglMemTotal(KAGE_VITA_VGL_MEM_PHYCONT);
    snapshot->vgl_budget_free = (unsigned)vglMemFree(VGL_MEM_BUDGET);
    snapshot->vgl_budget_total = (unsigned)vglMemTotal(VGL_MEM_BUDGET);

    /* Copy the exact params from vitaGL's setup_render_target boundary. */
    memset(&render_target_params, 0, sizeof render_target_params);
    render_target_params.width = KAGE_VITA_RESERVED_RT_WIDTH;
    render_target_params.height = KAGE_VITA_RESERVED_RT_HEIGHT;
    render_target_params.scenesPerFrame = KAGE_VITA_RESERVED_RT_SCENES;
    render_target_params.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    render_target_params.driverMemBlock = (SceUID)-1;
    snapshot->render_target_query_result = sceGxmGetRenderTargetMemSize(
        &render_target_params, &snapshot->render_target_driver_bytes);
}

static void kage_vita_log_memory_snapshot(
    const char *phase, const kage_vita_memory_snapshot *snapshot,
    int include_event, int32_t event_result, const void *target)
{
    if (include_event) {
        KAGE_VITA_MEMORY_LOG(
            "KAGE VITA MEM: b=%.96s p=%s k=0x%08x "
            "kf(u/c/p)=%u/%u/%u vf/t(r/v/p/b)=%u/%u,%u/%u,%u/%u,%u/%u "
            "rq=0x%08x rb=%u ev=0x%08x t=0x%08x rt=1024x1024/8/0",
            ISAAC_VITA_IO_PROFILE_BUILD_ID, phase,
            (unsigned)snapshot->kernel_result,
            snapshot->kernel_user, snapshot->kernel_cdram,
            snapshot->kernel_phycont,
            snapshot->vgl_ram_free, snapshot->vgl_ram_total,
            snapshot->vgl_vram_free, snapshot->vgl_vram_total,
            snapshot->vgl_phycont_free, snapshot->vgl_phycont_total,
            snapshot->vgl_budget_free, snapshot->vgl_budget_total,
            (unsigned)snapshot->render_target_query_result,
            snapshot->render_target_driver_bytes,
            (unsigned)event_result, (unsigned)(uintptr_t)target);
    } else {
        KAGE_VITA_MEMORY_LOG(
            "KAGE VITA MEM: b=%.96s p=%s k=0x%08x "
            "kf(u/c/p)=%u/%u/%u vf/t(r/v/p/b)=%u/%u,%u/%u,%u/%u,%u/%u "
            "rq=0x%08x rb=%u rt=1024x1024/8/0 "
#if defined(ISAAC_VITA_DISPLAY_RASTER_720)
            "init=0/" KAGE_VITA_DISPLAY_WIDTH_TEXT "/"
            KAGE_VITA_DISPLAY_HEIGHT_TEXT "/16777216/0",
#else
            "init=0/960/544/16777216/0",
#endif
            ISAAC_VITA_IO_PROFILE_BUILD_ID, phase,
            (unsigned)snapshot->kernel_result,
            snapshot->kernel_user, snapshot->kernel_cdram,
            snapshot->kernel_phycont,
            snapshot->vgl_ram_free, snapshot->vgl_ram_total,
            snapshot->vgl_vram_free, snapshot->vgl_vram_total,
            snapshot->vgl_phycont_free, snapshot->vgl_phycont_total,
            snapshot->vgl_budget_free, snapshot->vgl_budget_total,
            (unsigned)snapshot->render_target_query_result,
            snapshot->render_target_driver_bytes);
    }
}

void kage_vita_backend_memory_snapshot_at_first_fbo(
    int32_t event_result, const void *target)
{
    kage_vita_memory_snapshot snapshot;

    if (!kage_vita_claim_memory_snapshot(
            KAGE_VITA_MEMORY_SNAPSHOT_FIRST_FBO))
        return;
    kage_vita_sample_memory(&snapshot);
    kage_vita_log_memory_snapshot(
        "first-1024-fbo-event", &snapshot, 1, event_result, target);
}

static void kage_vita_backend_memory_snapshot_after_init(void)
{
    kage_vita_memory_snapshot snapshot;

    if (!kage_vita_claim_memory_snapshot(KAGE_VITA_MEMORY_SNAPSHOT_POST_INIT))
        return;
    kage_vita_sample_memory(&snapshot);
    kage_vita_log_memory_snapshot(
        "vitagl-init-return", &snapshot, 0, 0, NULL);
}
#endif

static int kage_vita_log_heartbeat(unsigned count)
{
    /* Only explicitly requested diagnostics get dense early evidence and
     * then one record per 120 guest presents. Turning PHASE_PROFILE off must
     * not silently enable a different periodic observer. Keep the cadence
     * receipt's existing heartbeat/clock even without stage diagnostics. */
#if defined(ISAAC_VITA_PHASE_PROFILE) || \
    !((defined(ISAAC_VITA_STAGE_HEARTBEAT) && ISAAC_VITA_STAGE_HEARTBEAT) || \
      (defined(ISAAC_VITA_SIM_CADENCE_RECEIPT) && ISAAC_VITA_SIM_CADENCE_RECEIPT))
    (void)count;
    return 0;
#else
    return count != 0u &&
        ((count <= 64u && (count & (count - 1u)) == 0u) ||
         count % 120u == 0u);
#endif
}

static void kage_vita_set_error(const char *message)
{
    if (!message)
        message = "unknown Vita KAGE backend error";
    snprintf(s_error, sizeof s_error, "%s", message);
}

#if !defined(ISAAC_VITA_VITAGL_STOCK_REFERENCE)
static int kage_vita_display_surfaces_ready(
    const IsaacVitaGlDisplaySurfaceStatus *status)
{
    uint32_t i;

    if (!status || status->size != sizeof(*status) ||
            status->display_size != 0x00200000u ||
            status->buffer_count != ISAAC_VITAGL_DISPLAY_SURFACE_COUNT ||
            status->dedicated_count != ISAAC_VITAGL_DISPLAY_SURFACE_COUNT ||
            status->system_app_mode != 0u ||
            status->failure_stage != ISAAC_VITAGL_DISPLAY_SURFACE_STAGE_NONE ||
            status->failure_index != UINT32_MAX)
        return 0;
    for (i = 0; i < ISAAC_VITAGL_DISPLAY_SURFACE_COUNT; ++i) {
        if (status->alloc_result[i] < 0 ||
                status->get_base_result[i] != 0 ||
                status->map_result[i] != 0 ||
                status->dedicated[i] != 1u ||
                status->color_init_result[i] != 0 ||
                status->sync_create_result[i] != 0)
            return 0;
    }
    return 1;
}
#endif

static void kage_vita_backend_start_loading_epoch(void)
{
    /* A new counter epoch precedes both readers.  Start the independent
     * watchdog before loading_start's first vglSwapBuffers: if that call never
     * returns, pre-loop reports retain fread=0 and loading_swaps=0.  Watchdog
     * creation remains diagnostic-only. */
    isaac_vita_reset_archive_fread_epoch();
    KAGE_VITA_STALL_START();
    kage_vita_loading_start();
}

static void kage_vita_backend_activate_session(void)
{
    /* This is the common physical-Vita boundary used by manual_kage_vita.c.
     * The legacy kage_pc_backend_initialize wrapper is not on that route. */
    KAGE_VITA_FULLSPEED_RESET();
    KAGE_VITA_STABLE30_RESET();
#if defined(ISAAC_VITA_SIM_CADENCE_RECEIPT)
    kage_vita_sim_cadence_reset();
#endif
    s_active = 1;
}

/* The installed SDK archive may still contain vitaGL's optional splash
 * thread.  That thread asks Vita3K for a second sceGxm context and then dies
 * through a null context at +0x78.  CMake always links this with
 * --wrap=invoke_splashscreen, making the unwanted feature unreachable. */
void __wrap_invoke_splashscreen(void)
{
    sceClibPrintf("[kage-vita] vitaGL splash suppressed\n");
}

int kage_vita_backend_initialize(uint32_t width, uint32_t height)
{
#if !defined(ISAAC_VITA_VITAGL_STOCK_REFERENCE)
    IsaacVitaGlDisplaySurfaceStatus display_status;
#endif
    int state = s_context_state;

    if (width != KAGE_VITA_LOGICAL_WIDTH ||
        height != KAGE_VITA_LOGICAL_HEIGHT) {
        kage_vita_set_error(
            "Vita KAGE backend requires the measured 960x540 logical size");
        return 0;
    }

    if (state == KAGE_VITA_CONTEXT_READY) {
        if (!kage_vita_input_initialize()) {
            kage_vita_set_error("sceCtrl sampling initialization failed");
            return 0;
        }
        s_width = width;
        s_height = height;
        /* KAGE can ask to initialize an already-active, already-loaded
         * backend again.  Keep that request idempotent: restarting loading
         * here would rebase the watchdog onto a stale fread counter. */
        if (s_active)
            return 1;
#if defined(ISAAC_VITA_DISPLAY_RASTER_720)
        /* vitaGL survives KAGE Shutdown.  Restore the default-target native
         * viewport before a new guest backend session begins. */
        glViewport(0, KAGE_VITA_VIEWPORT_Y,
                   (GLsizei)KAGE_VITA_VIEWPORT_WIDTH,
                   (GLsizei)KAGE_VITA_VIEWPORT_HEIGHT);
#endif
        kage_vita_backend_activate_session();
        kage_vita_backend_start_loading_epoch();
        return 1;
    }
    if (state == KAGE_VITA_CONTEXT_INITIALIZING) {
        kage_vita_set_error("concurrent vitaGL initialization rejected");
        return 0;
    }
    if (state == KAGE_VITA_CONTEXT_FAILED) {
        kage_vita_set_error("vitaGL initialization previously failed");
        return 0;
    }
    if (!__sync_bool_compare_and_swap(
            &s_context_state, KAGE_VITA_CONTEXT_COLD,
            KAGE_VITA_CONTEXT_INITIALIZING)) {
        kage_vita_set_error("vitaGL initialization state changed concurrently");
        return 0;
    }
    if (!kage_vita_input_initialize()) {
        s_context_state = KAGE_VITA_CONTEXT_COLD;
        kage_vita_set_error("sceCtrl sampling initialization failed");
        return 0;
    }

#if !defined(ISAAC_VITA_VITAGL_STOCK_REFERENCE) ||     defined(ISAAC_VITA_STOCK_DISPLAY_RT_SCENES)
    /* Size the display render target for several GXM scenes per frame.  The
     * game binds the default framebuffer and the manager FBO about five times
     * per in-game frame (two more before c8dafea), and vitaGL ends and begins
     * a GXM scene at each switch.  With vitaGL's default of one scene per
     * frame the CPU blocks inside those GL calls waiting for the GPU as soon
     * as it presents faster than the GPU drains: Bundle33 measured the same
     * 33 draws / 5 clears / 5 binds per present costing 7 ms at ~30 presents
     * per second and 21 ms at ~45, with vglSwapBuffers itself at 0.1 ms
     * throughout.  The overlay profile has always requested eight (3575d1c);
     * the stock hardware-reference profile omitted it, and
     * ISAAC_VITA_STOCK_DISPLAY_RT_SCENES restores it there for an A/B. */
    vglSetupDisplayRenderTarget(
        (uint8_t)KAGE_VITA_RENDER_TARGET_SCENES);
#endif
#if defined(ISAAC_VITA_STOCK_FBO_RT_SCENES)
    /* Offscreen (FBO) render targets: N scene slots instead of vitaGL's one,
     * so the three offscreen passes of a frame on the manager FBO no longer
     * serialize CPU and GPU at each sceGxmEndScene (see the value macro).
     * The patch stores the request only when it is 1..MAX_SCENES_PER_FRAME
     * and returns what it will use; the creation site falls back to one slot
     * if sceGxmCreateRenderTarget refuses the larger target (ph120.gx rt(x)).
     * Once per process: the warm re-initialize after Shutdown reuses the
     * vitaGL context and returns before this block. */
    s_stock_fbo_rt_scenes = vglIsaacSetupFboRenderTargetScenes(
        (uint8_t)ISAAC_VITA_STOCK_FBO_RT_SCENES);
#endif
#if defined(ISAAC_VITA_STOCK_FBO_VALID_REGION)
    /* Offscreen valid region (0009): 0 = observe (count only), 1 = apply the
     * logical rect to every FBO sceGxmBeginScene.  Once per process like the
     * FBO scenes request above; the banner prints the mode the patch
     * returned.  The display render target is never touched. */
    s_stock_fbo_valid_region_apply = vglIsaacSetupFboValidRegion(
        (uint8_t)((ISAAC_VITA_STOCK_FBO_VALID_REGION) == 2));
#endif

#if defined(ISAAC_VITA_VITAGL_CACHED_MEM)
    /* vitaGL copies every client vertex array and uniform block into its RAM
     * pool on each draw; that pool is USER_RW_UNCACHE unless this is set
     * before vglInit*, so every one of those per-draw stores goes uncached
     * (~212 attribute pointers and 33 draws per in-game present).  Cached
     * pools let sceGxm handle cache maintenance instead.  A/B knob. */
    vglUseCachedMem(GL_TRUE);
#endif
    /* This order is the mature Vita-port contract.  Current vitaGL already
     * prefers VRAM, so the removed vglUseVram API must not be called. */
    vglSetupRuntimeShaderCompiler(
        SHARK_OPT_UNSAFE, SHARK_ENABLE, SHARK_ENABLE, SHARK_ENABLE);

    /* The return is res_fallback (resolution was clamped), not success. */
#if defined(ISAAC_VITA_VITAGL_STOCK_REFERENCE)
    (void)vglInitExtended(
        0, KAGE_VITA_DISPLAY_WIDTH, KAGE_VITA_DISPLAY_HEIGHT,
        KAGE_VITA_RAM_THRESHOLD, SCE_GXM_MULTISAMPLE_NONE);
#else
    (void)vglInitWithCustomThreshold(
        0, KAGE_VITA_DISPLAY_WIDTH, KAGE_VITA_DISPLAY_HEIGHT,
        KAGE_VITA_RAM_THRESHOLD, KAGE_VITA_CDRAM_THRESHOLD,
        KAGE_VITA_PHYCONT_THRESHOLD, KAGE_VITA_CDIALOG_THRESHOLD,
        SCE_GXM_MULTISAMPLE_NONE);
    if (!gl_vita_backend_get_display_surface_status(&display_status) ||
            !kage_vita_display_surfaces_ready(&display_status)) {
        kage_vita_input_deactivate();
        s_context_state = KAGE_VITA_CONTEXT_FAILED;
        kage_vita_set_error(
            "vitaGL did not create three dedicated display surfaces");
        return 0;
    }
#endif
#if defined(ISAAC_VITA_IO_PROFILE)
    kage_vita_backend_memory_snapshot_after_init();
#endif

    s_gl_version = (const char *)glGetString(GL_VERSION);
    if (!s_gl_version || !s_gl_version[0]) {
        kage_vita_input_deactivate();
        s_context_state = KAGE_VITA_CONTEXT_FAILED;
        kage_vita_set_error("vitaGL initialized without a GL_VERSION string");
        return 0;
    }

    /* KAGE retains its measured 960x540 logical coordinate system.  The
     * optional scaled display raster (720x408 or 480x272) scales only the
     * default framebuffer and uses a centred NUM/DEN viewport (720x405 or
     * 480x270); KAGE may replace it later through the GL boundary, which
     * applies the same logical-to-native transform. */
#if defined(ISAAC_VITA_DISPLAY_RASTER_720)
    glViewport(0, KAGE_VITA_VIEWPORT_Y,
               (GLsizei)KAGE_VITA_VIEWPORT_WIDTH,
               (GLsizei)KAGE_VITA_VIEWPORT_HEIGHT);
#else
    glViewport(0, KAGE_VITA_VIEWPORT_Y,
               (GLsizei)width, (GLsizei)height);
#endif
    vglWaitVblankStart(GL_FALSE);

    s_width = width;
    s_height = height;
    s_time_origin = sceKernelGetProcessTimeWide();
    s_present_count = 0u;
    s_vsync_change_count = 0u;
    s_vsync_enabled = 0;
    kage_vita_backend_activate_session();
    s_error[0] = '\0';
    __sync_synchronize();
    s_context_state = KAGE_VITA_CONTEXT_READY;
    kage_vita_backend_start_loading_epoch();
#if defined(ISAAC_VITA_VITAGL_STOCK_REFERENCE)
# if defined(ISAAC_VITA_DISPLAY_RASTER_720)
    sceClibPrintf(
        "[kage-vita] vitaGL ready: profile=stock-vitagl-reference "
        "source=73dd57a display=" KAGE_VITA_DISPLAY_WIDTH_TEXT "x"
        KAGE_VITA_DISPLAY_HEIGHT_TEXT " panel=960x544 "
        "logical=960x540 rt-scenes=%u" KAGE_VITA_STOCK_FBO_RT_SCENES_BANNER
        KAGE_VITA_STOCK_FBO_VALID_REGION_BANNER " GL=%s\n",
        (unsigned)KAGE_VITA_STOCK_DISPLAY_RT_SCENES_VALUE,
        KAGE_VITA_STOCK_FBO_RT_SCENES_ARG
        KAGE_VITA_STOCK_FBO_VALID_REGION_ARG s_gl_version);
# else
    sceClibPrintf(
        "[kage-vita] vitaGL ready: profile=stock-vitagl-reference "
        "source=73dd57a physical=960x544 logical=960x540 "
        "rt-scenes=%u" KAGE_VITA_STOCK_FBO_RT_SCENES_BANNER
        KAGE_VITA_STOCK_FBO_VALID_REGION_BANNER " GL=%s\n",
        (unsigned)KAGE_VITA_STOCK_DISPLAY_RT_SCENES_VALUE,
        KAGE_VITA_STOCK_FBO_RT_SCENES_ARG
        KAGE_VITA_STOCK_FBO_VALID_REGION_ARG s_gl_version);
# endif
#else
# if defined(ISAAC_VITA_DISPLAY_RASTER_720)
    sceClibPrintf(
        "[kage-vita] vitaGL ready: display=" KAGE_VITA_DISPLAY_WIDTH_TEXT
        "x" KAGE_VITA_DISPLAY_HEIGHT_TEXT " panel=960x544 "
        "logical=960x540 "
        "rt-scenes=%u GL=%s\n",
        (unsigned)KAGE_VITA_RENDER_TARGET_SCENES, s_gl_version);
# else
    sceClibPrintf(
        "[kage-vita] vitaGL ready: physical=960x544 logical=960x540 "
        "rt-scenes=%u GL=%s\n",
        (unsigned)KAGE_VITA_RENDER_TARGET_SCENES, s_gl_version);
# endif
#endif
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    {
        /* Boot receipt of the diagnostic build: the census itself is proven
         * per window by the bid-tagged ph120.fa/ph120.fp records; this line
         * pins the dump knob and its thresholds, which leave no other trace
         * until a dump fires. */
        uint32_t dump_render_p50_us;
        uint32_t dump_min_window;
        uint32_t dump_window;
        uint32_t dump_frames;
        uint32_t dump = gl_vita_backend_fill_census_dump_config(
            &dump_render_p50_us, &dump_min_window, &dump_window,
            &dump_frames);

        sceClibPrintf(
            "[kage-vita] GL fill census: on records=ph120.fa,ph120.fp "
            "dump=%u rnd_us=%u min_win=%u win=%u frames=%u\n",
            (unsigned)dump, (unsigned)dump_render_p50_us,
            (unsigned)dump_min_window, (unsigned)dump_window,
            (unsigned)dump_frames);
    }
#endif
    return 1;
}

void kage_vita_backend_deactivate(void)
{
    gl_vita_backend_attrib_sync();
    /* Join the diagnostic reader before any process-owned guest/backend state
     * can be released.  Stop is idempotent across ordinary and final teardown. */
    KAGE_VITA_STALL_STOP();
    kage_vita_loading_finish();
    s_active = 0;
    KAGE_VITA_FULLSPEED_DEACTIVATE();
    KAGE_VITA_STABLE30_DEACTIVATE();
    kage_vita_input_deactivate();
}

int kage_vita_backend_present(void)
{
#if defined(ISAAC_VITA_DEEP_PROFILE)
    /* This includes the transition's Present inside Manager::Update, not
     * just the ordinary Render phase. It is API wall time, not GPU time. */
    KAGE_VITA_DEEP_SCOPE(KVD_GPU_WAIT);
#endif
    if (!kage_vita_backend_ready()) {
        kage_vita_set_error("Vita KAGE Present requested before initialize");
        return 0;
    }
#if defined(ISAAC_VITA_GL_WRAPPER_TIME)
    /* ph120.gw p bucket: this whole method (attribute sync, loading finish,
     * shader-cache/fill-census bookkeeping, the swap, the heartbeat) on the
     * gt clock; its gt body is the vglSwapBuffers bracket below. */
    uint64_t wrapper_started_at = sceKernelGetProcessTimeWide();
#endif
    KAGE_VITA_STALL_NOTE_PRESENT_ENTER(s_present_count);
    gl_vita_backend_attrib_sync();
    /* Belt-and-suspenders fallback.  The generated loop-head hook normally
     * drains the CPU loading overlay before Update/Render starts; Present is
     * the final boundary at which the display callback must be gone. */
    kage_vita_loading_finish();
#if defined(ISAAC_VITA_VITAGL_SHADER_CACHE)
    gl_vita_backend_shader_cache_report();
#endif
#if defined(ISAAC_VITA_FIRST_FRAME_PROBE)
    gl_vita_backend_first_frame_before_present(s_present_count);
    gl_vita_backend_first_frame_queue_begin(s_present_count);
#endif
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION)
    gl_vita_backend_fbo_present();
#endif
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    /* Pass ordinal reset + one-frame dump state machine; no logging here. */
    gl_vita_backend_fill_census_present();
#endif
    KAGE_VITA_PHASE_PROFILE_PRESENT_ENTER();
#if defined(ISAAC_VITA_GL_TIME_PROFILE) && \
    (!defined(ISAAC_VITA_GL_TIME_SDK_SPARSE) || !ISAAC_VITA_GL_TIME_SDK_SPARSE)
    {
        /* Same bracket as the GL boundary buckets: the swap ends the display
         * scene (sceGxmEndScene) and queues the flip; it is the sixth bucket
         * of ph120.gt and coincides with the ph120.t swp phase. */
        uint64_t swap_started_at = sceKernelGetProcessTimeWide();

        vglSwapBuffers(GL_FALSE);
        isaac_vita_gl_time_add(
            &g_isaac_vita_gl_time_profile.present, swap_started_at,
            sceKernelGetProcessTimeWide());
    }
#else
    vglSwapBuffers(GL_FALSE);
#endif
    KAGE_VITA_PHASE_PROFILE_PRESENT_RETURN();
#if defined(ISAAC_VITA_FIRST_FRAME_PROBE)
    gl_vita_backend_first_frame_queue_end();
#endif
#if !defined(ISAAC_VITA_VITAGL_STOCK_REFERENCE)
    {
        IsaacVitaGlDisplaySurfaceStatus display_status;

        if (!gl_vita_backend_get_display_surface_status(&display_status) ||
                !kage_vita_display_surfaces_ready(&display_status)) {
            s_active = 0;
            s_context_state = KAGE_VITA_CONTEXT_FAILED;
            kage_vita_set_error(
                "vitaGL lost its three dedicated display surfaces");
            return 0;
        }
    }
#endif
#if defined(ISAAC_VITA_RAW_GXM_PROBE)
    kage_vita_raw_gxm_probe_end_frame(s_present_count);
#endif
    s_present_count++;
#if defined(ISAAC_VITA_SCREENSHOT_PROBE)
    /* This synchronous seam is still the vitaGL owner thread, after the 600th
     * submitted swap returns.  It does not claim that scanout has completed. */
    kage_vita_screenshot_probe_post_swap(s_present_count);
#endif
#if defined(ISAAC_VITA_FIRST_FRAME_PROBE)
    kage_vita_first_frame_note_present_return(s_present_count);
#endif
    KAGE_VITA_STALL_NOTE_PRESENT_RETURN(s_present_count);
    if (s_present_count == 1u) {
        sceClibPrintf("[kage-vita] first present complete\n");
#if defined(ISAAC_VITA_PHASE_PROFILE) && defined(GUEST_IMAGE_BASE) && \
    !defined(ISAAC_KAGE_VITA_BACKEND_ORACLE)
        /* Profile builds: one read of mimalloc's os_preloading byte in the
         * mapped image (RVA 0x7aa2ec, .data) once the first frame reached
         * the display.  Non-zero means the CRT still believes it is
         * pre-loading, which keeps mi_option_get re-running mi_option_init;
         * zero means the sampler's mi_option_init attribution came from a
         * stale stack word.  Device only: the host oracle has no image. */
        sceClibPrintf("[kage-vita] mi os_preloading=%u\n",
                      (unsigned)*(const volatile uint8_t *)(uintptr_t)
                          (GUEST_IMAGE_BASE + UINT32_C(0x007aa2ec)));
#endif
    }
    if (kage_vita_log_heartbeat(s_present_count)) {
        uint64_t now = sceKernelGetProcessTimeWide();
        uint32_t elapsed_ms = now >= s_time_origin
            ? (uint32_t)((now - s_time_origin) / 1000u) : 0u;

        ISAAC_VITA_LOG_PRINTF(
            "[kage-vita] present heartbeat count=%u elapsed_ms=%u\n",
            s_present_count, elapsed_ms);
#if defined(ISAAC_VITA_SIM_CADENCE_RECEIPT)
        /* This report is deliberately adjacent to the established present
         * heartbeat and reuses its one clock sample.  The Game::Update hot
         * seam itself performs no timing or logging. */
        kage_vita_sim_cadence_report_present_heartbeat(
            s_present_count, elapsed_ms);
#endif
    }
#if defined(ISAAC_VITA_GL_WRAPPER_TIME)
    isaac_vita_gl_wrapper_time_add(
        &g_isaac_vita_gl_wrapper_time.wrapper[ISAAC_VITA_GL_WRAPPER_PRESENT],
        wrapper_started_at, sceKernelGetProcessTimeWide());
#endif
    return 1;
}

int kage_vita_backend_set_vsync(int enabled)
{
    int requested;

    if (!kage_vita_backend_ready()) {
        kage_vita_set_error("Vita KAGE vsync requested before initialize");
        return 0;
    }
    requested = enabled != 0;
    /* The frozen guest's own monotonic 1/60 limiter is the only pacing
     * authority on Vita.  Enabling vitaGL swap wait also makes the guest skip
     * that limiter, coupling simulation speed directly to emulator Presents. */
    vglWaitVblankStart(GL_FALSE);
    s_vsync_enabled = 0;
    s_vsync_change_count++;
    ISAAC_VITA_LOG_PRINTF(
        "[kage-vita] vsync request=%u actual=0 policy=software-60hz\n",
        (unsigned)requested);
    return 1;
}

int kage_vita_backend_time_seconds(double *seconds)
{
    uint64_t now;
    if (!seconds || !kage_vita_backend_ready()) {
        kage_vita_set_error("Vita KAGE clock requested before initialize");
        return 0;
    }
    now = sceKernelGetProcessTimeWide();
    if (now < s_time_origin) {
        kage_vita_set_error("Vita process clock moved backwards");
        return 0;
    }
    *seconds = (double)(now - s_time_origin) / 1000000.0;
    return 1;
}

int kage_vita_backend_ready(void)
{
    return s_context_state == KAGE_VITA_CONTEXT_READY && s_active != 0;
}

uint32_t kage_vita_backend_width(void) { return s_width; }
uint32_t kage_vita_backend_height(void) { return s_height; }
unsigned kage_vita_backend_present_count(void) { return s_present_count; }
unsigned kage_vita_backend_vsync_change_count(void)
{
    return s_vsync_change_count;
}
int kage_vita_backend_vsync_enabled(void) { return s_vsync_enabled; }
const char *kage_vita_backend_gl_version(void)
{
    return s_gl_version ? s_gl_version : "vitaGL context is not initialized";
}
const char *kage_vita_backend_last_error(void)
{
    return s_error[0] ? s_error : "Vita KAGE backend: no error";
}
