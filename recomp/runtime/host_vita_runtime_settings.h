/* Boot-time port.ini loader (ISAAC_VITA_RUNTIME_SETTINGS).
 *
 * isaac_vita_runtime_settings_load() runs once inside
 * isaac_vita_run_first_fault(), right after the "guest boundary begin" line
 * and before guest_register_all_imports(): single-threaded, before module
 * discovery and before the first input sample, so every consumer table it
 * pushes is immutable when the guest starts.  It opens exactly one file,
 * ISAAC_VITA_NATIVE_DATA_ROOT "/port.ini", reads at most 4097 bytes into a
 * static buffer (the 4097th byte is the too-large signal), closes it,
 * parses with the shared TU (host_vita_settings_parse.c), resets the remap
 * section when the a/b guard fails, pushes the consumer setters and logs the
 * design section 1.5 receipt through isaac_vita_log:
 *
 *   ISAAC VITA SETTINGS: path=<path> status=<status> bytes=%u lines=%u
 *     rejected=%u unknown=%u dup=%u fps_lock=%u[<fps-suffix>]
 *     raster=%ux%u(ignored:not-compiled) remap=default|custom[(guard)]
 *     chords=<l>/<r> restart_hold=%u stick_swap=%u[ debug_overlay=<mode>]
 *   ISAAC VITA SETTINGS REJECTED: lines=%u,%u,%u,%u unknown=%s,%s,%s,%s
 *
 * " debug_overlay=off|compact|full" is appended only in a build with
 * ISAAC_VITA_DEBUG_OVERLAY (the key exists only there); the mode is pushed
 * to isaac_vita_debug_overlay_set_mode() with the other consumers.
 *
 * <status> is absent | ok | schema | garbage | too-large |
 * io-error(0x%08x); <fps-suffix> is "(ignored:no-fullspeed-scheduler)" in
 * a build without ISAAC_VITA_FULLSPEED_SCHEDULER (the setter is then not
 * compiled and fps_lock is only reported); the raster suffix stays
 * "(ignored:not-compiled)" until the runtime raster sub-knob exists; the
 * second line is emitted only when rejected + unknown > 0.  On an open or
 * read error other than ENOENT the counters are zero except bytes, which
 * holds the count read before the failure; the struct stays at the
 * defaults and the defaults are pushed.
 *
 * This header declares nothing unless ISAAC_VITA_RUNTIME_SETTINGS is
 * defined; CMake attaches that definition per source only inside
 * if(ISAAC_VITA_RUNTIME_SETTINGS), so an OFF build preprocesses every
 * includer to exactly today's text. */
#ifndef HOST_VITA_RUNTIME_SETTINGS_H
#define HOST_VITA_RUNTIME_SETTINGS_H

#if defined(ISAAC_VITA_RUNTIME_SETTINGS)

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read, parse, guard, push, log.  Never fails: every failure is a receipt
 * status and the compile-time defaults are pushed. */
void isaac_vita_runtime_settings_load(void);

#if defined(ISAAC_VITA_RUNTIME_SETTINGS_ORACLE)
/* Host oracle seams (host_vita_runtime_settings_oracle.c defines them)
 * replacing sceIoOpen / sceIoRead / sceIoClose.  Return values follow the
 * SCE convention: open returns a descriptor >= 0 or a negative SCE error
 * (0x80010002 = ENOENT = absent); read returns the byte count, 0 at EOF or a
 * negative SCE error; close returns 0 or a negative SCE error (ignored). */
int isaac_vita_runtime_settings_oracle_open(const char *path);
int isaac_vita_runtime_settings_oracle_read(int descriptor, void *buffer,
                                            uint32_t size);
int isaac_vita_runtime_settings_oracle_close(int descriptor);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ISAAC_VITA_RUNTIME_SETTINGS */

#endif /* HOST_VITA_RUNTIME_SETTINGS_H */
