/* Single process-wide vitaGL context for the recompiled KAGE boundary.
 *
 * vitaGL has no teardown API and suppresses a second initialization itself.
 * Keep the context alive after KAGE Shutdown, but mark the current KAGE
 * session inactive.  A later Initialize reuses the real context instead of
 * trying to manufacture or recreate a GLFW object. */
#include "kage_vita_backend.h"
#if defined(ISAAC_VITA_GL_THREAD_RECORD) && ISAAC_VITA_GL_THREAD_RECORD
#include "gl_record_backend.h"
#endif
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
#if defined(ISAAC_VITA_TROPHIES)
# include "host_vita_trophies.h"
#endif
#if defined(ISAAC_VITA_FIRST_FRAME_PROBE)
void kage_vita_first_frame_note_present_return(uint32_t present_count);
#endif
#if defined(ISAAC_VITA_DEBUG_OVERLAY)
# include "host_vita_debug_overlay.h"
#endif
#if defined(ISAAC_VITA_GPU_FRAME_PROFILE)
# include "kage_vita_gpu_frame_profile.h"
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
/* ISAAC_VITA_VITAGL_SCENE_GUARD: the counters of 0034-isaac-scene-guard.patch.
 * Stock vitaGL ignores setup_render_target and sceGxmBeginScene failures in
 * scene_reset (sceGxmCreateRenderTarget refuses under driver-memory pressure,
 * seen at Ultra Greed's death); glClear then wrote its clear uniforms through
 * an uninitialised default-uniform-buffer pointer (Vita C2-12828-1, data
 * abort at dfar 0x10e).  The patch fails those writes and draws closed and
 * counts them; this owner logs a startup receipt and, after a swap on which
 * the counters changed, their snapshot: the first eight changes, then every
 * 256th.  The hooks are declared here, not in vitaGL.h (the 0004 shader
 * cache hashes that header).  A failed sceGxmEndScene leaves the scene open
 * inside GXM (2026-09-27: every later BeginScene returned WITHIN_SCENE and
 * the display froze); the guard keeps retrying the end like stock vitaGL, and
 * the recovery words say whether a scene is still dangling, how many ends
 * were retried and recovered, and the last EndScene error itself. */
#if defined(ISAAC_VITA_VITAGL_SCENE_GUARD)
void vglIsaacSceneGuardSnapshot(uint32_t out[8]);
void vglIsaacSceneGuardRecovery(uint32_t out[4]);
uint32_t vglIsaacSceneGuardChanged(void);
void isaac_vita_log(const char *format, ...);
static uint32_t s_scene_guard_seen;
static uint32_t s_scene_guard_changes;

static void kage_vita_scene_guard_receipt(void)
{
    uint32_t events = vglIsaacSceneGuardChanged();
    uint32_t counters[8];
    uint32_t recovery[4];

    if (events == s_scene_guard_seen)
        return;
    s_scene_guard_seen = events;
    s_scene_guard_changes++;
    if (s_scene_guard_changes > 8u && (s_scene_guard_changes % 256u) != 0u)
        return;
    vglIsaacSceneGuardSnapshot(counters);
    vglIsaacSceneGuardRecovery(recovery);
    isaac_vita_log(
        "KAGE VITA SCENE GUARD: rt_fail=%u begin_fail=%u end_fail=%u "
        "reserve_fail=%u skipped(clear,draw)=%u,%u last_err=0x%08x "
        "skipped_end=%u dangling=%u end_retry=%u recovered=%u "
        "end_err=0x%08x changes=%u",
        (unsigned)counters[0], (unsigned)counters[1], (unsigned)counters[2],
        (unsigned)counters[3], (unsigned)counters[4], (unsigned)counters[5],
        (unsigned)counters[7], (unsigned)counters[6],
        (unsigned)recovery[0], (unsigned)recovery[1], (unsigned)recovery[2],
        (unsigned)recovery[3], (unsigned)s_scene_guard_changes);
}
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

#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
/* Render thread: every vitaGL call runs there.  The
 * game thread keeps session/loading ownership, counters and the heartbeat. */
#include <psp2/kernel/threadmgr.h>
#include "gl_owner_check.h"
static int s_defer_session, s_owed_activate, s_owed_loading;
static uint32_t s_present_position;
static void kvb_noop_closure(void *unused) { (void)unused; }
#endif

static void kage_vita_backend_start_loading_epoch(void)
{
    /* A new counter epoch precedes both readers.  Start the independent
     * watchdog before loading_start's first vglSwapBuffers: if that call never
     * returns, pre-loop reports retain fread=0 and loading_swaps=0.  Watchdog
     * creation remains diagnostic-only. */
#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
    if (s_defer_session) { s_owed_loading = 1; return; } /* game thread */
#endif
    isaac_vita_reset_archive_fread_epoch();
    KAGE_VITA_STALL_START();
    kage_vita_loading_start();
}

static void kage_vita_backend_activate_session(void)
{
    /* This is the common physical-Vita boundary used by manual_kage_vita.c.
     * The legacy kage_pc_backend_initialize wrapper is not on that route. */
#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
    if (s_defer_session) { s_owed_activate = 1; return; } /* game thread */
#endif
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

#if defined(ISAAC_VITA_TROPHIES)
/* Pumps one trophy setup-dialog frame: vglSwapBuffers(GL_TRUE) runs
 * sceCommonDialogUpdate on the back buffer, so the trophy unit never
 * includes vitaGL. */
static void kage_vita_trophy_present(void)
{
    vglSwapBuffers(GL_TRUE);
}
#endif

#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
static int kage_vita_backend_initialize_impl(uint32_t width, uint32_t height)
#else
int kage_vita_backend_initialize(uint32_t width, uint32_t height)
#endif
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
#if defined(ISAAC_VITA_VITAGL_SCENE_GUARD)
    /* Startup receipt of the 0034 guard, once per process like the requests
     * below (the warm re-initialize after Shutdown returned above). */
    isaac_vita_log("KAGE VITA SCENE GUARD: on");
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
#if defined(ISAAC_VITA_TROPHIES)
    /* Trophy Stage B (recomp/vita/TROPHIES.md): opens the sets with queued
     * trophies one at a time and then the gameplay pair, one bounded setup
     * dialog per context (at most two live; device run 2026-10-09); no
     * dialog is ever shown later.  Only here does this thread own GXM with
     * no scene open, the
     * loading overlay callback not yet installed (kage_vita_loading.c
     * overwrites the whole framebuffer), vsync at vitaGL's default and the
     * scheduler, watchdog and present counters not started; the warm path
     * above returned earlier.  A no-op when Stage A disabled trophies. */
    isaac_vita_trophies_setup(kage_vita_trophy_present);
#endif

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

#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
typedef struct KvbInit { uint32_t width, height; int result; } KvbInit;
#if defined(ISAAC_GL_RECORD_ADMISSION_RECEIPT)
/* GL_RECORD_ADMISSION_CACHE_STANDALONE only: the boot receipt names the free
 * wrap the recorder's admission cache relies on. */
# include <psp2/kernel/sysmem.h>
/* Weak: resolves to 0 if no free wrap is linked (the receipt then says MISSING). */
int __wrap_sceKernelFreeMemBlock(SceUID uid) __attribute__((weak));
#endif
#if defined(ISAAC_VITA_GL_THREAD_VGL_MEM_SNAPSHOT)
/* vitaGL's allocator is single-threaded, so the game-thread readers of its
 * free space (image retain headroom and ghost, heap census) read this copy.
 * The render thread publishes it after init and after every swap: at most
 * one frame old, inside the FRAME_PURGE_FREQ frames by which vitaGL already
 * defers texture frees. */
/* Same ABI slot as KAGE_VITA_VGL_MEM_PHYCONT (copied from the IO_PROFILE block
 * above, which this knob does not depend on). */
#define KVB_VGL_MEM_PHYCONT ((vglMemType)(VGL_MEM_RAM + 1))
static volatile uint32_t s_vgl_mem[KAGE_VITA_VGL_SNAPSHOT_WORDS];
static void kvb_publish_vgl_mem(void)
{
    __atomic_store_n(&s_vgl_mem[KAGE_VITA_VGL_SNAPSHOT_RAM_FREE],
                     (uint32_t)vglMemFree(VGL_MEM_RAM), __ATOMIC_RELAXED);
    __atomic_store_n(&s_vgl_mem[KAGE_VITA_VGL_SNAPSHOT_VRAM_FREE],
                     (uint32_t)vglMemFree(VGL_MEM_VRAM), __ATOMIC_RELAXED);
    __atomic_store_n(&s_vgl_mem[KAGE_VITA_VGL_SNAPSHOT_PHYCONT_FREE],
                     (uint32_t)vglMemFree(KVB_VGL_MEM_PHYCONT), __ATOMIC_RELAXED);
    __atomic_store_n(&s_vgl_mem[KAGE_VITA_VGL_SNAPSHOT_RAM_TOTAL],
                     (uint32_t)vglMemTotal(VGL_MEM_RAM), __ATOMIC_RELAXED);
}
uint32_t kage_vita_backend_vgl_mem(unsigned index)
{
    return index < KAGE_VITA_VGL_SNAPSHOT_WORDS
        ? __atomic_load_n(&s_vgl_mem[index], __ATOMIC_RELAXED) : 0u;
}
#else
# define kvb_publish_vgl_mem() ((void)0)
#endif

static void kvb_initialize_closure(void *context)
{
    KvbInit *init = (KvbInit *)context;
    init->result = kage_vita_backend_initialize_impl(init->width, init->height);
    if (init->result)
        kvb_publish_vgl_mem();
}

/* Start the render thread once, narrow the game thread to USER_0 and log the
 * RESOLVED ids/masks/mode (not the intent). */
static int kvb_worker_ensure(void)
{
    int self, before, after, rc, render, render_mask;
    if (isaac_gl_record_worker_active())
        return 1;
    self = sceKernelGetThreadId();
    before = sceKernelGetThreadCpuAffinityMask(self);
    rc = sceKernelChangeThreadCpuAffinityMask(self, SCE_KERNEL_CPU_MASK_USER_0);
    after = sceKernelGetThreadCpuAffinityMask(self);
    if (!isaac_gl_record_worker_start(NULL)) {
        isaac_vita_log("KAGE VITA GL RENDER THREAD: start FAILED");
        return 0;
    }
    isaac_gl_record_worker_call(kvb_noop_closure, NULL); /* thread id is set */
    render = isaac_gl_record_worker_thread_id();
    render_mask = sceKernelGetThreadCpuAffinityMask(render);
    isaac_vita_log(
        "KAGE VITA GL RENDER THREAD: mode=%s game_tid=0x%08x "
        "game_mask_before=0x%08x game_mask_after=0x%08x game_mask_rc=0x%08x "
        "render_tid=0x%08x render_mask=0x%08x render_free_stack=%d",
        ISAAC_GL_RECORD_WORKER_MODE_NAME, (unsigned)self, (unsigned)before,
        (unsigned)after, (unsigned)rc, (unsigned)render, (unsigned)render_mask,
        sceKernelGetThreadStackFreeSize(render));
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_WAIT) && ISAAC_VITA_GL_THREAD_ADMIT_WAIT
    isaac_vita_log("KAGE VITA GL ADMIT WAIT: mode=%s bound=time-test slice_us=2000 backoff=30 record=ph120.aw",
                   ISAAC_VITA_GL_THREAD_ADMIT_WAIT == 2 ? "on" : "observe");
#endif
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND
    /* bytes: the value the scheduler's gate compares with, read back from
     * that translation unit, not the option text. */
    isaac_vita_log("KAGE VITA GL ADMIT BEHIND: mode=%s bytes=%u backoff=%u record=ph120.ab",
                   ISAAC_VITA_GL_THREAD_ADMIT_BEHIND == 2 ? "on" : "observe",
                   (unsigned)kage_vita_fullspeed_scheduler_gl_admit_behind_bytes(),
                   (unsigned)KAGE_VITA_GL_ADMIT_BEHIND_BACKOFF);
#endif
#if defined(ISAAC_GL_RECORD_ADMISSION_RECEIPT)
    {
        /* --wrap redirects this TU's reference too: equal iff the wrap is
         * linked.  volatile copies keep GCC from folding the comparison of
         * two symbol addresses. */
        volatile uintptr_t called = (uintptr_t)&sceKernelFreeMemBlock;
        volatile uintptr_t wrap = (uintptr_t)&__wrap_sceKernelFreeMemBlock;
        int linked = wrap != 0u && called == wrap;
# if defined(ISAAC_GL_RECORD_ADMISSION_CACHE_VERIFY)
        uint32_t st[4];
        int st_ok = isaac_gl_record_admission_selftest(st);   /* game thread = producer */
        isaac_vita_log("KAGE VITA GL RECORD ADMISSION: cache=on owner=%s wrap=%s verify=1 "
                       "selftest(cached,bumped,stale,after)=%u,%u,%u,%u ok=%d",
                       ISAAC_GL_RECORD_ADMISSION_RECEIPT, linked ? "linked" : "MISSING",
                       (unsigned)st[0], (unsigned)st[1], (unsigned)st[2], (unsigned)st[3], st_ok);
# else
        isaac_vita_log("KAGE VITA GL RECORD ADMISSION: cache=on owner=%s wrap=%s verify=0",
                       ISAAC_GL_RECORD_ADMISSION_RECEIPT, linked ? "linked" : "MISSING");
# endif
    }
#endif
    return 1;
}

int kage_vita_backend_initialize(uint32_t width, uint32_t height)
{
    KvbInit init;
    if (!kvb_worker_ensure()) {
        kage_vita_set_error("GL render thread could not start");
        return 0;
    }
    init.width = width;
    init.height = height;
    init.result = 0;
    s_defer_session = 1;
    s_owed_activate = s_owed_loading = 0;
    isaac_gl_record_worker_call(kvb_initialize_closure, &init);
    s_defer_session = 0;
    /* Same order as the impl: activate, then the loading epoch. */
    if (s_owed_activate) kage_vita_backend_activate_session();
    if (s_owed_loading) kage_vita_backend_start_loading_epoch();
    return init.result;
}
#endif

#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
static void kvb_attrib_sync_closure(void *unused)
{
    (void)unused;
    gl_vita_backend_attrib_sync();
}
#endif

void kage_vita_backend_deactivate(void)
{
#if defined(ISAAC_VITA_GL_THREAD_RECORD) && ISAAC_VITA_GL_THREAD_RECORD
    isaac_gl_record_drain();
#endif
#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
    isaac_gl_record_worker_wait(s_present_position);
    isaac_gl_record_worker_call(kvb_attrib_sync_closure, NULL);
#else
    gl_vita_backend_attrib_sync();
#endif
    /* Join the diagnostic reader before any process-owned guest/backend state
     * can be released.  Stop is idempotent across ordinary and final teardown. */
    KAGE_VITA_STALL_STOP();
    kage_vita_loading_finish();
    s_active = 0;
    KAGE_VITA_FULLSPEED_DEACTIVATE();
    KAGE_VITA_STABLE30_DEACTIVATE();
    kage_vita_input_deactivate();
}

#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
#if defined(ISAAC_VITA_GL_THREAD_RENDER_ADMISSION) && ISAAC_VITA_GL_THREAD_RENDER_ADMISSION
/* Fullspeed scheduler, interpolated (non-full) phase only: while the previous
 * present closure has not run, rendering this frame would make Present wait
 * for the render thread and delay the next Game::Update.  Decline it (one
 * frame stays in flight, the update cadence is untouched).  Never waits. */
int kage_vita_backend_render_admit(void)
{
    if (!isaac_gl_record_worker_behind(s_present_position))
        return 1;
    isaac_gl_record_worker_note_render_skip();
    return 0;
}
#endif
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_WAIT) && ISAAC_VITA_GL_THREAD_ADMIT_WAIT
# if !defined(ISAAC_VITA_GL_THREAD_RENDER_ADMISSION) || !ISAAC_VITA_GL_THREAD_RENDER_ADMISSION
#  error "GL admit wait needs GL render admission"
# endif
/* GL admit wait (R60-DRYADMIT-SPEC-20261028.md).  Game thread only, except
 * s_admit_done_us: the present closure writes it on the render thread before
 * its record's done_pos is published (release, gl_record_ring.c:249) and the
 * game thread reads it only after worker_wait saw that done_pos (acquire,
 * gl_record_ring.c:257). */
static uint64_t s_admit_done_us;
static uint32_t s_admit_fit_armed, s_admit_fit_position;
static uint64_t s_admit_fit_latest_us;
/* e, a, miss_us, miss_max_us; hd n, hd us (ON: Present holds on a closure
 * that was admitted after a wait). */
static uint32_t s_admit_fit[6];
# if ISAAC_VITA_GL_THREAD_ADMIT_WAIT == 2
/* Review round 2 finding 1: the scheduler arms s_admit_waited_arm when it
 * admits a frame after a wait; that frame's Present records its closure's
 * position; the next Present that finds this closure still running holds the
 * game thread, and the hold (us, at least 1) waits in s_admit_hold_us for the
 * scheduler's next loop head (kage_vita_backend_admit_hold_take). */
static uint32_t s_admit_waited_arm, s_admit_waited_position, s_admit_hold_us;
# endif

/* The stock admission predicate, without its rs count. */
int kage_vita_backend_render_behind(void)
{
    return isaac_gl_record_worker_behind(s_present_position);
}

/* The gate declined while behind: ph120.rw rs as kage_vita_backend_render_admit;
 * latest_us != 0 arms the fit check at the next Present. */
void kage_vita_backend_render_refused(uint64_t latest_us)
{
    isaac_gl_record_worker_note_render_skip();
    if (latest_us) {
        s_admit_fit_armed = 1u;
        s_admit_fit_position = s_present_position;
        s_admit_fit_latest_us = latest_us;
    }
}

# if ISAAC_VITA_GL_THREAD_ADMIT_WAIT == 2
int kage_vita_backend_render_wait_until(uint64_t deadline_us)
{
    return isaac_gl_record_worker_wait_until(s_present_position, deadline_us);
}

void kage_vita_backend_render_admitted_after_wait(void)
{
    s_admit_waited_arm = 1u;
}

uint32_t kage_vita_backend_admit_hold_take(void)
{
    uint32_t hold_us = s_admit_hold_us;

    s_admit_hold_us = 0u;
    return hold_us;
}

/* hd(n,us): Present, around the stock worker_wait (which stays one unguarded
 * line, so OFF is the stock text): is this Present about to wait for a
 * closure admitted after a wait, and for how long did it. */
static uint32_t s_admit_hold_armed;
static uint64_t s_admit_hold_at;

static void kvb_admit_hold_begin(void)
{
    s_admit_hold_armed = s_admit_waited_position != 0u &&
        s_admit_waited_position == s_present_position &&
        isaac_gl_record_worker_behind(s_present_position);
    s_admit_hold_at = s_admit_hold_armed ? sceKernelGetProcessTimeWide() : 0u;
    s_admit_waited_position = 0u;
}

static void kvb_admit_hold_end(void)
{
    uint64_t now, hold, sum;

    if (!s_admit_hold_armed)
        return;
    s_admit_hold_armed = 0u;
    now = sceKernelGetProcessTimeWide();
    hold = now > s_admit_hold_at ? now - s_admit_hold_at : 0u;
    s_admit_hold_us = hold == 0u ? 1u :
        hold > UINT32_MAX ? UINT32_MAX : (uint32_t)hold;
    if (s_admit_fit[4] != UINT32_MAX)
        ++s_admit_fit[4];
    sum = (uint64_t)s_admit_fit[5] + s_admit_hold_us;
    s_admit_fit[5] = sum > UINT32_MAX ? UINT32_MAX : (uint32_t)sum;
}
# endif

void kage_vita_backend_gl_admit_fit_take(uint32_t out[6])
{
    if (out)
        memcpy(out, s_admit_fit, sizeof s_admit_fit);
    memset(s_admit_fit, 0, sizeof s_admit_fit);
}

/* Present, after worker_wait(s_present_position): that closure has run, so
 * s_admit_done_us is its completion stamp. */
static void kvb_admit_fit_check(void)
{
    uint64_t miss, sum;
    if (!s_admit_fit_armed)
        return;
    s_admit_fit_armed = 0u;
    if (!s_present_position || s_admit_fit_position != s_present_position)
        return;
    ++s_admit_fit[0];
    if (s_admit_done_us <= s_admit_fit_latest_us) {
        ++s_admit_fit[1];
        return;
    }
    miss = s_admit_done_us - s_admit_fit_latest_us;
    sum = (uint64_t)s_admit_fit[2] + miss;     /* saturating, as the scheduler's add_u32 */
    s_admit_fit[2] = sum > UINT32_MAX ? UINT32_MAX : (uint32_t)sum;
    if (miss > s_admit_fit[3])
        s_admit_fit[3] = miss > UINT32_MAX ? UINT32_MAX : (uint32_t)miss;
}
#endif
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND
# if !defined(ISAAC_VITA_GL_THREAD_RENDER_ADMISSION) || !ISAAC_VITA_GL_THREAD_RENDER_ADMISSION
#  error "GL admit behind needs GL render admission"
# endif
# if defined(ISAAC_VITA_GL_THREAD_ADMIT_WAIT) && ISAAC_VITA_GL_THREAD_ADMIT_WAIT
#  error "GL admit behind and GL admit wait each own the behind arm of the gate: build one"
# endif
/* GL admit behind (R60-FABLE-PLAN.md lane B).  Game thread only, except
 * s_ab_done_us: the present closure writes it on the render thread before
 * its record's done_pos is published and the game thread reads it only
 * after worker_wait saw that done_pos (the admit wait's s_admit_done_us
 * contract, above). */
static uint64_t s_ab_done_us;
static uint32_t s_ab_check_armed, s_ab_check_position, s_ab_check_would;
static uint64_t s_ab_check_present_us, s_ab_check_return_us, s_ab_check_head_us;
/* pm(c,ok,us,max) 0..3, wp(c,ok,us,max) 4..7, phl 8, hd(n,us) 9..10. */
static uint32_t s_ab[KAGE_VITA_BACKEND_ADMIT_BEHIND_WORDS];

static void kvb_ab_add(uint32_t *counter, uint64_t value)
{
    uint64_t sum = (uint64_t)*counter + value; /* saturating, as the scheduler's add_u32 */

    *counter = value > UINT32_MAX || sum > UINT32_MAX ? UINT32_MAX : (uint32_t)sum;
}

# if ISAAC_VITA_GL_THREAD_ADMIT_BEHIND == 2
/* The scheduler arms s_ab_admit_arm when it renders a frame while behind.
 * That frame's own Present waits for the closure it was behind; the Present
 * after it waits for that frame's closure (s_ab_chain_position).  Either
 * wait is a hold (us, at least 1) left in s_ab_hold_us for the scheduler's
 * next loop head. */
static uint32_t s_ab_admit_arm, s_ab_chain_position, s_ab_hold_us;
static uint32_t s_ab_hold_armed;
static uint64_t s_ab_hold_at;
# endif

int kage_vita_backend_admit_behind_gate(uint32_t *outstanding)
{
# if ISAAC_VITA_GL_THREAD_ADMIT_BEHIND == 2
    /* An arm still set here belongs to an admitted Render that never
     * reached Present (the gate runs once per loop, before the admission
     * that sets it): drop it, so a later, unrelated Present is not taken
     * for that frame's. */
    s_ab_admit_arm = 0u;
# endif
    if (!isaac_gl_record_worker_behind(s_present_position))
        return 0;
    *outstanding = isaac_gl_record_worker_outstanding();
    return 1;
}

void kage_vita_backend_admit_behind_refused(
    uint64_t present_us, uint64_t return_us, uint64_t head_due_us,
    uint32_t would_admit)
{
    isaac_gl_record_worker_note_render_skip();
    /* Two refusals against one closure (a skipped full Render between
     * them): the later one is the one checked. */
    s_ab_check_armed = 1u;
    s_ab_check_position = s_present_position;
    s_ab_check_present_us = present_us;
    s_ab_check_return_us = return_us;
    s_ab_check_head_us = head_due_us;
    s_ab_check_would = would_admit;
}

/* Present, after worker_wait(s_present_position): that closure has run, so
 * s_ab_done_us is its completion stamp.  Had the refused frame rendered, its
 * Present (predicted at gate + last Render body) would have held until then:
 * pm counts the refusals checked, those whose closure was done by that
 * Present (ok), and the hold of the others (us, max); wp the same over the
 * frames ON would have rendered; phl those of pm whose hold would have
 * carried the Render return to or past the next loop head's due time, i.e.
 * the hold + late head that arms the ON back-off. */
static void kvb_admit_behind_check(void)
{
    uint64_t miss;

    if (!s_ab_check_armed)
        return;
    s_ab_check_armed = 0u;
    if (!s_present_position || s_ab_check_position != s_present_position)
        return;
    miss = s_ab_done_us > s_ab_check_present_us ?
        s_ab_done_us - s_ab_check_present_us : 0u;
    kvb_ab_add(&s_ab[0], 1u);
    if (s_ab_check_would)
        kvb_ab_add(&s_ab[4], 1u);
    if (!miss) {
        kvb_ab_add(&s_ab[1], 1u);
        if (s_ab_check_would)
            kvb_ab_add(&s_ab[5], 1u);
        return;
    }
    kvb_ab_add(&s_ab[2], miss);
    if (miss > s_ab[3])
        s_ab[3] = miss > UINT32_MAX ? UINT32_MAX : (uint32_t)miss;
    if (s_ab_check_would) {
        kvb_ab_add(&s_ab[6], miss);
        if (miss > s_ab[7])
            s_ab[7] = miss > UINT32_MAX ? UINT32_MAX : (uint32_t)miss;
    }
    if (s_ab_check_return_us <= UINT64_MAX - miss &&
            s_ab_check_return_us + miss >= s_ab_check_head_us)
        kvb_ab_add(&s_ab[8], 1u);
}

# if ISAAC_VITA_GL_THREAD_ADMIT_BEHIND == 2
void kage_vita_backend_admit_behind_admitted(void)
{
    s_ab_admit_arm = 1u;
}

uint32_t kage_vita_backend_admit_behind_hold_take(void)
{
    uint32_t hold_us = s_ab_hold_us;

    s_ab_hold_us = 0u;
    return hold_us;
}

/* Around the stock worker_wait, which stays one unguarded line. */
static void kvb_admit_behind_hold_begin(void)
{
    s_ab_hold_armed =
        (s_ab_admit_arm ||
         (s_ab_chain_position != 0u &&
          s_ab_chain_position == s_present_position)) &&
        isaac_gl_record_worker_behind(s_present_position);
    s_ab_hold_at = s_ab_hold_armed ? sceKernelGetProcessTimeWide() : 0u;
    s_ab_chain_position = 0u;
}

static void kvb_admit_behind_hold_end(void)
{
    uint64_t now, hold;

    if (!s_ab_hold_armed)
        return;
    s_ab_hold_armed = 0u;
    now = sceKernelGetProcessTimeWide();
    hold = now > s_ab_hold_at ? now - s_ab_hold_at : 0u;
    if (!hold)
        hold = 1u;
    kvb_ab_add(&s_ab_hold_us, hold);
    kvb_ab_add(&s_ab[9], 1u);
    kvb_ab_add(&s_ab[10], hold);
}
# endif

void kage_vita_backend_gl_admit_behind_take(
    uint32_t out[KAGE_VITA_BACKEND_ADMIT_BEHIND_WORDS])
{
    if (out)
        memcpy(out, s_ab, sizeof s_ab);
    memset(s_ab, 0, sizeof s_ab);
}
#endif

static void kvb_present_closure(void *unused)
{
    uint64_t swap_started_at;
    (void)unused;
    gl_vita_backend_attrib_sync();
#if defined(ISAAC_VITA_VITAGL_SHADER_CACHE)
    gl_vita_backend_shader_cache_report();
#endif
#if defined(ISAAC_VITA_FBO_DEFERRED_ATTACH)
    gl_vita_backend_fbo_deferred_attach_present();
#endif
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION)
    gl_vita_backend_fbo_present();
#endif
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    gl_vita_backend_fill_census_present();
#endif
#if defined(ISAAC_VITA_DEBUG_OVERLAY)
    isaac_vita_debug_overlay_present_render(); /* the display callback */
#endif
    swap_started_at = sceKernelGetProcessTimeWide();
    vglSwapBuffers(GL_FALSE);
    isaac_gl_record_worker_frame_end(
        (uint32_t)(sceKernelGetProcessTimeWide() - swap_started_at));
    kvb_publish_vgl_mem();
#if defined(ISAAC_VITA_VITAGL_SCENE_GUARD)
    kage_vita_scene_guard_receipt();
#endif
#if defined(ISAAC_VITA_GL_OWNER_CHECK) && ISAAC_VITA_GL_OWNER_CHECK
    isaac_gl_owner_report();
#endif
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_WAIT) && ISAAC_VITA_GL_THREAD_ADMIT_WAIT
    /* ph120.aw fit check: the scheduler's clock (isaac_vita_get_process_time,
     * platform.c), published with this record's done_pos. */
    s_admit_done_us = sceKernelGetProcessTimeWide();
#endif
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND
    /* ph120.abp closure check: the scheduler's clock, published with this
     * record's done_pos. */
    s_ab_done_us = sceKernelGetProcessTimeWide();
#endif
}
#endif

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
#if defined(ISAAC_VITA_GL_THREAD_RECORD) && ISAAC_VITA_GL_THREAD_RECORD && \
    !(defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER)
    /* Worker: no drain.  The present closure is ordered behind every record
     * of the frame; draining here would make the game thread wait for the
     * whole replay and leave only the swap overlapping. */
    isaac_gl_record_drain();
#endif
#if defined(ISAAC_VITA_GL_WRAPPER_TIME)
    /* ph120.gw p bucket: this whole method (attribute sync, loading finish,
     * shader-cache/fill-census bookkeeping, the swap, the heartbeat) on the
     * gt clock; its gt body is the vglSwapBuffers bracket below. */
    uint64_t wrapper_started_at = sceKernelGetProcessTimeWide();
#endif
    KAGE_VITA_STALL_NOTE_PRESENT_ENTER(s_present_count);
#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
    /* Loading finish keeps its lock/owner on this thread; its own vitaGL
     * calls are closures.  Then one frame in flight: wait for the previous
     * present's closure before queueing this one. */
    kage_vita_loading_finish();
    KAGE_VITA_PHASE_PROFILE_PRESENT_ENTER();
#if defined(ISAAC_VITA_DEBUG_OVERLAY)
    ISAAC_VITA_DEBUG_OVERLAY_PRESENT_ENTER();
#endif
    /* GPU frame profile: game-side bracket around the post (its header: with
     * the worker busy/rbusy/gf stay exact, lag/q/swp describe this post). */
#if defined(ISAAC_VITA_GPU_FRAME_PROFILE)
    KAGE_VITA_GPU_FRAME_PROFILE_PRESENT_ENTER();
#endif
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_WAIT) && ISAAC_VITA_GL_THREAD_ADMIT_WAIT == 2
    kvb_admit_hold_begin();
#endif
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND == 2
    kvb_admit_behind_hold_begin();
#endif
    isaac_gl_record_worker_wait(s_present_position);
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND == 2
    kvb_admit_behind_hold_end();
#endif
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND
    kvb_admit_behind_check();
#endif
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_WAIT) && ISAAC_VITA_GL_THREAD_ADMIT_WAIT == 2
    kvb_admit_hold_end();
#endif
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_WAIT) && ISAAC_VITA_GL_THREAD_ADMIT_WAIT
    kvb_admit_fit_check();
#endif
    s_present_position = isaac_gl_record_worker_post(kvb_present_closure, NULL);
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND == 2
    if (s_ab_admit_arm) {
        s_ab_admit_arm = 0u;
        s_ab_chain_position = s_present_position;
    }
#endif
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_WAIT) && ISAAC_VITA_GL_THREAD_ADMIT_WAIT == 2
    if (s_admit_waited_arm) {
        s_admit_waited_arm = 0u;
        s_admit_waited_position = s_present_position;
    }
#endif
#if defined(ISAAC_VITA_GPU_FRAME_PROFILE)
    KAGE_VITA_GPU_FRAME_PROFILE_PRESENT_RETURN();
#endif
    KAGE_VITA_PHASE_PROFILE_PRESENT_RETURN();
#if defined(ISAAC_VITA_DEBUG_OVERLAY)
    ISAAC_VITA_DEBUG_OVERLAY_PRESENT_RETURN();
#endif
#if defined(ISAAC_GL_RECORD_ADMISSION_CACHE_VERIFY)
    /* Measurement builds: cumulative since the boot self-test, one line per
     * 1024 presents; the gate reads the last one (misses <= 4 x wraps + 1). */
    if ((s_present_count & 1023u) == 1023u) {
        uint32_t vs[3];
        isaac_gl_record_admission_verify_stats(vs);
        isaac_vita_log("KAGE VITA GL RECORD ADMISSION VERIFY: presents=%u wraps=%u misses=%u hits=%u",
                       (unsigned)(s_present_count + 1u), (unsigned)vs[0], (unsigned)vs[1],
                       (unsigned)vs[2]);
    }
#endif
#else
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
#if defined(ISAAC_VITA_FBO_DEFERRED_ATTACH)
    gl_vita_backend_fbo_deferred_attach_present();
#endif
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION)
    gl_vita_backend_fbo_present();
#endif
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    /* Pass ordinal reset + one-frame dump state machine; no logging here. */
    gl_vita_backend_fill_census_present();
#endif
    KAGE_VITA_PHASE_PROFILE_PRESENT_ENTER();
#if defined(ISAAC_VITA_DEBUG_OVERLAY)
    /* Debug overlay: swap bracket + display callback (one branch when off). */
    ISAAC_VITA_DEBUG_OVERLAY_PRESENT_ENTER();
#endif
#if defined(ISAAC_VITA_GPU_FRAME_PROFILE)
    KAGE_VITA_GPU_FRAME_PROFILE_PRESENT_ENTER();
#endif
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
#if defined(ISAAC_VITA_GPU_FRAME_PROFILE)
    KAGE_VITA_GPU_FRAME_PROFILE_PRESENT_RETURN();
#endif
    KAGE_VITA_PHASE_PROFILE_PRESENT_RETURN();
#if defined(ISAAC_VITA_DEBUG_OVERLAY)
    ISAAC_VITA_DEBUG_OVERLAY_PRESENT_RETURN();
#endif
#if defined(ISAAC_VITA_VITAGL_SCENE_GUARD)
    kage_vita_scene_guard_receipt();
#endif
#endif /* render thread */
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

#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
static void kvb_vsync_closure(void *unused)
{
    (void)unused;
    vglWaitVblankStart(GL_FALSE);
}
#endif

int kage_vita_backend_set_vsync(int enabled)
{
    int requested;

    if (!kage_vita_backend_ready()) {
        kage_vita_set_error("Vita KAGE vsync requested before initialize");
        return 0;
    }
    requested = enabled != 0;
#if defined(ISAAC_VITA_GL_THREAD_RECORD) && ISAAC_VITA_GL_THREAD_RECORD
    isaac_gl_record_drain();
#endif
    /* The frozen guest's own monotonic 1/60 limiter is the only pacing
     * authority on Vita.  Enabling vitaGL swap wait also makes the guest skip
     * that limiter, coupling simulation speed directly to emulator Presents. */
#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
    isaac_gl_record_worker_call(kvb_vsync_closure, NULL);
#else
    vglWaitVblankStart(GL_FALSE);
#endif
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
