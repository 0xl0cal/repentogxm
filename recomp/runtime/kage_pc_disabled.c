/* Link-complete disabled implementation of generated PC-only diagnostics.
 *
 * The generator deliberately bakes optional KAGE instrumentation calls into
 * translated units.  Vita's first-fault target keeps those calls inert and
 * does not link WGL, USER32, input automation, or clipboard transport. */
#include "kage_pc_backend.h"

#include <string.h>

static const char s_disabled[] =
    "KAGE PC backend is unavailable in the Vita first-fault target";

int kage_pc_backend_set_mode(int mode)
{
    return mode == KAGE_PC_BACKEND_DISABLED;
}

int kage_pc_backend_mode(void)
{
    return KAGE_PC_BACKEND_DISABLED;
}

int kage_pc_backend_initialize(uint32_t width, uint32_t height)
{
    (void)width;
    (void)height;
    return 0;
}

void kage_pc_backend_shutdown(void) {}
int kage_pc_backend_present(void) { return 0; }
void kage_pc_backend_note_loop_head(void) {}
void kage_pc_backend_note_service_entry(void) {}
void kage_pc_backend_note_update_entry(void) {}
void kage_pc_backend_audio_cooperative_poll(struct CPU *cpu) { (void)cpu; }
void kage_pc_backend_note_render_entry(void) {}
void kage_pc_backend_note_render_return(void) {}
void kage_pc_backend_note_limiter_entry(void) {}
void kage_pc_backend_note_limiter_exit(void) {}
void kage_pc_backend_note_menu_init(uint32_t site) { (void)site; }
void kage_pc_backend_note_menu_render(uint32_t site) { (void)site; }
void kage_pc_backend_note_menu_character(uint32_t site) { (void)site; }

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
    memset(keys, 0, KAGE_PC_KEY_COUNT);
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
    if (seconds)
        *seconds = 0.0;
    return 0;
}

int kage_pc_backend_set_vsync(int enabled) { (void)enabled; return 0; }
int kage_pc_backend_vsync_enabled(void) { return 0; }
uint32_t kage_pc_backend_refresh_rate(void) { return 0U; }
unsigned kage_pc_backend_vsync_change_count(void) { return 0U; }
int kage_pc_backend_ready(void) { return 0; }
uint32_t kage_pc_backend_width(void) { return 0U; }
uint32_t kage_pc_backend_height(void) { return 0U; }
const char *kage_pc_backend_last_error(void) { return s_disabled; }
const char *kage_pc_backend_gl_version(void) { return s_disabled; }
unsigned kage_pc_backend_clear_count(void) { return 0U; }
unsigned kage_pc_backend_present_count(void) { return 0U; }
int kage_pc_backend_accelerated(void) { return 0; }

void kage_pc_backend_probe_rgba(uint8_t rgba[4])
{
    if (rgba)
        memset(rgba, 0, 4U);
}

uintptr_t kage_pc_backend_gl_proc(const char *name)
{
    (void)name;
    return (uintptr_t)0;
}
