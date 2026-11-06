/* On-screen debug overlay (ISAAC_VITA_DEBUG_OVERLAY, debug builds only).
 *
 * What the player sees on the Vita screen, top-right corner, drawn by the CPU
 * into the final A8B8G8R8 scanout buffer from vitaGL's display callback (the
 * kage_vita_loading.c / kage_vita_continue_profile.c precedent): the picture
 * is touched after GXM finished it, so no guest GL state, FBO, render scale
 * or shader is involved.
 *
 *   compact: FPS (presents in the last second), frame ms p50/max, free user
 *            RAM / free CDRAM, the overlay's own cost.
 *   full:    frame breakdown (Game::Update / Render / vglSwapBuffers p50/max
 *            from the fullspeed scheduler's own clocks and the present
 *            bracket), per-core load of cores 0-2, the busiest native
 *            threads, and one row per RAM consumer: used / reserved / peak,
 *            plus FREE, UNACCOUNTED and BASE rows that reconcile per domain
 *            (user RAM, CDRAM, phycont).
 *
 * Where the RAM numbers come from: every memblock the eboot allocates goes
 * through sceKernelAllocMemBlock / kuKernelAllocMemBlock (newlib's heap in
 * crt0, vitaGL's pools, every isaac_* reservation); CMake links the eboot
 * with --wrap for those two and sceKernelFreeMemBlock, so this TU keeps a
 * ledger of live memblocks by name, type and size without touching any
 * owner.  Thread stacks come the same way from sceKernelCreateThread /
 * sceKernelDeleteThread / sceKernelExitDeleteThread.  BASE is the free size
 * the kernel reported at the first wrapped call (crt0, before newlib's heap
 * exists) plus what was already mapped then (eboot segments, main stack);
 * UNACCOUNTED = BASE - rows - FREE is whatever was allocated later outside
 * the ledger (system modules, threads created by system libraries).  "Used"
 * inside a reservation is shown only where its owner counts it.
 *
 * Mode: port.ini debug_overlay=off|compact|full, pushed once at boot by
 * host_vita_runtime_settings.c.  With mode off the per-present cost is one
 * load and one branch per hook and the display callback is never installed;
 * the wrapped allocation calls still book-keep (a few stores per memblock).
 *
 * This header declares nothing unless ISAAC_VITA_DEBUG_OVERLAY is defined;
 * CMake attaches that definition per source inside if(ISAAC_VITA_DEBUG_OVERLAY)
 * only, so every includer of an OFF build preprocesses to today's text. */
#ifndef HOST_VITA_DEBUG_OVERLAY_H
#define HOST_VITA_DEBUG_OVERLAY_H

#if defined(ISAAC_VITA_DEBUG_OVERLAY)

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Same values as enum isaac_vita_settings_debug_overlay (port.ini). */
#define ISAAC_VITA_DEBUG_OVERLAY_MODE_OFF     0u
#define ISAAC_VITA_DEBUG_OVERLAY_MODE_COMPACT 1u
#define ISAAC_VITA_DEBUG_OVERLAY_MODE_FULL    2u

/* Written once at boot by isaac_vita_debug_overlay_set_mode(); read by the
 * two present hooks as their only work while the mode is off. */
extern volatile uint8_t g_isaac_vita_debug_overlay_mode;

/* Boot-time push from the port.ini loader (out-of-range values select off)
 * and its one-line receipt "KAGE VITA DEBUG OVERLAY: mode=...". */
void isaac_vita_debug_overlay_set_mode(uint8_t mode);
/* "off" / "compact" / "full"; "off" for any other value. */
const char *isaac_vita_debug_overlay_mode_name(uint8_t mode);

/* kage_vita_backend_present(): around vglSwapBuffers, on the vitaGL owner
 * (game) thread. */
void isaac_vita_debug_overlay_present_enter(void);
void isaac_vita_debug_overlay_present_return(void);
#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
/* GL render thread only: the vitaGL half of present_enter. */
void isaac_vita_debug_overlay_present_render(void);
#endif

#define ISAAC_VITA_DEBUG_OVERLAY_PRESENT_ENTER() \
    do { \
        if (g_isaac_vita_debug_overlay_mode) \
            isaac_vita_debug_overlay_present_enter(); \
    } while (0)
#define ISAAC_VITA_DEBUG_OVERLAY_PRESENT_RETURN() \
    do { \
        if (g_isaac_vita_debug_overlay_mode) \
            isaac_vita_debug_overlay_present_return(); \
    } while (0)

/* ---- model API: the pure half, exercised by the host oracle ----------- */

/* Text page handed to the display callback: at most LINES lines of COLS
 * characters (ASCII; lowercase is drawn as uppercase). */
#define ISAAC_VITA_DEBUG_OVERLAY_COLS  40u
#define ISAAC_VITA_DEBUG_OVERLAY_LINES 31u

enum isaac_vita_debug_overlay_color {
    ISAAC_VITA_DEBUG_OVERLAY_COLOR_TEXT = 0,
    ISAAC_VITA_DEBUG_OVERLAY_COLOR_DIM,
    ISAAC_VITA_DEBUG_OVERLAY_COLOR_WARN,
    ISAAC_VITA_DEBUG_OVERLAY_COLOR_BAD,
    ISAAC_VITA_DEBUG_OVERLAY_COLOR_COUNT
};

typedef struct isaac_vita_debug_overlay_page {
    uint8_t mode;
    uint8_t line_count;
    uint8_t length[ISAAC_VITA_DEBUG_OVERLAY_LINES];
    uint8_t color[ISAAC_VITA_DEBUG_OVERLAY_LINES];
    char text[ISAAC_VITA_DEBUG_OVERLAY_LINES][ISAAC_VITA_DEBUG_OVERLAY_COLS];
} isaac_vita_debug_overlay_page;

/* Memory domains with a kernel free counter (sceKernelGetFreeMemorySize);
 * CDLG (common-dialog budget) has none and is shown, not reconciled. */
enum isaac_vita_debug_overlay_domain {
    ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_USER = 0,
    ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_CDRAM,
    ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_PHYCONT,
    ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_RECONCILED,
    ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_CDLG = ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_RECONCILED,
    ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_COUNT
};

/* Row flags. */
#define ISAAC_VITA_DEBUG_OVERLAY_ROW_USED        0x01u /* used is measured  */
#define ISAAC_VITA_DEBUG_OVERLAY_ROW_PEAK        0x02u /* peak is measured  */
#define ISAAC_VITA_DEBUG_OVERLAY_ROW_PEAK_SAMPLED 0x04u /* peak = max of 1 Hz
                                                         * samples, not the
                                                         * owner's own peak */
#define ISAAC_VITA_DEBUG_OVERLAY_ROW_VGL         0x08u /* vitaGL pool row   */

#define ISAAC_VITA_DEBUG_OVERLAY_LABEL_BYTES 16u
#define ISAAC_VITA_DEBUG_OVERLAY_MAX_ROWS    48u

typedef struct isaac_vita_debug_overlay_row {
    char label[ISAAC_VITA_DEBUG_OVERLAY_LABEL_BYTES];
    uint8_t domain;
    uint8_t flags;
    uint16_t count;  /* memblocks / threads folded into this row */
    uint32_t used;   /* bytes, meaningful with ROW_USED */
    uint32_t resv;   /* bytes taken from the domain (summed) */
    uint32_t peak;   /* bytes, meaningful with ROW_PEAK */
} isaac_vita_debug_overlay_row;

/* One sampling pass, everything the formatter needs.  Times in us. */
typedef struct isaac_vita_debug_overlay_model {
    /* frames over the last second */
    uint32_t fps_presents;      /* presents with a return inside 1 s */
    uint32_t frame_p50_us, frame_p95_us, frame_max_us;
    uint32_t update_p50_us, update_max_us;
    uint32_t render_p50_us, render_max_us;
    uint32_t swap_p50_us, swap_max_us;
    uint8_t have_frames;
    uint8_t have_scheduler;     /* update/render come from the scheduler */
    /* cpu */
    uint8_t have_cpu;           /* sceKernelGetSystemInfo worked twice */
    uint8_t cpu_load[3];        /* percent, cores 0..2 */
    uint8_t have_game_thread;
    uint8_t game_thread_load;   /* percent of one core */
    uint8_t top_thread_count;
    uint8_t top_thread_load[3];
    char top_thread_name[3][10];
    /* memory */
    uint8_t have_memory;
    uint8_t have_free;
    uint8_t have_base;
    uint8_t ledger_dropped;     /* nonzero: some memblocks were not booked */
    uint32_t row_count;
    isaac_vita_debug_overlay_row rows[ISAAC_VITA_DEBUG_OVERLAY_MAX_ROWS];
    uint32_t free_bytes[ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_RECONCILED];
    uint32_t base_bytes[ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_RECONCILED];
    /* overlay's own cost */
    uint32_t cost_frame_avg_us; /* both present hooks, per present */
    uint32_t cost_sample_us;    /* last sampling + formatting pass */
    uint32_t cost_sample_max_us;
    uint32_t cost_draw_avg_us;  /* display callback, per drawn frame */
    uint32_t cost_draw_max_us;
} isaac_vita_debug_overlay_model;

/* Sum of resv of the domain's rows. */
uint64_t isaac_vita_debug_overlay_domain_rows(
    const isaac_vita_debug_overlay_model *model, unsigned domain);
/* BASE - rows - FREE for a reconciled domain (may be negative). */
int64_t isaac_vita_debug_overlay_unaccounted(
    const isaac_vita_debug_overlay_model *model, unsigned domain);

/* Format the model into page for mode (compact/full).  Lines never exceed
 * ISAAC_VITA_DEBUG_OVERLAY_COLS; returns the line count. */
unsigned isaac_vita_debug_overlay_format(
    const isaac_vita_debug_overlay_model *model, uint8_t mode,
    isaac_vita_debug_overlay_page *page);

/* Draw page into a width x height A8B8G8R8 buffer with the given stride (in
 * pixels): stippled backing + text, top-right corner, every write clipped to
 * the buffer.  Returns the number of pixels written. */
uint32_t isaac_vita_debug_overlay_draw(
    uint32_t *framebuffer, unsigned width, unsigned height, unsigned stride,
    const isaac_vita_debug_overlay_page *page);

/* "12.3" MiB with one decimal, "512K" below 1 MiB, right-aligned in width
 * columns; returns characters written (never more than width). */
unsigned isaac_vita_debug_overlay_format_bytes(char *out, unsigned width,
                                               uint64_t bytes);

#if defined(ISAAC_VITA_DEBUG_OVERLAY_ORACLE)
/* Oracle seams: reset every static, run one sampling pass now, read the
 * model and the published page, call the display callback directly. */
void isaac_vita_debug_overlay_test_reset(void);
void isaac_vita_debug_overlay_test_sample(void);
const isaac_vita_debug_overlay_model *isaac_vita_debug_overlay_test_model(void);
const isaac_vita_debug_overlay_page *isaac_vita_debug_overlay_test_page(void);
void isaac_vita_debug_overlay_test_display(void *framebuffer);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ISAAC_VITA_DEBUG_OVERLAY */

#endif /* HOST_VITA_DEBUG_OVERLAY_H */
