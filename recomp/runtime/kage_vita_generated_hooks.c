/* Vita implementation of the optional hooks baked into generated bodies.
 *
 * The public names remain kage_pc_backend_* because changing the generator
 * interface would invalidate all 171 expensive translated units.  The
 * display/clock/present subset and cached Vita controls are live; PC
 * automation, clipboard and input diagnostics stay inert. */
#include "kage_pc_backend.h"

#include <string.h>

#include <psp2/kernel/clib.h>
/* The stage heartbeat is a per-window record on the game thread. */
#if defined(ISAAC_VITA_LOG_ASYNC)
# include "host_vita_log_async.h"
#else
# define ISAAC_VITA_LOG_PRINTF sceClibPrintf
#endif

#include "kage_vita_backend.h"
#include "kage_vita_fullspeed_scheduler.h"
#include "kage_vita_stable30.h"
#include "host_vita_audio_cooperative.h"
#include "kage_vita_input.h"
#include "kage_vita_loading.h"
#include "kage_vita_phase_profile.h"
#include "kage_vita_stall_probe.h"
#if defined(ISAAC_VITA_RAW_GXM_PROBE)
# include "kage_vita_raw_gxm_probe.h"
#endif

#if defined(ISAAC_VITA_FIRST_FRAME_PROBE)
# include "guest.h"
# include "gl_vita_backend.h"

# ifndef ISAAC_VITA_FIRST_FRAME_BUILD_ID
#  define ISAAC_VITA_FIRST_FRAME_BUILD_ID "first-frame:unstamped"
# endif

/* Frozen-PE pins observed at the Render boundary.  DIRECT_DEFAULT changes
 * only the four verified branches in the derived Render TU; these hooks are
 * passive and never write guest state in that mode. */
# define KAGE_VITA_GAME_POINTER_SLOT \
    (GUEST_IMAGE_BASE + 0x007fd680u)
# define KAGE_VITA_POSTPROCESS_FLAG_OFFSET 0x00029e74u
# define KAGE_VITA_RENDER_TARGET_DEPTH_SLOT \
    (GUEST_IMAGE_BASE + 0x007fec80u)
# define KAGE_VITA_RENDER_TARGET_CURRENT_SLOT \
    (GUEST_IMAGE_BASE + 0x007c7a48u)
# if !defined(ISAAC_VITA_DIRECT_DEFAULT)
# define KAGE_VITA_POSTPROCESS_BYPASS_BEGIN \
    ISAAC_VITA_FIRST_FRAME_BLIT_END_PRESENT
# define KAGE_VITA_POSTPROCESS_BYPASS_END \
    ISAAC_VITA_FIRST_FRAME_BYPASS_END_PRESENT
# endif

_Static_assert(GUEST_IMAGE_BASE == 0x98000000u,
               "first-frame probe is pinned to the relocated Vita image");
_Static_assert(KAGE_VITA_GAME_POINTER_SLOT == 0x987fd680u,
               "first-frame game-pointer pin drifted");
# if !defined(ISAAC_VITA_DIRECT_DEFAULT)
_Static_assert(KAGE_VITA_POSTPROCESS_BYPASS_BEGIN == 240u &&
               KAGE_VITA_POSTPROCESS_BYPASS_END == 360u &&
               KAGE_VITA_POSTPROCESS_BYPASS_END -
                   KAGE_VITA_POSTPROCESS_BYPASS_BEGIN == 120u,
               "postprocess bypass must be the exact [240,360) window");
# endif

void isaac_vita_log(const char *format, ...);

# if defined(ISAAC_VITA_DIRECT_DEFAULT)
static unsigned s_direct_default_observation_logged;

static void kage_vita_direct_default_observe(void)
{
    uint32_t game;
    uint32_t flag = UINT32_MAX;

    if (s_direct_default_observation_logged)
        return;
    game = ld32(KAGE_VITA_GAME_POINTER_SLOT);
    if (game && game <= UINT32_MAX - KAGE_VITA_POSTPROCESS_FLAG_OFFSET)
        flag = ld8(game + KAGE_VITA_POSTPROCESS_FLAG_OFFSET);
    s_direct_default_observation_logged = 1u;
    isaac_vita_log(
        "KAGE VITA FIRST FRAME: bid40=%.40s "
        "phase=true-direct-default-enter present=%u game=0x%08x "
        "flag=%u rt_depth=%u rt_current=0x%08x",
        ISAAC_VITA_FIRST_FRAME_BUILD_ID,
        (unsigned)kage_vita_backend_present_count(), (unsigned)game,
        (unsigned)flag,
        (unsigned)ld32(KAGE_VITA_RENDER_TARGET_DEPTH_SLOT),
        (unsigned)ld32(KAGE_VITA_RENDER_TARGET_CURRENT_SLOT));
}

void kage_vita_first_frame_note_present_return(uint32_t present_count)
{
    (void)present_count;
}
# else
static uint32_t s_postprocess_bypass_game;
static uint8_t s_postprocess_bypass_prior;
static unsigned s_postprocess_bypass_active;
static unsigned s_postprocess_bypass_result_logged;
static unsigned s_postprocess_bypass_started;
static unsigned s_postprocess_bypass_complete_logged;
static unsigned s_postprocess_restored_logged;
static uint32_t s_postprocess_bypass_applied;
static IsaacVitaFirstFrameSnapshot s_postprocess_bypass_begin_snapshot;
static IsaacVitaFirstFrameSnapshot s_postprocess_bypass_end_snapshot;

static uint32_t kage_vita_snapshot_delta(uint32_t end, uint32_t begin)
{
    return end >= begin ? end - begin : UINT32_MAX;
}

static void kage_vita_postprocess_log_counters(
    const char *phase, uint32_t present,
    const IsaacVitaFirstFrameSnapshot *begin,
    const IsaacVitaFirstFrameSnapshot *end)
{
    isaac_vita_log(
        "KAGE VITA FIRST FRAME: bid40=%.40s phase=%s present=%u "
        "fbo=%u attach_arg=%u att_calls=%u "
        "d_bind=%u/%u d_clear=%u/%u "
        "d_draw=%u/%u last=%u applied=%u",
        ISAAC_VITA_FIRST_FRAME_BUILD_ID, phase, (unsigned)present,
        (unsigned)end->current_guest_fbo,
        (unsigned)end->manager_color_attach_arg,
        (unsigned)end->manager_color_attach_calls,
        (unsigned)kage_vita_snapshot_delta(
            end->bind_zero, begin->bind_zero),
        (unsigned)kage_vita_snapshot_delta(
            end->bind_nonzero, begin->bind_nonzero),
        (unsigned)kage_vita_snapshot_delta(
            end->clear_default, begin->clear_default),
        (unsigned)kage_vita_snapshot_delta(
            end->clear_offscreen, begin->clear_offscreen),
        (unsigned)kage_vita_snapshot_delta(
            end->draw_default, begin->draw_default),
        (unsigned)kage_vita_snapshot_delta(
            end->draw_offscreen, begin->draw_offscreen),
        (unsigned)end->last_draw_fbo,
        (unsigned)s_postprocess_bypass_applied);
}

static void kage_vita_postprocess_bypass_enter(void)
{
    uint32_t present = kage_vita_backend_present_count();
    uint32_t game;
    uint8_t prior;

    if (!isaac_vita_first_frame_postprocess_bypass_active(present))
        return;
    if (!s_postprocess_bypass_started) {
        gl_vita_backend_first_frame_snapshot(
            &s_postprocess_bypass_begin_snapshot);
        s_postprocess_bypass_started = 1u;
    }
    if (s_postprocess_bypass_active) {
        if (!(s_postprocess_bypass_result_logged & 1u)) {
            isaac_vita_log(
                "KAGE VITA FIRST FRAME: bid40=%.40s "
                "phase=postprocess-bypass "
                "result=nested-override",
                ISAAC_VITA_FIRST_FRAME_BUILD_ID);
            s_postprocess_bypass_result_logged |= 1u;
        }
        return;
    }

    game = ld32(KAGE_VITA_GAME_POINTER_SLOT);
    if (!game || game > UINT32_MAX - KAGE_VITA_POSTPROCESS_FLAG_OFFSET) {
        if (!(s_postprocess_bypass_result_logged & 2u)) {
            isaac_vita_log(
                "KAGE VITA FIRST FRAME: bid40=%.40s "
                "phase=postprocess-bypass "
                "result=invalid-game game=0x%08x",
                ISAAC_VITA_FIRST_FRAME_BUILD_ID, (unsigned)game);
            s_postprocess_bypass_result_logged |= 2u;
        }
        return;
    }

    prior = ld8(game + KAGE_VITA_POSTPROCESS_FLAG_OFFSET);
    s_postprocess_bypass_game = game;
    s_postprocess_bypass_prior = prior;
    s_postprocess_bypass_active = 1u;
    st8(game + KAGE_VITA_POSTPROCESS_FLAG_OFFSET, 0u);
    if (s_postprocess_bypass_applied != UINT32_MAX)
        ++s_postprocess_bypass_applied;
    if (!(s_postprocess_bypass_result_logged & 4u)) {
        isaac_vita_log(
            "KAGE VITA FIRST FRAME: bid40=%.40s "
            "phase=postprocess-bypass result=armed "
            "range=[%u,%u) present=%u game=0x%08x prior=%u",
            ISAAC_VITA_FIRST_FRAME_BUILD_ID,
            (unsigned)KAGE_VITA_POSTPROCESS_BYPASS_BEGIN,
            (unsigned)KAGE_VITA_POSTPROCESS_BYPASS_END,
            (unsigned)present, (unsigned)game,
            (unsigned)prior);
        s_postprocess_bypass_result_logged |= 4u;
    }
}

static void kage_vita_postprocess_bypass_leave(void)
{
    if (!s_postprocess_bypass_active)
        return;
    st8(s_postprocess_bypass_game + KAGE_VITA_POSTPROCESS_FLAG_OFFSET,
        s_postprocess_bypass_prior);
    s_postprocess_bypass_game = 0u;
    s_postprocess_bypass_prior = 0u;
    s_postprocess_bypass_active = 0u;
}

/* Called only after vglSwapBuffers returned and the successful-present count
 * advanced.  This makes the phase boundary exact even though Render entry is
 * the place where the frozen guest byte can be overridden. */
void kage_vita_first_frame_note_present_return(uint32_t present_count)
{
    IsaacVitaFirstFrameSnapshot snapshot;

    if (isaac_vita_first_frame_postprocess_bypass_complete(present_count) &&
            s_postprocess_bypass_started &&
            !s_postprocess_bypass_complete_logged) {
        gl_vita_backend_first_frame_snapshot(&snapshot);
        s_postprocess_bypass_end_snapshot = snapshot;
        kage_vita_postprocess_log_counters(
            "postprocess-bypass-complete", present_count,
            &s_postprocess_bypass_begin_snapshot, &snapshot);
        s_postprocess_bypass_complete_logged = 1u;
    } else if (isaac_vita_first_frame_postprocess_restored(present_count) &&
            s_postprocess_bypass_complete_logged &&
            !s_postprocess_restored_logged) {
        gl_vita_backend_first_frame_snapshot(&snapshot);
        kage_vita_postprocess_log_counters(
            "postprocess-restored", present_count,
            &s_postprocess_bypass_end_snapshot, &snapshot);
        s_postprocess_restored_logged = 1u;
    }
}
# endif
#endif

#if defined(ISAAC_VITA_PHASE_PROFILE) || \
    defined(ISAAC_VITA_STAGE_HEARTBEAT)
static unsigned s_loop_count;
#endif
#if defined(ISAAC_VITA_STAGE_HEARTBEAT)
static unsigned s_update_count;
static unsigned s_render_entry_count;
static unsigned s_render_return_count;

static int kage_vita_log_stage_heartbeat(unsigned count)
{
    return count != 0u &&
        ((count <= 64u && (count & (count - 1u)) == 0u) ||
         count % 120u == 0u);
}
#endif

static void kage_vita_reset_stage_heartbeat(void)
{
#if defined(ISAAC_VITA_PHASE_PROFILE) || \
    defined(ISAAC_VITA_STAGE_HEARTBEAT)
    s_loop_count = 0u;
#endif
#if defined(ISAAC_VITA_STAGE_HEARTBEAT)
    s_update_count = 0u;
    s_render_entry_count = 0u;
    s_render_return_count = 0u;
#endif
    KAGE_VITA_PHASE_PROFILE_RESET();
#if defined(ISAAC_VITA_FIRST_FRAME_PROBE)
# if defined(ISAAC_VITA_DIRECT_DEFAULT)
    s_direct_default_observation_logged = 0u;
# else
    s_postprocess_bypass_game = 0u;
    s_postprocess_bypass_prior = 0u;
    s_postprocess_bypass_active = 0u;
    s_postprocess_bypass_result_logged = 0u;
    s_postprocess_bypass_started = 0u;
    s_postprocess_bypass_complete_logged = 0u;
    s_postprocess_restored_logged = 0u;
    s_postprocess_bypass_applied = 0u;
    memset(&s_postprocess_bypass_begin_snapshot, 0,
           sizeof s_postprocess_bypass_begin_snapshot);
    memset(&s_postprocess_bypass_end_snapshot, 0,
           sizeof s_postprocess_bypass_end_snapshot);
# endif
#endif
}

int kage_pc_backend_set_mode(int mode)
{
    return mode == KAGE_PC_BACKEND_DISABLED ||
           mode == KAGE_PC_BACKEND_HIDDEN ||
           mode == KAGE_PC_BACKEND_VISIBLE;
}

int kage_pc_backend_mode(void)
{
    return kage_vita_backend_ready() ? KAGE_PC_BACKEND_HIDDEN
                                     : KAGE_PC_BACKEND_DISABLED;
}

int kage_pc_backend_initialize(uint32_t width, uint32_t height)
{
    int result = kage_vita_backend_initialize(width, height);

    if (result)
        kage_vita_reset_stage_heartbeat();
    return result;
}

void kage_pc_backend_shutdown(void)
{
    kage_vita_backend_deactivate();
    kage_vita_reset_stage_heartbeat();
}

int kage_pc_backend_present(void)
{
    int result;

    KAGE_VITA_FULLSPEED_NOTE_PRESENT();
    KAGE_VITA_STABLE30_BEFORE_PRESENT();
    result = kage_vita_backend_present();
    KAGE_VITA_STABLE30_AFTER_PRESENT(result);
    return result;
}

void kage_pc_backend_note_loop_head(void)
{
    /* The first loop head is the exact handoff from archive startup to the
    * game loop.  Drain the CPU display overlay before any Update/Render GL
     * work; then sample input exactly once for this measured tick. */
#if defined(ISAAC_VITA_PHASE_PROFILE) || \
    defined(ISAAC_VITA_STAGE_HEARTBEAT)
    ++s_loop_count;
#endif
#if defined(ISAAC_VITA_PHASE_PROFILE)
    KAGE_VITA_PHASE_PROFILE_LOOP_HEAD(s_loop_count);
#endif
    /* The profiler closes the preceding loop before the scheduler plans this
     * one, so its 120-loop scheduler deltas align with the timed window. */
    KAGE_VITA_FULLSPEED_LOOP_HEAD();
    KAGE_VITA_STABLE30_LOOP_HEAD();
    KAGE_VITA_STALL_NOTE_LOOP();
    kage_vita_loading_finish();
#if defined(ISAAC_VITA_RAW_GXM_PROBE)
    /* Arm only after the CPU loading callback and its queued work are gone.
     * The bracket stays live through vglSwapBuffers so EndScene and the exact
     * display-queue return belong to the same guest present. */
    kage_vita_raw_gxm_probe_begin_frame(
        kage_vita_backend_present_count());
#endif
    (void)kage_vita_input_sample();
}
void kage_pc_backend_note_service_entry(void)
{
    KAGE_VITA_FULLSPEED_NOTE_SERVICE();
    KAGE_VITA_PHASE_PROFILE_SERVICE_ENTRY();
}
void kage_pc_backend_note_update_entry(void)
{
#if defined(ISAAC_VITA_STAGE_HEARTBEAT)
    ++s_update_count;
#endif
    KAGE_VITA_FULLSPEED_NOTE_MANAGER_DISPATCH();
    KAGE_VITA_PHASE_PROFILE_UPDATE_ENTRY();
    KAGE_VITA_STALL_NOTE_UPDATE();
}
void kage_pc_backend_audio_cooperative_poll(CPU *cpu)
{
    isaac_vita_audio_cooperative_poll(cpu);
}
void kage_pc_backend_note_render_entry(void)
{
#if defined(ISAAC_VITA_STAGE_HEARTBEAT)
    ++s_render_entry_count;
#endif
    KAGE_VITA_FULLSPEED_NOTE_RENDER_ENTRY();
    KAGE_VITA_PHASE_PROFILE_RENDER_ENTRY();
    KAGE_VITA_STALL_NOTE_RENDER();
#if defined(ISAAC_VITA_FIRST_FRAME_PROBE)
# if defined(ISAAC_VITA_DIRECT_DEFAULT)
    kage_vita_direct_default_observe();
# else
    kage_vita_postprocess_bypass_enter();
# endif
#endif
}
void kage_pc_backend_note_render_return(void)
{
#if defined(ISAAC_VITA_FIRST_FRAME_PROBE) && \
    !defined(ISAAC_VITA_DIRECT_DEFAULT)
    kage_vita_postprocess_bypass_leave();
#endif
#if defined(ISAAC_VITA_STAGE_HEARTBEAT)
    ++s_render_return_count;
#endif
    KAGE_VITA_FULLSPEED_NOTE_RENDER_RETURN();
    KAGE_VITA_PHASE_PROFILE_RENDER_RETURN();
    KAGE_VITA_STALL_NOTE_RENDER_RETURN();
#if defined(ISAAC_VITA_STAGE_HEARTBEAT)
    if (kage_vita_log_stage_heartbeat(s_render_return_count)) {
        ISAAC_VITA_LOG_PRINTF(
            "[kage-vita] stage heartbeat loop=%u update=%u "
            "render=%u return=%u present=%u\n",
            s_loop_count, s_update_count, s_render_entry_count,
            s_render_return_count, kage_vita_backend_present_count());
    }
#endif
}

#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
void kage_pc_backend_fullspeed_note_manager_entry(
    uint32_t manager_pointer, uint32_t manager_counter,
    uint32_t interpolation_enabled, uint32_t game_pointer,
    uint32_t game_frame)
{
    kage_vita_fullspeed_scheduler_note_manager_entry(
        manager_pointer, manager_counter, interpolation_enabled,
        game_pointer, game_frame);
}

void kage_pc_backend_fullspeed_note_manager_counter_rebase(
    uint32_t manager_pointer, uint32_t old_manager_counter,
    uint32_t new_manager_counter)
{
    kage_vita_fullspeed_scheduler_note_manager_counter_rebase(
        manager_pointer, old_manager_counter, new_manager_counter);
}

void kage_pc_backend_fullspeed_note_game_pointer_publish(
    uint32_t manager_pointer, uint32_t manager_counter,
    uint32_t old_game_pointer, uint32_t new_game_pointer)
{
    kage_vita_fullspeed_scheduler_note_game_pointer_publish(
        manager_pointer, manager_counter,
        old_game_pointer, new_game_pointer);
}

void kage_pc_backend_fullspeed_note_game_update_begin(
    uint32_t manager_counter, uint32_t game_pointer, uint32_t game_frame)
{
    kage_vita_fullspeed_scheduler_note_game_update_begin(
        manager_counter, game_pointer, game_frame);
}

void kage_pc_backend_fullspeed_note_game_update_early_return(
    uint32_t return_site)
{
    kage_vita_fullspeed_scheduler_note_game_update_early_return(return_site);
}

void kage_pc_backend_fullspeed_note_game_update_end(
    uint32_t game_pointer, uint32_t game_frame)
{
    kage_vita_fullspeed_scheduler_note_game_update_end(
        game_pointer, game_frame);
}

int kage_pc_backend_fullspeed_plan_render(
    uint32_t manager_pointer, uint32_t manager_counter,
    uint32_t interpolation_enabled, uint32_t game_pointer,
    uint32_t game_frame)
{
    return kage_vita_fullspeed_scheduler_plan_render(
        manager_pointer, manager_counter, interpolation_enabled,
        game_pointer, game_frame);
}

void kage_pc_backend_fullspeed_note_render_body(
    uint32_t manager_pointer, uint32_t manager_counter,
    uint32_t interpolation_enabled)
{
    kage_vita_fullspeed_scheduler_note_render_body(
        manager_pointer, manager_counter, interpolation_enabled);
}

void kage_pc_backend_note_render_skipped(void)
{
    /* Render entry normally closes the Update timing sample.  A deliberately
     * decimated frame has no Render/Present sample and must close it here. */
    KAGE_VITA_PHASE_PROFILE_RENDER_SKIPPED();
}

int kage_pc_backend_fullspeed_bypass_limiter(void)
{
    return kage_vita_fullspeed_scheduler_bypass_limiter();
}
#endif

#if defined(ISAAC_VITA_STABLE_30_PRESENTATION)
int kage_pc_backend_stable30_manager_entry(uint32_t manager_counter)
{
    return kage_vita_stable30_manager_entry(manager_counter);
}

int kage_pc_backend_stable30_plan_render(uint32_t manager_counter)
{
    return kage_vita_stable30_plan_render(manager_counter);
}

int kage_pc_backend_stable30_bypass_limiter(void)
{
    return kage_vita_stable30_finish_tick();
}
#endif

void kage_pc_backend_note_limiter_entry(void)
{
    KAGE_VITA_PHASE_PROFILE_LIMITER_ENTRY();
}
void kage_pc_backend_note_limiter_exit(void)
{
    KAGE_VITA_PHASE_PROFILE_LIMITER_EXIT();
}
void kage_pc_backend_note_menu_init(uint32_t site)
{
    (void)site;
    KAGE_VITA_STALL_NOTE_GUEST_SITE(site);
}
void kage_pc_backend_note_menu_render(uint32_t site)
{
    (void)site;
    KAGE_VITA_STALL_NOTE_GUEST_SITE(site);
}
void kage_pc_backend_note_menu_character(uint32_t site)
{
    (void)site;
    KAGE_VITA_STALL_NOTE_GUEST_SITE(site);
}

void kage_pc_backend_note_menu_character_wheel_snapshot(
    uint32_t this_ptr, uint32_t character_count,
    uint32_t scratch_begin, uint32_t scratch_end,
    uint32_t scratch_capacity, uint32_t guard)
{
    (void)this_ptr;
    (void)character_count;
    (void)scratch_begin;
    (void)scratch_end;
    (void)scratch_capacity;
    (void)guard;
}

void kage_pc_backend_keyboard_snapshot(uint8_t keys[KAGE_PC_KEY_COUNT])
{
    kage_vita_input_keyboard_snapshot(keys);
}

int kage_pc_backend_console_trace_enabled(void) { return 0; }
int kage_pc_backend_note_console_command(uint32_t guest_stack)
{
    (void)guest_stack;
    return 0;
}

int kage_pc_backend_virtual_clipboard_open(uintptr_t owner, uint32_t *result)
{
    (void)owner;
    (void)result;
    return 0;
}

int kage_pc_backend_virtual_clipboard_get_data(
    uint32_t format, uintptr_t *result)
{
    (void)format;
    (void)result;
    return 0;
}

int kage_pc_backend_virtual_global_lock(
    uintptr_t handle, uintptr_t *result)
{
    (void)handle;
    (void)result;
    return 0;
}

int kage_pc_backend_virtual_global_unlock(
    uintptr_t handle, uint32_t *result)
{
    (void)handle;
    (void)result;
    return 0;
}

int kage_pc_backend_virtual_clipboard_close(uint32_t *result)
{
    (void)result;
    return 0;
}

void kage_pc_backend_note_guest_gl_call(const char *name) { (void)name; }
int kage_pc_backend_time_seconds(double *seconds)
{
    return kage_vita_backend_time_seconds(seconds);
}
int kage_pc_backend_set_vsync(int enabled)
{
    return kage_vita_backend_set_vsync(enabled);
}
int kage_pc_backend_vsync_enabled(void)
{
    return kage_vita_backend_vsync_enabled();
}
uint32_t kage_pc_backend_refresh_rate(void)
{
    return kage_vita_backend_ready() ? 60u : 0u;
}
unsigned kage_pc_backend_vsync_change_count(void)
{
    return kage_vita_backend_vsync_change_count();
}
int kage_pc_backend_ready(void) { return kage_vita_backend_ready(); }
uint32_t kage_pc_backend_width(void) { return kage_vita_backend_width(); }
uint32_t kage_pc_backend_height(void) { return kage_vita_backend_height(); }
const char *kage_pc_backend_last_error(void)
{
    return kage_vita_backend_last_error();
}
const char *kage_pc_backend_gl_version(void)
{
    return kage_vita_backend_gl_version();
}
unsigned kage_pc_backend_clear_count(void) { return 0u; }
unsigned kage_pc_backend_present_count(void)
{
    return kage_vita_backend_present_count();
}
int kage_pc_backend_accelerated(void)
{
    return kage_vita_backend_ready();
}
void kage_pc_backend_probe_rgba(uint8_t rgba[4])
{
    if (rgba)
        memset(rgba, 0, 4u);
}
uintptr_t kage_pc_backend_gl_proc(const char *name)
{
    /* Native GL pointers never cross this boundary on Vita. */
    (void)name;
    return (uintptr_t)0;
}
