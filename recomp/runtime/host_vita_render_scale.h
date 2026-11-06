/* options.ini MaxRenderScale normalizer (ISAAC_VITA_RENDER_SCALE_DEFAULT).
 *
 * The game keeps its room render scale in options.ini (MaxRenderScale: 1 =
 * the 480x270 world raster, 99 = the stock unbounded value, 2x at the 960x540
 * window) and offers no menu for it.  Owner decision 2026-09-16: 1 is the
 * port default and a port.ini setting (render_scale=1|2) switches it.  This
 * unit is the bridge: isaac_vita_render_scale_apply() runs once from the
 * port.ini loader (host_vita_runtime_settings.c, single-threaded, before the
 * guest starts) and rewrites the one MaxRenderScale line of
 *
 *   ISAAC_VITA_NATIVE_DATA_ROOT
 *     "/Documents/My Games/Binding of Isaac Repentance/options.ini"
 *
 * to the value the setting selects (1 -> "1", 2 -> "99"), leaving every
 * other byte (line endings included) as it was.  Rules of the pure transform
 * isaac_vita_render_scale_transform(), all oracle-tested:
 *
 *   - the key is matched per line, case-insensitively, after optional
 *     blanks; the value token (after '=', blanks trimmed) is replaced, the
 *     line's own spelling, blanks and ending are kept;
 *   - value already the target: UNCHANGED, nothing is written;
 *   - key absent: a "MaxRenderScale=<target>" line is inserted at the end
 *     of the [Options] section (before the next section header or at EOF,
 *     adding the missing final newline first) using the file's first line
 *     ending (LF when the file has none): APPENDED;
 *   - key twice or more: DUPLICATE; no [Options] header: NO_SECTION; a NUL
 *     byte: BINARY; zero bytes: EMPTY; more than
 *     ISAAC_VITA_RENDER_SCALE_FILE_MAX bytes: TOO_LARGE - in every one of
 *     these the file is left alone;
 *   - no options.ini at all: ABSENT, the game writes its own defaults on
 *     the first run and the next boot normalizes them.
 *
 * Publish protocol (never renames onto an existing path, mirrors the
 * manager's port.ini writer): options.ini.rs-tmp is created O_EXCL, written,
 * synced and closed; options.ini is renamed aside to options.ini.rs-bak; the
 * temp is renamed into place; the aside copy is removed; ux0: is synced.  A
 * failed second rename moves the aside copy back.  At the start of every
 * apply a stale temp is removed and a stranded aside copy is renamed back
 * when options.ini is missing (recovered=1 in the receipt) or removed when
 * options.ini exists.  One receipt line through isaac_vita_log:
 *
 *   ISAAC VITA RENDER SCALE: mode=%u target=%s status=%s old=%s bytes=%u
 *     rc=0x%08x recovered=%u
 *
 * status is unchanged | rewritten | appended | absent | empty | too-large |
 * binary | no-section | duplicate | read-error | write-error |
 * rename-error; old is the previous value token ("-" when there was none);
 * rc is the failing SCE code (0 otherwise).  Nothing here allocates; two
 * static buffers hold the file and its rewrite.  Never fails the boot.
 *
 * Declares nothing without ISAAC_VITA_RENDER_SCALE_DEFAULT; CMake attaches
 * the definition per source (this unit and the loader) only inside
 * if(ISAAC_VITA_RENDER_SCALE_DEFAULT), nested in the runtime-settings
 * block, so an OFF build preprocesses every includer to today's text. */
#ifndef HOST_VITA_RENDER_SCALE_H
#define HOST_VITA_RENDER_SCALE_H

#if defined(ISAAC_VITA_RENDER_SCALE_DEFAULT)

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Longest options.ini the normalizer touches (the stock file is ~1 KiB). */
#define ISAAC_VITA_RENDER_SCALE_FILE_MAX 16384u
/* Rewrite capacity: the inserted line is at most ending + key + '=' +
 * two digits + ending (21 bytes); a value replacement grows by two. */
#define ISAAC_VITA_RENDER_SCALE_OUT_BYTES (ISAAC_VITA_RENDER_SCALE_FILE_MAX + 32u)
/* Longest previous value token kept for the receipt (plus the NUL). */
#define ISAAC_VITA_RENDER_SCALE_OLD_MAX 15u

enum isaac_vita_render_scale_status {
    ISAAC_VITA_RENDER_SCALE_UNCHANGED = 0,
    ISAAC_VITA_RENDER_SCALE_REWRITTEN,
    ISAAC_VITA_RENDER_SCALE_APPENDED,
    ISAAC_VITA_RENDER_SCALE_ABSENT,
    ISAAC_VITA_RENDER_SCALE_EMPTY,
    ISAAC_VITA_RENDER_SCALE_TOO_LARGE,
    ISAAC_VITA_RENDER_SCALE_BINARY,
    ISAAC_VITA_RENDER_SCALE_NO_SECTION,
    ISAAC_VITA_RENDER_SCALE_DUPLICATE,
    ISAAC_VITA_RENDER_SCALE_READ_ERROR,
    ISAAC_VITA_RENDER_SCALE_WRITE_ERROR,
    ISAAC_VITA_RENDER_SCALE_RENAME_ERROR,
    ISAAC_VITA_RENDER_SCALE_STATUS_COUNT
};

struct isaac_vita_render_scale_edit {
    uint8_t status;     /* enum isaac_vita_render_scale_status */
    uint32_t out_length; /* bytes in out for REWRITTEN / APPENDED, else 0 */
    char old_value[ISAAC_VITA_RENDER_SCALE_OLD_MAX + 1u]; /* "-" when none */
};

/* The pure transform (see the header comment).  in[0..in_length) is the
 * file; out receives the rewrite (out_capacity >= in_length + 21 or the
 * result is TOO_LARGE); target is "1" or "99".  Never reads past in_length,
 * never writes past out_capacity. */
void isaac_vita_render_scale_transform(const char *in, uint32_t in_length,
                                       const char *target, char *out,
                                       uint32_t out_capacity,
                                       struct isaac_vita_render_scale_edit *edit);

/* "1" for render_scale 1, "99" for anything else (2 is the only other
 * value the parser accepts). */
const char *isaac_vita_render_scale_target(uint8_t render_scale);

/* Receipt word of a status ("unchanged", ...); "?" out of range. */
const char *isaac_vita_render_scale_status_name(
    enum isaac_vita_render_scale_status status);

/* Recover sidecars, read, transform, publish, log.  render_scale is the
 * parsed port.ini value (1 or 2). */
void isaac_vita_render_scale_apply(uint8_t render_scale);

#if defined(ISAAC_VITA_RENDER_SCALE_ORACLE)
/* Host oracle seams (host_vita_render_scale_oracle.c defines them) in place
 * of the sceIo* calls.  SCE conventions: open returns a descriptor >= 0 or a
 * negative SCE error (0x80010002 = ENOENT); create is O_WRONLY | O_CREAT |
 * O_EXCL; read/write return the byte count or a negative error; close,
 * sync_fd, remove, rename and sync_device return 0 or a negative error. */
int isaac_vita_render_scale_oracle_open_read(const char *path);
int isaac_vita_render_scale_oracle_open_create(const char *path);
int isaac_vita_render_scale_oracle_read(int descriptor, void *buffer,
                                        uint32_t size);
int isaac_vita_render_scale_oracle_write(int descriptor, const void *buffer,
                                         uint32_t size);
int isaac_vita_render_scale_oracle_close(int descriptor);
int isaac_vita_render_scale_oracle_sync_fd(int descriptor);
int isaac_vita_render_scale_oracle_remove(const char *path);
int isaac_vita_render_scale_oracle_rename(const char *source,
                                          const char *destination);
int isaac_vita_render_scale_oracle_sync_device(const char *device);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ISAAC_VITA_RENDER_SCALE_DEFAULT */

#endif /* HOST_VITA_RENDER_SCALE_H */
