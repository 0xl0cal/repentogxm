#ifndef KAGE_PC_BACKEND_H
#define KAGE_PC_BACKEND_H

#include <stdint.h>

/* Explicit only: the ordinary entry probe must remain deterministic and
 * headless.  A caller has to opt into either hidden smoke mode or a visible
 * development window before any KAGE wrapper can report success. */
enum {
    KAGE_PC_BACKEND_DISABLED = 0,
    KAGE_PC_BACKEND_HIDDEN = 1,
    KAGE_PC_BACKEND_VISIBLE = 2
};

/* Opt-in diagnostic artefacts are relative to the explicit PC KAGE working
 * directory.  REPENTOGXM_PC_CAPTURE=1 is the only value that enables them. */
#define KAGE_PC_CAPTURE_PPM       "first_game_frame.ppm"
#define KAGE_PC_CAPTURE_PPM_TEMP  "first_game_frame.ppm.tmp"

/* The original DeviceKeyboard callback and its three guest buffers use the
 * inclusive GLFW key-token domain 0..GLFW_KEY_LAST (348).  The native window
 * owns this host-only snapshot; the backend never receives a guest pointer. */
#define KAGE_PC_KEY_COUNT 349u

/* Console::RunCommand tracing is an explicit PC-only automation contract.
 * The helper reads the MSVC std::string argument from the untouched guest
 * entry stack, validates one bounded printable ASCII line, and emits one
 * flushed record. */
#define KAGE_PC_CONSOLE_COMMAND_MAX 4096u

/* Process-local automation transport.  WM_COPYDATA carries an exact 16-byte
 * little-endian header (version, target PID, transport sequence, text size)
 * followed by `text size` bytes.  The text size includes its NUL; the preceding
 * byte is LF, and all earlier bytes are printable ASCII or non-empty-line LFs.
 * This channel is gated by REPENTOGXM_PC_CONSOLE_TRACE=1 and never publishes
 * its HGLOBAL through the system clipboard. */
#define KAGE_PC_COMMAND_COPYDATA_MAGIC       0x4d584752u
#define KAGE_PC_COMMAND_COPYDATA_VERSION     1u
#define KAGE_PC_COMMAND_COPYDATA_HEADER_SIZE 16u
#define KAGE_PC_COMMAND_TEXT_MAX             4096u

struct CPU;

int         kage_pc_backend_set_mode(int mode);
int         kage_pc_backend_mode(void);
int         kage_pc_backend_initialize(uint32_t width, uint32_t height);
void        kage_pc_backend_shutdown(void);
int         kage_pc_backend_present(void);
void        kage_pc_backend_note_loop_head(void);
void        kage_pc_backend_note_service_entry(void);
void        kage_pc_backend_note_update_entry(void);
void        kage_pc_backend_audio_cooperative_poll(struct CPU *cpu);
void        kage_pc_backend_note_render_entry(void);
void        kage_pc_backend_note_render_return(void);
void        kage_pc_backend_note_limiter_entry(void);
void        kage_pc_backend_note_limiter_exit(void);
void        kage_pc_backend_note_menu_init(uint32_t site);
void        kage_pc_backend_note_menu_render(uint32_t site);
void        kage_pc_backend_note_menu_character(uint32_t site);
void        kage_pc_backend_note_menu_character_wheel_snapshot(
                uint32_t this_ptr, uint32_t character_count,
                uint32_t scratch_begin, uint32_t scratch_end,
                uint32_t scratch_capacity, uint32_t guard);
void        kage_pc_backend_keyboard_snapshot(
                uint8_t keys[KAGE_PC_KEY_COUNT]);
int         kage_pc_backend_console_trace_enabled(void);
int         kage_pc_backend_note_console_command(uint32_t guest_stack);
/* Return one when the import was handled by the private command channel and
 * write its exact Win32 result.  Return zero only when the caller must invoke
 * the native API.  OpenClipboard(NULL) is deliberately handled as FALSE while
 * console tracing is enabled but no valid command is armed, preventing a stale
 * Ctrl+V from ever falling through to the user's clipboard. */
int         kage_pc_backend_virtual_clipboard_open(
                uintptr_t owner, uint32_t *result);
int         kage_pc_backend_virtual_clipboard_get_data(
                uint32_t format, uintptr_t *result);
int         kage_pc_backend_virtual_global_lock(
                uintptr_t handle, uintptr_t *result);
int         kage_pc_backend_virtual_global_unlock(
                uintptr_t handle, uint32_t *result);
int         kage_pc_backend_virtual_clipboard_close(uint32_t *result);
/* Opt-in capture diagnostics consume and attribute native GL error flags
 * immediately after each typed guest GL call.  Ordinary runs are untouched. */
void        kage_pc_backend_note_guest_gl_call(const char *name);
/* Seconds elapsed since the successful backend initialization began.  The
 * PC implementation uses one checked QPC frequency/origin pair and rejects a
 * failed or backwards sample; non-Windows targets report unavailable. */
int         kage_pc_backend_time_seconds(double *seconds);
int         kage_pc_backend_set_vsync(int enabled);
int         kage_pc_backend_vsync_enabled(void);
uint32_t    kage_pc_backend_refresh_rate(void);
unsigned    kage_pc_backend_vsync_change_count(void);
int         kage_pc_backend_ready(void);
uint32_t    kage_pc_backend_width(void);
uint32_t    kage_pc_backend_height(void);
const char *kage_pc_backend_last_error(void);
const char *kage_pc_backend_gl_version(void);
unsigned    kage_pc_backend_clear_count(void);
unsigned    kage_pc_backend_present_count(void);
int         kage_pc_backend_accelerated(void);
void        kage_pc_backend_probe_rgba(uint8_t rgba[4]);

/* Resolve one exact OpenGL entry point for the current context.  This is an
 * integer on purpose: no untyped FARPROC crosses into the typed GL bridge.
 * Returns zero when there is no current KAGE context or the driver does not
 * expose the name. */
uintptr_t   kage_pc_backend_gl_proc(const char *name);

#endif
