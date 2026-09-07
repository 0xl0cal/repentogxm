/* Aggregate attribution for the two long save-menu actions on Vita.
 *
 * FILE selection and Continue are distinct blocking operations in the frozen
 * PE.  Generated hooks bracket only coarse, named call boundaries; this file
 * records their wall time and writes one summary after each completed top
 * level action.  It deliberately has no per-read, per-allocation or per-frame
 * logging.
 */
#include "kage_vita_continue_profile.h"
#include "gl_vita_backend.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(ISAAC_VITA_GAME_LOG_BATCH)
# include "host_vita_crt.h"
#endif

#if defined(ISAAC_VITA_CONTINUE_PROFILE)
# if defined(ISAAC_VITA_CONTINUE_PROFILE_ORACLE)
#  include "kage_vita_continue_profile_test_platform.h"
# else
#  include <psp2/kernel/clib.h>
#  include <psp2/kernel/processmgr.h>
#  include <psp2/kernel/threadmgr.h>
#  if defined(ISAAC_VITA_CONTINUE_OVERLAY)
#   include <psp2/gxm.h>
#   include <vitaGL.h>
#   include "kage_vita_loading.h"
#  endif
# endif

# if defined(ISAAC_VITA_CONTINUE_PROFILE_ORACLE)
#  define continue_profile_log sceClibPrintf
# else
void isaac_vita_log(const char *format, ...);
#  define continue_profile_log isaac_vita_log
# endif

#define CONTINUE_PROFILE_FAULT_ORDER       0x00000001u
#define CONTINUE_PROFILE_FAULT_CLOCK       0x00000004u
#define CONTINUE_PROFILE_FAULT_OVERLAP     0x00000008u
#define CONTINUE_PROFILE_FAULT_SATURATED   0x00000010u
#define CONTINUE_PROFILE_FAULT_ARITHMETIC  0x00000020u
#define CONTINUE_PROFILE_FAULT_OVERLAY     0x00000040u

#define CONTINUE_PROFILE_ROOM_SLOTS 4u
#define CONTINUE_PROFILE_NO_VALUE UINT32_MAX
#define CONTINUE_PROFILE_ACTIVE_NONE 0u
#define CONTINUE_PROFILE_ACTIVE_FILE 1u
#define CONTINUE_PROFILE_ACTIVE_RUN  2u

typedef struct ContinueProfileSpan {
    uint64_t begin;
    uint64_t total;
    uint32_t count;
    uint32_t open;
} ContinueProfileSpan;

typedef struct ContinueFileState {
    ContinueProfileSpan total;
    ContinueProfileSpan persistent;
    ContinueProfileSpan gamestate_load;
    ContinueProfileSpan gamestate_read;
    ContinueProfileSpan native_log;
    ContinueProfileSpan guest_log;
    ContinueProfileSpan guest_write;
    ContinueProfileSpan guest_flush;
    uint64_t guest_write_bytes;
    uint32_t guest_log_depth;
    uint32_t active;
    uint32_t faults;
    uint32_t overlay_swaps;
} ContinueFileState;

typedef struct ContinueRunState {
    ContinueProfileSpan total;
    ContinueProfileSpan restore;
    ContinueProfileSpan player_create;
    ContinueProfileSpan itempool_init;
    ContinueProfileSpan sfx_load;
    ContinueProfileSpan itempool_restore;
    ContinueProfileSpan players_pre;
    ContinueProfileSpan level_restore;
    ContinueProfileSpan players_post;
    ContinueProfileSpan room_load;
    ContinueProfileSpan native_log;
    ContinueProfileSpan guest_log;
    ContinueProfileSpan guest_write;
    ContinueProfileSpan guest_flush;
    uint64_t guest_write_bytes;
    uint32_t guest_log_depth;
    uint32_t room_stage[CONTINUE_PROFILE_ROOM_SLOTS];
    uint32_t room_us[CONTINUE_PROFILE_ROOM_SLOTS];
    uint32_t room_record_count;
    uint32_t room_stage_open;
    uint32_t active;
    uint32_t saw_restore;
    uint32_t faults;
    uint32_t exclusive_open;
    uint32_t overlay_swaps;
} ContinueRunState;

static ContinueFileState s_file;
static ContinueRunState s_run;
static uint32_t s_active_kind;
static uint32_t s_active_owner;

static uint32_t continue_profile_atomic_load(const uint32_t *value)
{
# if defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
# else
    return *value;
# endif
}

static void continue_profile_atomic_store(uint32_t *value,
                                          uint32_t replacement)
{
# if defined(__GNUC__) || defined(__clang__)
    __atomic_store_n(value, replacement, __ATOMIC_RELEASE);
# else
    *value = replacement;
# endif
}

static void continue_profile_publish(uint32_t kind, uint32_t owner)
{
    continue_profile_atomic_store(&s_active_owner, owner);
    continue_profile_atomic_store(&s_active_kind, kind);
}

static void continue_profile_unpublish(void)
{
    continue_profile_atomic_store(
        &s_active_kind, CONTINUE_PROFILE_ACTIVE_NONE);
}

static uint64_t continue_profile_now(void)
{
    return (uint64_t)sceKernelGetProcessTimeWide();
}

static uint32_t continue_profile_thread(void)
{
    return (uint32_t)sceKernelGetThreadId();
}

static void continue_profile_fault(uint32_t *faults, uint32_t bit)
{
# if defined(__GNUC__) || defined(__clang__)
    (void)__atomic_fetch_or(faults, bit, __ATOMIC_RELAXED);
# else
    *faults |= bit;
# endif
}

static void continue_profile_increment(uint32_t *value, uint32_t *faults)
{
    if (*value != UINT32_MAX)
        ++*value;
    else
        continue_profile_fault(faults, CONTINUE_PROFILE_FAULT_SATURATED);
}

static void continue_profile_add_u64(uint64_t *total, uint64_t value,
                                     uint32_t *faults)
{
    if (UINT64_MAX - *total < value) {
        *total = UINT64_MAX;
        continue_profile_fault(faults, CONTINUE_PROFILE_FAULT_SATURATED);
    } else {
        *total += value;
    }
}

static void continue_profile_span_begin(ContinueProfileSpan *span,
                                        uint64_t now, uint32_t *faults)
{
    if (span->open) {
        continue_profile_fault(faults, CONTINUE_PROFILE_FAULT_ORDER);
        return;
    }
    span->begin = now;
    span->open = 1u;
}

static uint64_t continue_profile_span_end(ContinueProfileSpan *span,
                                          uint64_t now, uint32_t *faults)
{
    uint64_t elapsed;

    if (!span->open) {
        continue_profile_fault(faults, CONTINUE_PROFILE_FAULT_ORDER);
        return 0u;
    }
    span->open = 0u;
    if (now < span->begin) {
        continue_profile_fault(faults, CONTINUE_PROFILE_FAULT_CLOCK);
        elapsed = 0u;
    } else {
        elapsed = now - span->begin;
    }
    if (UINT64_MAX - span->total < elapsed) {
        span->total = UINT64_MAX;
        continue_profile_fault(faults, CONTINUE_PROFILE_FAULT_SATURATED);
    } else {
        span->total += elapsed;
    }
    continue_profile_increment(&span->count, faults);
    return elapsed;
}

static uint32_t continue_profile_us32(uint64_t value, uint32_t *faults)
{
    if (value > UINT32_MAX) {
        continue_profile_fault(faults, CONTINUE_PROFILE_FAULT_SATURATED);
        return UINT32_MAX;
    }
    return (uint32_t)value;
}

static uint32_t continue_profile_residual(uint64_t total,
                                          const uint64_t *parts,
                                          size_t count,
                                          uint32_t *faults)
{
    uint64_t used = 0u;
    size_t index;

    for (index = 0u; index < count; ++index) {
        if (UINT64_MAX - used < parts[index]) {
            continue_profile_fault(faults,
                                   CONTINUE_PROFILE_FAULT_ARITHMETIC);
            return UINT32_MAX;
        }
        used += parts[index];
    }
    if (used > total) {
        continue_profile_fault(faults,
                               CONTINUE_PROFILE_FAULT_ARITHMETIC);
        return UINT32_MAX;
    }
    return continue_profile_us32(total - used, faults);
}

#if defined(ISAAC_VITA_CONTINUE_OVERLAY)
#define CONTINUE_OVERLAY_WIDTH 960u
#define CONTINUE_OVERLAY_HEIGHT 544u
#define CONTINUE_OVERLAY_STRIDE 960u
#define CONTINUE_OVERLAY_ACTIVE 0x80000000u
#define CONTINUE_OVERLAY_KIND_RUN 0x40000000u
#define CONTINUE_OVERLAY_STAGE_MASK 0x000000ffu
#define CONTINUE_OVERLAY_PHASE_MASK 0x00000700u
#define CONTINUE_OVERLAY_PHASE_SHIFT 8u
/* Same measured A8B8G8R8 scanout contract as kage_vita_loading.c. */
#define CONTINUE_OVERLAY_RGBA(r, g, b, a) \
    (((uint32_t)(a) << 24u) | ((uint32_t)(b) << 16u) | \
     ((uint32_t)(g) << 8u) | (uint32_t)(r))
#define CONTINUE_OVERLAY_BLACK \
    CONTINUE_OVERLAY_RGBA(0x00u, 0x00u, 0x00u, 0xffu)
#define CONTINUE_OVERLAY_BG \
    CONTINUE_OVERLAY_RGBA(0x12u, 0x0cu, 0x12u, 0xffu)
#define CONTINUE_OVERLAY_PANEL \
    CONTINUE_OVERLAY_RGBA(0x23u, 0x17u, 0x1eu, 0xffu)
#define CONTINUE_OVERLAY_TEXT \
    CONTINUE_OVERLAY_RGBA(0xebu, 0xe1u, 0xd6u, 0xffu)
#define CONTINUE_OVERLAY_MUTED \
    CONTINUE_OVERLAY_RGBA(0x9cu, 0x91u, 0x8au, 0xffu)
#define CONTINUE_OVERLAY_ACCENT \
    CONTINUE_OVERLAY_RGBA(0xb0u, 0x22u, 0x37u, 0xffu)
#define CONTINUE_OVERLAY_TRACK \
    CONTINUE_OVERLAY_RGBA(0x37u, 0x2du, 0x32u, 0xffu)

_Static_assert(CONTINUE_OVERLAY_WIDTH == CONTINUE_OVERLAY_STRIDE,
               "Continue overlay requires the measured 960-pixel stride");

static uint32_t s_overlay_snapshot;
static uint32_t s_overlay_active;
static uint32_t s_overlay_phase;
static uint32_t s_overlay_swaps;

static const uint8_t s_overlay_digits[10][7] = {
    {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},
    {14, 17, 1, 2, 4, 8, 31}, {30, 1, 1, 14, 1, 1, 30},
    {2, 6, 10, 18, 31, 2, 2}, {31, 16, 16, 30, 1, 1, 30},
    {6, 8, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},
    {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 2, 12}
};
static const uint8_t s_overlay_letters[26][7] = {
    {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},
    {15,16,16,16,16,16,15},{30,17,17,17,17,17,30},
    {31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
    {15,16,16,23,17,17,15},{17,17,17,31,17,17,17},
    {31,4,4,4,4,4,31},{7,2,2,2,18,18,12},
    {17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},
    {14,17,17,17,17,17,14},{30,17,17,30,16,16,16},
    {14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14},{17,17,17,17,17,10,4},
    {17,17,21,21,21,27,17},{17,10,4,4,4,10,17},
    {17,10,4,4,4,4,4},{31,1,2,4,8,16,31}
};
static const uint8_t s_overlay_blank[7] = {0, 0, 0, 0, 0, 0, 0};

static uint32_t continue_overlay_load(void)
{
# if defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(&s_overlay_snapshot, __ATOMIC_ACQUIRE);
# else
    return s_overlay_snapshot;
# endif
}

static void continue_overlay_store(uint32_t value)
{
# if defined(__GNUC__) || defined(__clang__)
    __atomic_store_n(&s_overlay_snapshot, value, __ATOMIC_RELEASE);
# else
    s_overlay_snapshot = value;
# endif
}

static void continue_overlay_rect(uint32_t *buffer, unsigned x, unsigned y,
                                  unsigned width, unsigned height,
                                  uint32_t color)
{
    unsigned row;
    unsigned column;

    if (!buffer || x >= CONTINUE_OVERLAY_WIDTH ||
        y >= CONTINUE_OVERLAY_HEIGHT)
        return;
    if (width > CONTINUE_OVERLAY_WIDTH - x)
        width = CONTINUE_OVERLAY_WIDTH - x;
    if (height > CONTINUE_OVERLAY_HEIGHT - y)
        height = CONTINUE_OVERLAY_HEIGHT - y;
    for (row = 0u; row < height; ++row) {
        uint32_t *out = buffer +
            (y + row) * CONTINUE_OVERLAY_STRIDE + x;
        for (column = 0u; column < width; ++column)
            out[column] = color;
    }
}

static const uint8_t *continue_overlay_glyph(char character)
{
    if (character >= 'A' && character <= 'Z')
        return s_overlay_letters[(unsigned)(character - 'A')];
    if (character >= '0' && character <= '9')
        return s_overlay_digits[(unsigned)(character - '0')];
    return s_overlay_blank;
}

static void continue_overlay_text(uint32_t *buffer, const char *text,
                                  unsigned x, unsigned y, unsigned scale,
                                  uint32_t color)
{
    unsigned index;

    for (index = 0u; text[index]; ++index) {
        const uint8_t *glyph = continue_overlay_glyph(text[index]);
        unsigned row;
        for (row = 0u; row < 7u; ++row) {
            unsigned column;
            for (column = 0u; column < 5u; ++column) {
                if (glyph[row] & (uint8_t)(1u << (4u - column)))
                    continue_overlay_rect(
                        buffer, x + index * 6u * scale + column * scale,
                        y + row * scale, scale, scale, color);
            }
        }
    }
}

static unsigned continue_overlay_text_width(const char *text, unsigned scale)
{
    unsigned length = 0u;
    while (text[length])
        ++length;
    return length ? (length * 6u - 1u) * scale : 0u;
}

static void continue_overlay_center(uint32_t *buffer, const char *text,
                                    unsigned y, unsigned scale,
                                    uint32_t color)
{
    unsigned width = continue_overlay_text_width(text, scale);
    unsigned x = width < CONTINUE_OVERLAY_WIDTH
        ? (CONTINUE_OVERLAY_WIDTH - width) / 2u : 0u;
    continue_overlay_text(buffer, text, x, y, scale, color);
}

static const char *continue_overlay_status(uint32_t kind_run, uint32_t stage)
{
    if (!kind_run) {
        switch (stage) {
        case 1u: return "READING PROFILE";
        case 2u: return "LOADING SAVE";
        case 3u: return "PARSING SAVE";
        default: return "SELECTING FILE";
        }
    }
    switch (stage) {
    case 1u: return "CREATING PLAYER";
    case 2u: return "LOADING ITEM POOL";
    case 3u: return "LOADING SOUND";
    case 4u: return "RESTORING ITEMS";
    case 5u: return "RESTORING PLAYERS";
    case 6u: return "LOADING LEVEL";
    case 7u: return "LOADING ROOMS";
    case 8u: return "FINISHING PLAYERS";
    default: return "RESTORING RUN";
    }
}

static void continue_overlay_callback(void *framebuffer_pointer)
{
    uint32_t *buffer = (uint32_t *)framebuffer_pointer;
    uint32_t snapshot = continue_overlay_load();
    uint32_t kind_run;
    uint32_t stage;
    uint32_t phase;
    unsigned index;

    if (!(snapshot & CONTINUE_OVERLAY_ACTIVE) || !buffer)
        return;
    kind_run = snapshot & CONTINUE_OVERLAY_KIND_RUN;
    stage = snapshot & CONTINUE_OVERLAY_STAGE_MASK;
    phase = (snapshot & CONTINUE_OVERLAY_PHASE_MASK) >>
        CONTINUE_OVERLAY_PHASE_SHIFT;

    continue_overlay_rect(buffer, 0u, 0u, CONTINUE_OVERLAY_WIDTH,
                          CONTINUE_OVERLAY_HEIGHT, CONTINUE_OVERLAY_BG);
    continue_overlay_rect(buffer, 0u, 0u, CONTINUE_OVERLAY_WIDTH, 8u,
                          CONTINUE_OVERLAY_ACCENT);
    continue_overlay_rect(buffer, 84u, 26u, 792u, 492u,
                          CONTINUE_OVERLAY_PANEL);
    continue_overlay_center(buffer,
                            kind_run ? "CONTINUING" : "LOADING FILE",
                            170u, 4u, CONTINUE_OVERLAY_TEXT);
    continue_overlay_center(buffer, continue_overlay_status(kind_run, stage),
                            252u, 2u, CONTINUE_OVERLAY_TEXT);
    continue_overlay_center(buffer, "PLEASE WAIT", 410u, 2u,
                            CONTINUE_OVERLAY_MUTED);

    for (index = 0u; index < 8u; ++index) {
        uint32_t color = index == phase
            ? CONTINUE_OVERLAY_TEXT : CONTINUE_OVERLAY_TRACK;
        continue_overlay_rect(buffer, 304u + index * 44u, 322u,
                              28u, 28u, color);
    }

    /* Match the established 960x540 guest viewport handoff: never leave the
     * four physical border rows carrying overlay colors. */
    continue_overlay_rect(buffer, 0u, 0u, CONTINUE_OVERLAY_WIDTH, 2u,
                          CONTINUE_OVERLAY_BLACK);
    continue_overlay_rect(buffer, 0u, 542u, CONTINUE_OVERLAY_WIDTH, 2u,
                          CONTINUE_OVERLAY_BLACK);
}

static int continue_overlay_begin(uint32_t kind_run, uint32_t stage,
                                  uint32_t *faults)
{
    uint32_t snapshot;

    if (s_overlay_active || kage_vita_loading_active()) {
        continue_profile_fault(faults, CONTINUE_PROFILE_FAULT_OVERLAY);
        return 0;
    }
    s_overlay_active = 1u;
    s_overlay_phase = 0u;
    s_overlay_swaps = 0u;
    snapshot = CONTINUE_OVERLAY_ACTIVE |
        (kind_run ? CONTINUE_OVERLAY_KIND_RUN : 0u) |
        (stage & CONTINUE_OVERLAY_STAGE_MASK);
    continue_overlay_store(snapshot);
    vglSetDisplayCallback(continue_overlay_callback);
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION)
    {
        /* Same scene-ending present as kage_vita_backend.c. */
        extern void gl_vita_backend_fbo_present(void);

        gl_vita_backend_fbo_present();
    }
#endif
    gl_vita_backend_attrib_sync();
    vglSwapBuffers(GL_FALSE);
    ++s_overlay_swaps;
    return 1;
}

static void continue_overlay_stage(uint32_t kind_run, uint32_t stage)
{
    uint32_t snapshot;

    if (!s_overlay_active)
        return;
    s_overlay_phase = (s_overlay_phase + 1u) & 7u;
    snapshot = CONTINUE_OVERLAY_ACTIVE |
        (kind_run ? CONTINUE_OVERLAY_KIND_RUN : 0u) |
        (s_overlay_phase << CONTINUE_OVERLAY_PHASE_SHIFT) |
        (stage & CONTINUE_OVERLAY_STAGE_MASK);
    continue_overlay_store(snapshot);
    gl_vita_backend_attrib_sync();
    vglSwapBuffers(GL_FALSE);
    ++s_overlay_swaps;
}

static uint32_t continue_overlay_finish(void)
{
    uint32_t swaps = s_overlay_swaps;

    if (!s_overlay_active)
        return 0u;
    continue_overlay_store(0u);
    vglSetDisplayCallback(NULL);
    (void)sceGxmDisplayQueueFinish();
    s_overlay_active = 0u;
    s_overlay_phase = 0u;
    s_overlay_swaps = 0u;
    return swaps;
}
#else
static int continue_overlay_begin(uint32_t kind_run, uint32_t stage,
                                  uint32_t *faults)
{
    (void)kind_run;
    (void)stage;
    (void)faults;
    return 0;
}

static void continue_overlay_stage(uint32_t kind_run, uint32_t stage)
{
    (void)kind_run;
    (void)stage;
}

static uint32_t continue_overlay_finish(void)
{
    return 0u;
}
#endif

static int continue_file_owner(void)
{
    return continue_profile_thread() ==
        continue_profile_atomic_load(&s_active_owner);
}

static int continue_run_owner(void)
{
    return continue_profile_thread() ==
        continue_profile_atomic_load(&s_active_owner);
}

#if defined(ISAAC_VITA_GAME_LOG_BATCH)
static void continue_log_batch_report(void)
{
    isaac_vita_crt_log_batch_snapshot snapshot;

    if (!isaac_vita_crt_log_batch_get_snapshot(&snapshot) ||
        snapshot.abi_version != ISAAC_VITA_CRT_LOG_BATCH_ABI)
        return;
    continue_profile_log(
        "[isaac-logbatch] info=%u deferred=%u batch=%u "
        "high=%u/%u/%u native=%u fail=%u reject=%u "
        "sticky=%u max=%u pending=%u",
        (unsigned)snapshot.info_seen,
        (unsigned)snapshot.info_deferred,
        (unsigned)snapshot.info_batch_flushes,
        (unsigned)snapshot.warn_forwarded,
        (unsigned)snapshot.error_forwarded,
        (unsigned)snapshot.assert_forwarded,
        (unsigned)snapshot.native_flush_calls,
        (unsigned)snapshot.native_failures,
        (unsigned)snapshot.chain_rejects,
        (unsigned)snapshot.sticky_fail_open,
        (unsigned)snapshot.max_deferred,
        (unsigned)snapshot.pending_info);
}
#else
static void continue_log_batch_report(void)
{
}
#endif

static void continue_file_report(uint64_t now)
{
    uint64_t top_parts[2];
    uint64_t load_parts[1];
    uint32_t total;
    uint32_t persistent;
    uint32_t loadstate;
    uint32_t read;
    uint32_t native_log_us;
    uint32_t guest_log_us;
    uint32_t guest_write_bytes;
    uint32_t guest_write_us;
    uint32_t guest_flush_us;
    uint32_t top_other;
    uint32_t load_other;
    uint32_t faults;

    (void)continue_profile_span_end(&s_file.total, now, &s_file.faults);
    total = continue_profile_us32(s_file.total.total, &s_file.faults);
    top_parts[0] = s_file.persistent.total;
    top_parts[1] = s_file.gamestate_load.total;
    top_other = continue_profile_residual(
        s_file.total.total, top_parts, 2u, &s_file.faults);
    load_parts[0] = s_file.gamestate_read.total;
    load_other = continue_profile_residual(
        s_file.gamestate_load.total, load_parts, 1u, &s_file.faults);
    s_file.overlay_swaps = continue_overlay_finish();
    persistent = continue_profile_us32(
        s_file.persistent.total, &s_file.faults);
    loadstate = continue_profile_us32(
        s_file.gamestate_load.total, &s_file.faults);
    read = continue_profile_us32(
        s_file.gamestate_read.total, &s_file.faults);
    native_log_us = continue_profile_us32(
        s_file.native_log.total, &s_file.faults);
    guest_log_us = continue_profile_us32(
        s_file.guest_log.total, &s_file.faults);
    guest_write_bytes = continue_profile_us32(
        s_file.guest_write_bytes, &s_file.faults);
    guest_write_us = continue_profile_us32(
        s_file.guest_write.total, &s_file.faults);
    guest_flush_us = continue_profile_us32(
        s_file.guest_flush.total, &s_file.faults);
    faults = s_file.faults;
    continue_profile_unpublish();
    continue_profile_log(
        "[isaac-continue] file total=%u persistent=%u loadstate=%u "
        "read=%u load_other=%u top_other=%u nlog=%u/%u "
        "glog=%u/%u write=%u/%u/%u flush=%u/%u "
        "overlay_swaps=%u faults=0x%02x",
        total, persistent, loadstate, read,
        load_other, top_other, (unsigned)s_file.native_log.count,
        native_log_us, (unsigned)s_file.guest_log.count, guest_log_us,
        (unsigned)s_file.guest_write.count,
        guest_write_bytes, guest_write_us,
        (unsigned)s_file.guest_flush.count, guest_flush_us,
        (unsigned)s_file.overlay_swaps, (unsigned)faults);
    continue_log_batch_report();
    memset(&s_file, 0, sizeof s_file);
}

static void continue_run_report(uint64_t now)
{
    uint64_t outer_parts[1];
    uint64_t restore_parts[6];
    uint32_t total;
    uint32_t outer_other;
    uint32_t restore_other;
    uint32_t restore;
    uint32_t player_create;
    uint32_t itempool_init;
    uint32_t sfx_load;
    uint32_t itempool_restore;
    uint32_t players_pre;
    uint32_t players_post;
    uint32_t level_restore;
    uint32_t room_load;
    uint32_t native_log_us;
    uint32_t guest_log_us;
    uint32_t guest_write_bytes;
    uint32_t guest_write_us;
    uint32_t guest_flush_us;
    uint32_t faults;

    (void)continue_profile_span_end(&s_run.total, now, &s_run.faults);
    if (!s_run.saw_restore) {
        continue_profile_unpublish();
        memset(&s_run, 0, sizeof s_run);
        return;
    }
    total = continue_profile_us32(s_run.total.total, &s_run.faults);
    outer_parts[0] = s_run.restore.total;
    outer_other = continue_profile_residual(
        s_run.total.total, outer_parts, 1u, &s_run.faults);
    restore_parts[0] = s_run.player_create.total;
    restore_parts[1] = s_run.itempool_init.total;
    restore_parts[2] = s_run.itempool_restore.total;
    restore_parts[3] = s_run.players_pre.total;
    restore_parts[4] = s_run.level_restore.total;
    restore_parts[5] = s_run.players_post.total;
    if (s_run.exclusive_open)
        continue_profile_fault(&s_run.faults,
                               CONTINUE_PROFILE_FAULT_ORDER);
    if (s_run.faults & CONTINUE_PROFILE_FAULT_OVERLAP) {
        restore_other = UINT32_MAX;
    } else {
        restore_other = continue_profile_residual(
            s_run.restore.total, restore_parts, 6u, &s_run.faults);
    }
    s_run.overlay_swaps = continue_overlay_finish();
    restore = continue_profile_us32(s_run.restore.total, &s_run.faults);
    player_create = continue_profile_us32(
        s_run.player_create.total, &s_run.faults);
    itempool_init = continue_profile_us32(
        s_run.itempool_init.total, &s_run.faults);
    sfx_load = continue_profile_us32(
        s_run.sfx_load.total, &s_run.faults);
    itempool_restore = continue_profile_us32(
        s_run.itempool_restore.total, &s_run.faults);
    players_pre = continue_profile_us32(
        s_run.players_pre.total, &s_run.faults);
    players_post = continue_profile_us32(
        s_run.players_post.total, &s_run.faults);
    level_restore = continue_profile_us32(
        s_run.level_restore.total, &s_run.faults);
    room_load = continue_profile_us32(
        s_run.room_load.total, &s_run.faults);
    native_log_us = continue_profile_us32(
        s_run.native_log.total, &s_run.faults);
    guest_log_us = continue_profile_us32(
        s_run.guest_log.total, &s_run.faults);
    guest_write_bytes = continue_profile_us32(
        s_run.guest_write_bytes, &s_run.faults);
    guest_write_us = continue_profile_us32(
        s_run.guest_write.total, &s_run.faults);
    guest_flush_us = continue_profile_us32(
        s_run.guest_flush.total, &s_run.faults);
    faults = s_run.faults;
    continue_profile_unpublish();
    continue_profile_log(
        "[isaac-continue] run total=%u restore=%u outer_other=%u "
        "player_create=%u pool_init=%u sfx=%u/%u pool_restore=%u "
        "players=%u/%u level=%u rooms=%u/%u restore_other=%u "
        "nlog=%u/%u glog=%u/%u write=%u/%u/%u flush=%u/%u "
        "room4=%u:%u,%u:%u,%u:%u,%u:%u "
        "overlay_swaps=%u faults=0x%02x",
        total, restore, outer_other, player_create, itempool_init,
        (unsigned)s_run.sfx_load.count, sfx_load, itempool_restore,
        players_pre, players_post, level_restore,
        (unsigned)s_run.room_load.count,
        room_load,
        restore_other, (unsigned)s_run.native_log.count,
        native_log_us, (unsigned)s_run.guest_log.count, guest_log_us,
        (unsigned)s_run.guest_write.count,
        guest_write_bytes, guest_write_us,
        (unsigned)s_run.guest_flush.count, guest_flush_us,
        (unsigned)s_run.room_stage[0], (unsigned)s_run.room_us[0],
        (unsigned)s_run.room_stage[1], (unsigned)s_run.room_us[1],
        (unsigned)s_run.room_stage[2], (unsigned)s_run.room_us[2],
        (unsigned)s_run.room_stage[3], (unsigned)s_run.room_us[3],
        (unsigned)s_run.overlay_swaps, (unsigned)faults);
    continue_log_batch_report();
    memset(&s_run, 0, sizeof s_run);
}

static void continue_run_exclusive_begin(ContinueProfileSpan *span,
                                         uint64_t now)
{
    if (s_run.exclusive_open)
        continue_profile_fault(&s_run.faults,
                               CONTINUE_PROFILE_FAULT_OVERLAP);
    continue_profile_increment(&s_run.exclusive_open, &s_run.faults);
    continue_profile_span_begin(span, now, &s_run.faults);
}

static uint64_t continue_run_exclusive_end(ContinueProfileSpan *span,
                                           uint64_t now)
{
    uint64_t elapsed = continue_profile_span_end(span, now, &s_run.faults);
    if (!s_run.exclusive_open)
        continue_profile_fault(&s_run.faults, CONTINUE_PROFILE_FAULT_ORDER);
    else
        --s_run.exclusive_open;
    return elapsed;
}

static void continue_profile_guest_log_note(
    ContinueProfileSpan *whole, ContinueProfileSpan *write,
    ContinueProfileSpan *flush, uint64_t *write_bytes,
    uint32_t *depth, uint32_t *faults,
    uint32_t event, uint32_t value, uint64_t now)
{
    switch (event) {
    case ISAAC_VITA_CONTINUE_GUEST_LOG_BEGIN:
        if (*depth == 0u)
            continue_profile_span_begin(whole, now, faults);
        continue_profile_increment(depth, faults);
        break;
    case ISAAC_VITA_CONTINUE_GUEST_LOG_WRITE_BEGIN:
        continue_profile_add_u64(write_bytes, value, faults);
        continue_profile_span_begin(write, now, faults);
        break;
    case ISAAC_VITA_CONTINUE_GUEST_LOG_WRITE_END:
        (void)continue_profile_span_end(write, now, faults);
        break;
    case ISAAC_VITA_CONTINUE_GUEST_LOG_FLUSH_BEGIN:
        continue_profile_span_begin(flush, now, faults);
        break;
    case ISAAC_VITA_CONTINUE_GUEST_LOG_FLUSH_END:
        (void)continue_profile_span_end(flush, now, faults);
        break;
    case ISAAC_VITA_CONTINUE_GUEST_LOG_END:
        if (*depth == 0u) {
            continue_profile_fault(faults, CONTINUE_PROFILE_FAULT_ORDER);
            break;
        }
        --*depth;
        if (*depth == 0u)
            (void)continue_profile_span_end(whole, now, faults);
        break;
    default:
        continue_profile_fault(faults, CONTINUE_PROFILE_FAULT_ORDER);
        break;
    }
}

void isaac_vita_continue_profile_note(uint32_t event, uint32_t value)
{
    uint64_t now;
    uint64_t elapsed;

    if (event == ISAAC_VITA_CONTINUE_FILE_BEGIN) {
        uint32_t previous = continue_profile_atomic_load(&s_active_kind);
        uint32_t owner = continue_profile_thread();
        memset(&s_file, 0, sizeof s_file);
        if (previous != CONTINUE_PROFILE_ACTIVE_NONE)
            continue_profile_fault(&s_file.faults,
                                   CONTINUE_PROFILE_FAULT_ORDER);
        s_file.active = 1u;
        continue_profile_unpublish();
        continue_profile_publish(CONTINUE_PROFILE_ACTIVE_FILE, owner);
        now = continue_profile_now();
        continue_profile_span_begin(&s_file.total, now, &s_file.faults);
        (void)continue_overlay_begin(0u, 0u, &s_file.faults);
        return;
    }
    if (event == ISAAC_VITA_CONTINUE_CANDIDATE_BEGIN) {
        uint32_t previous = continue_profile_atomic_load(&s_active_kind);
        uint32_t owner = continue_profile_thread();
        memset(&s_run, 0, sizeof s_run);
        if (previous != CONTINUE_PROFILE_ACTIVE_NONE)
            continue_profile_fault(&s_run.faults,
                                   CONTINUE_PROFILE_FAULT_ORDER);
        s_run.room_stage[0] = CONTINUE_PROFILE_NO_VALUE;
        s_run.room_stage[1] = CONTINUE_PROFILE_NO_VALUE;
        s_run.room_stage[2] = CONTINUE_PROFILE_NO_VALUE;
        s_run.room_stage[3] = CONTINUE_PROFILE_NO_VALUE;
        s_run.room_stage_open = CONTINUE_PROFILE_NO_VALUE;
        s_run.active = 1u;
        continue_profile_unpublish();
        continue_profile_publish(CONTINUE_PROFILE_ACTIVE_RUN, owner);
        now = continue_profile_now();
        continue_profile_span_begin(&s_run.total, now, &s_run.faults);
        return;
    }

    if (event >= ISAAC_VITA_CONTINUE_PERSISTENT_BEGIN &&
        event <= ISAAC_VITA_CONTINUE_FILE_END) {
        if (continue_profile_atomic_load(&s_active_kind) !=
                CONTINUE_PROFILE_ACTIVE_FILE || !continue_file_owner() ||
                !s_file.active)
            return;
        now = continue_profile_now();
        switch (event) {
        case ISAAC_VITA_CONTINUE_PERSISTENT_BEGIN:
            continue_profile_span_begin(&s_file.persistent, now,
                                        &s_file.faults);
            continue_overlay_stage(0u, 1u);
            break;
        case ISAAC_VITA_CONTINUE_PERSISTENT_END:
            (void)continue_profile_span_end(&s_file.persistent, now,
                                            &s_file.faults);
            break;
        case ISAAC_VITA_CONTINUE_GAMESTATE_LOAD_BEGIN:
            continue_profile_span_begin(&s_file.gamestate_load, now,
                                        &s_file.faults);
            continue_overlay_stage(0u, 2u);
            break;
        case ISAAC_VITA_CONTINUE_GAMESTATE_LOAD_END:
            (void)continue_profile_span_end(&s_file.gamestate_load, now,
                                            &s_file.faults);
            break;
        case ISAAC_VITA_CONTINUE_GAMESTATE_READ_BEGIN:
            continue_profile_span_begin(&s_file.gamestate_read, now,
                                        &s_file.faults);
            continue_overlay_stage(0u, 3u);
            break;
        case ISAAC_VITA_CONTINUE_GAMESTATE_READ_END:
            (void)continue_profile_span_end(&s_file.gamestate_read, now,
                                            &s_file.faults);
            break;
        case ISAAC_VITA_CONTINUE_FILE_END:
            continue_file_report(now);
            break;
        default:
            break;
        }
        return;
    }

    if (event >= ISAAC_VITA_CONTINUE_GUEST_LOG_BEGIN &&
        event <= ISAAC_VITA_CONTINUE_GUEST_LOG_END) {
        uint32_t kind = continue_profile_atomic_load(&s_active_kind);
        uint32_t owner = continue_profile_atomic_load(&s_active_owner);

        /* Other native/guest workers may log concurrently.  They neither
         * block this owner-thread action nor touch its non-atomic buckets. */
        if (continue_profile_thread() != owner)
            return;
        if (kind == CONTINUE_PROFILE_ACTIVE_FILE && s_file.active) {
            now = continue_profile_now();
            continue_profile_guest_log_note(
                &s_file.guest_log, &s_file.guest_write,
                &s_file.guest_flush, &s_file.guest_write_bytes,
                &s_file.guest_log_depth, &s_file.faults,
                event, value, now);
        } else if (kind == CONTINUE_PROFILE_ACTIVE_RUN && s_run.active) {
            now = continue_profile_now();
            continue_profile_guest_log_note(
                &s_run.guest_log, &s_run.guest_write,
                &s_run.guest_flush, &s_run.guest_write_bytes,
                &s_run.guest_log_depth, &s_run.faults,
                event, value, now);
        }
        return;
    }

    /* RoomConfig and ItemPool hooks also run outside Continue.  An inactive
     * candidate is therefore an expected fast no-op, not a lifecycle error. */
    if (continue_profile_atomic_load(&s_active_kind) !=
            CONTINUE_PROFILE_ACTIVE_RUN || !continue_run_owner() ||
            !s_run.active)
        return;
    now = continue_profile_now();
    switch (event) {
    case ISAAC_VITA_CONTINUE_RESTORE_BEGIN:
        s_run.saw_restore = 1u;
        continue_profile_span_begin(&s_run.restore, now, &s_run.faults);
        (void)continue_overlay_begin(1u, 0u, &s_run.faults);
        break;
    case ISAAC_VITA_CONTINUE_RESTORE_END:
        (void)continue_profile_span_end(&s_run.restore, now, &s_run.faults);
        break;
    case ISAAC_VITA_CONTINUE_CANDIDATE_END:
        continue_run_report(now);
        break;
    case ISAAC_VITA_CONTINUE_PLAYER_CREATE_BEGIN:
        continue_run_exclusive_begin(&s_run.player_create, now);
        continue_overlay_stage(1u, 1u);
        break;
    case ISAAC_VITA_CONTINUE_PLAYER_CREATE_END:
        (void)continue_run_exclusive_end(&s_run.player_create, now);
        break;
    case ISAAC_VITA_CONTINUE_ITEMPOOL_INIT_BEGIN:
        continue_run_exclusive_begin(&s_run.itempool_init, now);
        continue_overlay_stage(1u, 2u);
        break;
    case ISAAC_VITA_CONTINUE_ITEMPOOL_INIT_END:
        (void)continue_run_exclusive_end(&s_run.itempool_init, now);
        break;
    case ISAAC_VITA_CONTINUE_SFX_LOAD_BEGIN:
        continue_profile_span_begin(&s_run.sfx_load, now, &s_run.faults);
        continue_overlay_stage(1u, 3u);
        break;
    case ISAAC_VITA_CONTINUE_SFX_LOAD_END:
        (void)continue_profile_span_end(&s_run.sfx_load, now,
                                        &s_run.faults);
        break;
    case ISAAC_VITA_CONTINUE_ITEMPOOL_RESTORE_BEGIN:
        continue_run_exclusive_begin(&s_run.itempool_restore, now);
        continue_overlay_stage(1u, 4u);
        break;
    case ISAAC_VITA_CONTINUE_ITEMPOOL_RESTORE_END:
        (void)continue_run_exclusive_end(&s_run.itempool_restore, now);
        break;
    case ISAAC_VITA_CONTINUE_PLAYERS_PRE_BEGIN:
        continue_run_exclusive_begin(&s_run.players_pre, now);
        continue_overlay_stage(1u, 5u);
        break;
    case ISAAC_VITA_CONTINUE_PLAYERS_PRE_END:
        (void)continue_run_exclusive_end(&s_run.players_pre, now);
        break;
    case ISAAC_VITA_CONTINUE_LEVEL_RESTORE_BEGIN:
        continue_run_exclusive_begin(&s_run.level_restore, now);
        continue_overlay_stage(1u, 6u);
        break;
    case ISAAC_VITA_CONTINUE_LEVEL_RESTORE_END:
        (void)continue_run_exclusive_end(&s_run.level_restore, now);
        break;
    case ISAAC_VITA_CONTINUE_PLAYERS_POST_BEGIN:
        continue_run_exclusive_begin(&s_run.players_post, now);
        continue_overlay_stage(1u, 8u);
        break;
    case ISAAC_VITA_CONTINUE_PLAYERS_POST_END:
        (void)continue_run_exclusive_end(&s_run.players_post, now);
        break;
    case ISAAC_VITA_CONTINUE_ROOM_LOAD_BEGIN:
        s_run.room_stage_open = value;
        continue_profile_span_begin(&s_run.room_load, now, &s_run.faults);
        continue_overlay_stage(1u, 7u);
        break;
    case ISAAC_VITA_CONTINUE_ROOM_LOAD_END:
        elapsed = continue_profile_span_end(&s_run.room_load, now,
                                            &s_run.faults);
        if (s_run.room_record_count < CONTINUE_PROFILE_ROOM_SLOTS) {
            uint32_t slot = s_run.room_record_count++;
            s_run.room_stage[slot] = s_run.room_stage_open;
            s_run.room_us[slot] = continue_profile_us32(
                elapsed, &s_run.faults);
        } else {
            continue_profile_fault(&s_run.faults,
                                   CONTINUE_PROFILE_FAULT_SATURATED);
        }
        s_run.room_stage_open = CONTINUE_PROFILE_NO_VALUE;
        break;
    default:
        continue_profile_fault(&s_run.faults,
                               CONTINUE_PROFILE_FAULT_ORDER);
        break;
    }
}

void isaac_vita_continue_profile_log_begin(void)
{
    uint64_t now;
    uint32_t kind = continue_profile_atomic_load(&s_active_kind);
    uint32_t owner = continue_profile_atomic_load(&s_active_owner);

    if (kind == CONTINUE_PROFILE_ACTIVE_NONE ||
        continue_profile_thread() != owner)
        return;
    now = continue_profile_now();

    if (kind == CONTINUE_PROFILE_ACTIVE_FILE && s_file.active)
        continue_profile_span_begin(&s_file.native_log, now, &s_file.faults);
    else if (kind == CONTINUE_PROFILE_ACTIVE_RUN && s_run.active)
        continue_profile_span_begin(&s_run.native_log, now, &s_run.faults);
}

void isaac_vita_continue_profile_log_end(void)
{
    uint64_t now;
    uint32_t kind = continue_profile_atomic_load(&s_active_kind);
    uint32_t owner = continue_profile_atomic_load(&s_active_owner);

    if (kind == CONTINUE_PROFILE_ACTIVE_NONE ||
        continue_profile_thread() != owner)
        return;
    now = continue_profile_now();

    if (kind == CONTINUE_PROFILE_ACTIVE_FILE && s_file.active)
        (void)continue_profile_span_end(&s_file.native_log, now,
                                        &s_file.faults);
    else if (kind == CONTINUE_PROFILE_ACTIVE_RUN && s_run.active)
        (void)continue_profile_span_end(&s_run.native_log, now,
                                        &s_run.faults);
}

#else

void isaac_vita_continue_profile_note(uint32_t event, uint32_t value)
{
    (void)event;
    (void)value;
}

void isaac_vita_continue_profile_log_begin(void)
{
}

void isaac_vita_continue_profile_log_end(void)
{
}

#endif
