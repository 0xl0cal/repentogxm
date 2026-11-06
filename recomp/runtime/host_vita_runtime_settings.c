/* Boot-time port.ini loader; see host_vita_runtime_settings.h.
 *
 * The only file the runtime ever opens for settings is
 * ISAAC_VITA_NATIVE_DATA_ROOT "/port.ini" (design section 1.1); the manager
 * is the only writer and never runs concurrently with the game.  Nothing
 * here allocates: the file lands in one static 4097-byte buffer, the parser
 * works on that buffer, and the receipt is formatted by isaac_vita_log.
 *
 * Push design: consumers own their tables and export setters;
 * this TU calls them once with the parsed values.  The first stage pushes only the
 * fullspeed scheduler duty (kage_vita_fullspeed_scheduler_set_duty), and
 * only when this TU is compiled with ISAAC_VITA_FULLSPEED_SCHEDULER (the
 * CMake block nests that definition under the scheduler option).  WP3 adds
 * isaac_vita_xinput_set_remap(), kage_vita_input_set_chords() and
 * kage_vita_touch_set_remap() to runtime_settings_push(). */
#include "host_vita_runtime_settings.h"

#if defined(ISAAC_VITA_RUNTIME_SETTINGS)

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "host_vita_settings_parse.h"
#include "platform.h"
#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
#include "kage_vita_fullspeed_scheduler.h"
#endif
#if defined(ISAAC_VITA_RENDER_SCALE_DEFAULT)
#include "host_vita_render_scale.h"
#endif
#if defined(ISAAC_VITA_DEBUG_OVERLAY)
#include "host_vita_debug_overlay.h"
#endif
#if !defined(ISAAC_VITA_RUNTIME_SETTINGS_ORACLE)
#include <psp2/io/fcntl.h>
#endif

#define RUNTIME_SETTINGS_PATH ISAAC_VITA_NATIVE_DATA_ROOT "/port.ini"
/* One byte more than the accepted cap: a full buffer is the too-large
 * signal the parser turns into STATUS_TOO_LARGE. */
#define RUNTIME_SETTINGS_BUFFER_BYTES (ISAAC_VITA_SETTINGS_MAX_BYTES + 1u)
/* SCE_ERROR_ERRNO_ENOENT: the only open failure that means "absent". */
#define RUNTIME_SETTINGS_SCE_ENOENT 0x80010002u
/* "io-error(0x%08x)": 8 + 1 + 2 + 8 + 1 + NUL. */
#define RUNTIME_SETTINGS_STATUS_TEXT_BYTES 24u
/* Build raster of this eboot (design section 1.3: the display_raster default
 * is the build raster, not the parser's 960).  The CMake runtime-settings
 * block forwards ISAAC_VITA_DISPLAY_RASTER_WIDTH to this TU only; host
 * oracles that omit it model the native 960x544 build. */
#if defined(ISAAC_VITA_DISPLAY_RASTER_WIDTH)
#define RUNTIME_SETTINGS_BUILD_RASTER ((uint16_t)ISAAC_VITA_DISPLAY_RASTER_WIDTH)
#else
#define RUNTIME_SETTINGS_BUILD_RASTER ((uint16_t)960u)
#endif

/* ISAAC_VITA_DEBUG_OVERLAY: the debug_overlay key joins the end of the first
 * receipt line; without the overlay both macros expand to nothing. */
#if defined(ISAAC_VITA_DEBUG_OVERLAY)
#define RUNTIME_SETTINGS_OVERLAY_FORMAT " debug_overlay=%s"
#define RUNTIME_SETTINGS_OVERLAY_ARG(settings) \
    , isaac_vita_debug_overlay_mode_name((settings)->debug_overlay)
#else
#define RUNTIME_SETTINGS_OVERLAY_FORMAT
#define RUNTIME_SETTINGS_OVERLAY_ARG(settings)
#endif

_Static_assert(RUNTIME_SETTINGS_BUFFER_BYTES == 4097u,
               "the reader requests exactly one byte past the 4096 cap");
_Static_assert(RUNTIME_SETTINGS_BUILD_RASTER == 960u ||
               RUNTIME_SETTINGS_BUILD_RASTER == 720u ||
               RUNTIME_SETTINGS_BUILD_RASTER == 480u,
               "the build raster is one of the CMakeLists.txt raster widths");

static char s_file[RUNTIME_SETTINGS_BUFFER_BYTES];

/* ---- I/O seams --------------------------------------------------------- */

#if defined(ISAAC_VITA_RUNTIME_SETTINGS_ORACLE)
static int runtime_settings_open(const char *path)
{
    return isaac_vita_runtime_settings_oracle_open(path);
}

static int runtime_settings_read(int descriptor, void *buffer, uint32_t size)
{
    return isaac_vita_runtime_settings_oracle_read(descriptor, buffer, size);
}

static int runtime_settings_close(int descriptor)
{
    return isaac_vita_runtime_settings_oracle_close(descriptor);
}
#else
static int runtime_settings_open(const char *path)
{
    return sceIoOpen(path, SCE_O_RDONLY, 0);
}

static int runtime_settings_read(int descriptor, void *buffer, uint32_t size)
{
    return sceIoRead(descriptor, buffer, size);
}

static int runtime_settings_close(int descriptor)
{
    return sceIoClose(descriptor);
}
#endif

/* Fills s_file[0..return).  *status is OK when the bytes are the file (a
 * count of RUNTIME_SETTINGS_BUFFER_BYTES means "more than the cap"), ABSENT
 * on ENOENT, IO_ERROR with *io_error on any other open/read failure. */
static uint32_t runtime_settings_read_file(
    enum isaac_vita_settings_status *status, uint32_t *io_error)
{
    int descriptor;
    uint32_t total = 0u;

    *status = ISAAC_VITA_SETTINGS_STATUS_OK;
    *io_error = 0u;
    descriptor = runtime_settings_open(RUNTIME_SETTINGS_PATH);
    if (descriptor < 0) {
        if ((uint32_t)descriptor == RUNTIME_SETTINGS_SCE_ENOENT) {
            *status = ISAAC_VITA_SETTINGS_STATUS_ABSENT;
        } else {
            *status = ISAAC_VITA_SETTINGS_STATUS_IO_ERROR;
            *io_error = (uint32_t)descriptor;
        }
        return 0u;
    }
    while (total < RUNTIME_SETTINGS_BUFFER_BYTES) {
        uint32_t room = RUNTIME_SETTINGS_BUFFER_BYTES - total;
        int got = runtime_settings_read(descriptor, s_file + total, room);

        if (got < 0) {
            *status = ISAAC_VITA_SETTINGS_STATUS_IO_ERROR;
            *io_error = (uint32_t)got;
            break;
        }
        if (got == 0) {
            break;
        }
        /* A count past the request cannot have been written into the
         * buffer; keep what fits rather than trusting the driver. */
        if ((uint32_t)got > room) {
            got = (int)room;
        }
        total += (uint32_t)got;
    }
    (void)runtime_settings_close(descriptor);
    return total;
}

/* ---- receipt helpers --------------------------------------------------- */

/* "absent" | "ok" | ... | "io-error(0x80010013)"; cap >= STATUS_TEXT_BYTES. */
static void runtime_settings_status_text(
    const struct isaac_vita_settings_receipt *receipt, char *out, size_t cap)
{
    static const char k_hex[] = "0123456789abcdef";
    const char *name = isaac_vita_settings_status_name(
        (enum isaac_vita_settings_status)receipt->status);
    size_t len = strlen(name);
    unsigned i;

    if (len + 1u > cap) {
        len = cap - 1u;
    }
    memcpy(out, name, len);
    if (receipt->status == (uint8_t)ISAAC_VITA_SETTINGS_STATUS_IO_ERROR &&
            len + 12u < cap) {
        out[len++] = '(';
        out[len++] = '0';
        out[len++] = 'x';
        for (i = 0u; i < 8u; ++i) {
            out[len++] = k_hex[(receipt->io_error >> (28u - 4u * i)) & 0xFu];
        }
        out[len++] = ')';
    }
    out[len] = '\0';
}

static int runtime_settings_remap_is_default(
    const struct isaac_vita_settings *settings)
{
    struct isaac_vita_settings defaults;

    isaac_vita_settings_defaults(&defaults);
    return memcmp(settings->button, defaults.button,
                  sizeof defaults.button) == 0 &&
           settings->chord_start_l == defaults.chord_start_l &&
           settings->chord_start_r == defaults.chord_start_r &&
           settings->chord_restart_hold == defaults.chord_restart_hold &&
           memcmp(settings->touch, defaults.touch,
                  sizeof defaults.touch) == 0 &&
           settings->stick_swap == defaults.stick_swap;
}

/* Scanout height of a display_raster width (CMakeLists.txt raster table:
 * 960x544, 720x408, 480x272). */
static unsigned runtime_settings_raster_height(uint16_t width)
{
    switch (width) {
    case 720u:
        return 408u;
    case 480u:
        return 272u;
    case 960u:
    default:
        return 544u;
    }
}

static const char *runtime_settings_element_text(uint8_t element)
{
    return isaac_vita_settings_element_name(
        (enum isaac_vita_settings_element)element);
}

/* ---- consumers --------------------------------------------------------- */

static void runtime_settings_push(const struct isaac_vita_settings *settings)
{
#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
    kage_vita_fullspeed_scheduler_set_duty(
        isaac_vita_settings_fps_lock_duty_k(settings->fps_lock));
#else
    /* No fullspeed scheduler in this build (includes STABLE_30): fps_lock
     * is reported with "(ignored:no-fullspeed-scheduler)" and not applied. */
    (void)settings;
#endif
#if defined(ISAAC_VITA_DEBUG_OVERLAY)
    isaac_vita_debug_overlay_set_mode(settings->debug_overlay);
#endif
    /* WP3 pushes here, in this order: isaac_vita_xinput_set_remap(),
     * kage_vita_input_set_chords(), kage_vita_touch_set_remap(). */
}

/* ---- receipt ----------------------------------------------------------- */

static void runtime_settings_log(
    const struct isaac_vita_settings *settings,
    const struct isaac_vita_settings_receipt *receipt)
{
    char status_text[RUNTIME_SETTINGS_STATUS_TEXT_BYTES];
    const char *remap_text = "custom";
    const char *guard_suffix = "";

    runtime_settings_status_text(receipt, status_text, sizeof status_text);
    if (receipt->remap_guard) {
        remap_text = "default";
        guard_suffix = "(guard)";
    } else if (runtime_settings_remap_is_default(settings)) {
        remap_text = "default";
    }
    isaac_vita_log(
        "ISAAC VITA SETTINGS: path=%s status=%s bytes=%u lines=%u "
        "rejected=%u unknown=%u dup=%u fps_lock=%u%s raster=%ux%u%s "
        "remap=%s%s chords=%s/%s restart_hold=%u stick_swap=%u"
        RUNTIME_SETTINGS_OVERLAY_FORMAT,
        RUNTIME_SETTINGS_PATH, status_text,
        (unsigned)receipt->bytes, (unsigned)receipt->lines,
        (unsigned)receipt->rejected, (unsigned)receipt->unknown,
        (unsigned)receipt->dup,
        (unsigned)settings->fps_lock,
#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
        "",
#else
        "(ignored:no-fullspeed-scheduler)",
#endif
        (unsigned)settings->display_raster,
        runtime_settings_raster_height(settings->display_raster),
        "(ignored:not-compiled)",
        remap_text, guard_suffix,
        runtime_settings_element_text(settings->chord_start_l),
        runtime_settings_element_text(settings->chord_start_r),
        (unsigned)settings->chord_restart_hold,
        (unsigned)settings->stick_swap RUNTIME_SETTINGS_OVERLAY_ARG(settings));
    if (receipt->rejected != 0u || receipt->unknown != 0u) {
        isaac_vita_log(
            "ISAAC VITA SETTINGS REJECTED: lines=%u,%u,%u,%u "
            "unknown=%.15s,%.15s,%.15s,%.15s",
            (unsigned)receipt->rejected_lines[0],
            (unsigned)receipt->rejected_lines[1],
            (unsigned)receipt->rejected_lines[2],
            (unsigned)receipt->rejected_lines[3],
            receipt->unknown_keys[0], receipt->unknown_keys[1],
            receipt->unknown_keys[2], receipt->unknown_keys[3]);
    }
}

/* ---- entry ------------------------------------------------------------- */

void isaac_vita_runtime_settings_load(void)
{
    struct isaac_vita_settings settings;
    struct isaac_vita_settings_receipt receipt;
    enum isaac_vita_settings_status status;
    uint32_t io_error;
    uint32_t bytes;

    bytes = runtime_settings_read_file(&status, &io_error);
    if (status == ISAAC_VITA_SETTINGS_STATUS_OK) {
        /* The parser resets *settings to the defaults first and leaves
         * them there unless it returns OK; a 4097-byte read is TOO_LARGE. */
        (void)isaac_vita_settings_parse(s_file, bytes, &settings, &receipt);
    } else {
        isaac_vita_settings_defaults(&settings);
        isaac_vita_settings_receipt_clear(&receipt);
        receipt.status = (uint8_t)status;
        receipt.io_error = io_error;
        receipt.bytes = bytes;
    }
    if (receipt.remap_guard) {
        /* Section 0.9: no physical button maps to a or none to b; the whole
         * remap section falls back to the defaults, fps_lock stays. */
        isaac_vita_settings_reset_remap(&settings);
    }
    if ((receipt.assigned & ISAAC_VITA_SETTINGS_ASSIGNED_BIT(
             ISAAC_VITA_SETTINGS_KEY_DISPLAY_RASTER)) == 0u) {
        /* Key absent (or the file ignored): the shared parser cannot see the
         * build raster, so the design's default is substituted here. */
        settings.display_raster = RUNTIME_SETTINGS_BUILD_RASTER;
    }
    runtime_settings_push(&settings);
    runtime_settings_log(&settings, &receipt);
#if defined(ISAAC_VITA_RENDER_SCALE_DEFAULT)
    /* Owner decision 2026-09-16: render_scale (default 1) is written into
     * the game's options.ini MaxRenderScale before the guest reads it; its
     * own receipt line follows the settings receipt. */
    isaac_vita_render_scale_apply(settings.render_scale);
#endif
}

#endif /* ISAAC_VITA_RUNTIME_SETTINGS */
