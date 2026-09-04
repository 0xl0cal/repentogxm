/* Early Vita loading screen drawn by vitaGL's existing display queue.
 *
 * vitaGL is already initialized by kage_vita_backend before the guest spends
 * roughly a minute walking its packed archives.  vglSetDisplayCallback gives
 * us the final A8B8G8R8 display buffer on the display-queue thread, after GXM
 * rendering and before sceDisplaySetFrameBuf.  Writing that CPU buffer keeps
 * the overlay completely outside the guest's framebuffer, shader, viewport,
 * scissor and texture state.  It also avoids vitaGL's built-in splash, whose
 * second GXM context is fatal under Vita3K.
 */
#include "kage_vita_loading.h"
#include "kage_vita_io_profile.h"
#include "kage_vita_stall_probe.h"
#if defined(ISAAC_VITA_CRT_SEEK_SHADOW)
#include "host_vita_crt.h"
#endif

#include <stddef.h>
#include <stdint.h>

#if defined(ISAAC_KAGE_VITA_LOADING_ORACLE)
# include "kage_vita_loading_test_vitagl.h"
#else
# include <psp2/gxm.h>
# include <psp2/kernel/clib.h>
# include <psp2/kernel/processmgr.h>
# include <psp2/kernel/threadmgr.h>
# include <vitaGL.h>
#endif

#if defined(_MSC_VER)
# include <intrin.h>
#endif

#ifndef ISAAC_KAGE_VITA_LOADING_PRESENTATION
# define ISAAC_KAGE_VITA_LOADING_PRESENTATION 1
#endif

#if ISAAC_KAGE_VITA_LOADING_PRESENTATION != 0 && \
    ISAAC_KAGE_VITA_LOADING_PRESENTATION != 1
# error "ISAAC_KAGE_VITA_LOADING_PRESENTATION must be 0 or 1"
#endif

/* Scanout geometry.  The build passes ISAAC_VITA_DISPLAY_RASTER_* when the
 * default framebuffer is scaled (720x408 at 3/4, 480x272 at 1/2); otherwise
 * this is the native 960x544 panel.  vitaGL pads the display stride to 64
 * pixels (gxm.c DISPLAY_STRIDE = VGL_ALIGN(DISPLAY_WIDTH, 64)): 960 -> 960,
 * 720 -> 768, 480 -> 512.  Layout constants below are authored in the
 * original 960x544 pixel grid and scaled through KAGE_LOADING_AT. */
#if defined(ISAAC_VITA_DISPLAY_RASTER_WIDTH)
# define KAGE_LOADING_WIDTH  ((unsigned)ISAAC_VITA_DISPLAY_RASTER_WIDTH)
# define KAGE_LOADING_HEIGHT ((unsigned)ISAAC_VITA_DISPLAY_RASTER_HEIGHT)
# define KAGE_LOADING_SCALE_NUM ((unsigned)ISAAC_VITA_DISPLAY_RASTER_NUM)
# define KAGE_LOADING_SCALE_DEN ((unsigned)ISAAC_VITA_DISPLAY_RASTER_DEN)
#else
# define KAGE_LOADING_WIDTH            960u
# define KAGE_LOADING_HEIGHT           544u
# define KAGE_LOADING_SCALE_NUM        1u
# define KAGE_LOADING_SCALE_DEN        1u
#endif
#define KAGE_LOADING_STRIDE            ((KAGE_LOADING_WIDTH + 63u) & ~63u)
/* A 960-grid coordinate or extent in raster pixels (floor). */
#define KAGE_LOADING_AT(value) \
    ((value) * KAGE_LOADING_SCALE_NUM / KAGE_LOADING_SCALE_DEN)
/* A 960-grid glyph/animation pixel scale; never below one raster pixel. */
#define KAGE_LOADING_PX(value) \
    (KAGE_LOADING_AT(value) ? KAGE_LOADING_AT(value) : 1u)
/* The guest's centred viewport: logical 960x540 scaled, with the spare rows
 * split above and below (two at 960x544, one at 720x408 and 480x272). */
#define KAGE_LOADING_VIEWPORT_HEIGHT   KAGE_LOADING_AT(540u)
#define KAGE_LOADING_VIEWPORT_Y \
    ((KAGE_LOADING_HEIGHT - KAGE_LOADING_VIEWPORT_HEIGHT) / 2u)
#define KAGE_LOADING_VIEWPORT_END \
    (KAGE_LOADING_VIEWPORT_Y + KAGE_LOADING_VIEWPORT_HEIGHT)
#define KAGE_LOADING_FRAME_INTERVAL_US  233854u
#define KAGE_LOADING_SNAPSHOT_ACTIVE    0x80000000u
#define KAGE_LOADING_SNAPSHOT_STAGE     0x0000000fu
#define KAGE_LOADING_SNAPSHOT_FRAME     0x000001f0u
#define KAGE_LOADING_SNAPSHOT_FRAME_SHIFT 4u
#define KAGE_LOADING_SNAPSHOT_ELAPSED   0x7ffffe00u
#define KAGE_LOADING_SNAPSHOT_ELAPSED_SHIFT 9u
#define KAGE_LOADING_ELAPSED_MAX \
    (KAGE_LOADING_SNAPSHOT_ELAPSED >> \
        KAGE_LOADING_SNAPSHOT_ELAPSED_SHIFT)

/* A8B8G8R8 is represented as 0xAABBGGRR in a little-endian uint32_t. */
#define KAGE_LOADING_RGBA8(r, g, b, a) \
    (((uint32_t)(a) << 24u) | ((uint32_t)(b) << 16u) | \
     ((uint32_t)(g) << 8u) | (uint32_t)(r))

#define KAGE_LOADING_BACKGROUND \
    KAGE_LOADING_RGBA8(0x12u, 0x0cu, 0x12u, 0xffu)
#define KAGE_LOADING_BORDER \
    KAGE_LOADING_RGBA8(0x00u, 0x00u, 0x00u, 0xffu)
#define KAGE_LOADING_PANEL \
    KAGE_LOADING_RGBA8(0x23u, 0x17u, 0x1eu, 0xffu)
#define KAGE_LOADING_TEXT \
    KAGE_LOADING_RGBA8(0xebu, 0xe1u, 0xd6u, 0xffu)
#define KAGE_LOADING_MUTED \
    KAGE_LOADING_RGBA8(0x9cu, 0x91u, 0x8au, 0xffu)
#define KAGE_LOADING_ACCENT \
    KAGE_LOADING_RGBA8(0xb0u, 0x22u, 0x37u, 0xffu)
#define KAGE_LOADING_TRACK \
    KAGE_LOADING_RGBA8(0x37u, 0x2du, 0x32u, 0xffu)
#define KAGE_LOADING_FILL \
    KAGE_LOADING_RGBA8(0xdcu, 0xd2u, 0xc3u, 0xffu)

_Static_assert(KAGE_LOADING_STRIDE >= KAGE_LOADING_WIDTH &&
               KAGE_LOADING_STRIDE % 64u == 0u,
               "display stride must be the 64-pixel padded vitaGL stride");
_Static_assert(KAGE_LOADING_VIEWPORT_HEIGHT <= KAGE_LOADING_HEIGHT &&
               KAGE_LOADING_AT(960u) <= KAGE_LOADING_WIDTH,
               "scaled 960x540 viewport must fit the display raster");

/* One release/acquire word is the complete display-queue-thread contract.
 * The callback reads it once, so it sees either an inactive screen or one
 * immutable progress/frame pair.  No lock, allocation, stdio, Vita API or GL
 * call is allowed from that callback. */
static uint32_t s_display_snapshot;
static uint32_t s_control_lock;
static int s_owner_thread;
static uint64_t s_start_time;
static uint64_t s_last_present_time;
static unsigned s_stage;
static unsigned s_animation_frame;
static unsigned s_loading_swaps;
static unsigned s_foreign_thread_notes;
static unsigned s_verify_notes;
static unsigned s_archive_notes;
static unsigned s_shader_notes;

static void kage_loading_publish_swap_count(void)
{
    KAGE_VITA_STALL_NOTE_LOADING_SWAP(s_loading_swaps);
}

/* Diagnostic builds override these with --wrap=sceIoRead/sceIoLseek
 * aggregation. Keeping weak no-ops here makes the release loader independent
 * from profiling and gives it exactly zero per-I/O overhead when disabled. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void kage_vita_io_profile_begin(void)
{
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void kage_vita_io_profile_report(const char *reason)
{
    (void)reason;
}

static uint32_t kage_loading_snapshot_load(void)
{
#if defined(_MSC_VER)
    return (uint32_t)_InterlockedCompareExchange(
        (volatile long *)&s_display_snapshot, 0, 0);
#else
    return __atomic_load_n(&s_display_snapshot, __ATOMIC_ACQUIRE);
#endif
}

static void kage_loading_snapshot_store(uint32_t value)
{
#if defined(_MSC_VER)
    (void)_InterlockedExchange((volatile long *)&s_display_snapshot,
                               (long)value);
#else
    __atomic_store_n(&s_display_snapshot, value, __ATOMIC_RELEASE);
#endif
}

static unsigned kage_loading_elapsed_at(uint64_t now)
{
    uint64_t elapsed;
    if (now < s_start_time)
        return 0u;
    elapsed = (now - s_start_time) / UINT64_C(1000000);
    return elapsed > KAGE_LOADING_ELAPSED_MAX
        ? KAGE_LOADING_ELAPSED_MAX : (unsigned)elapsed;
}

static uint32_t kage_loading_snapshot(unsigned stage, unsigned frame,
                                      unsigned elapsed_seconds)
{
    if (stage > KAGE_VITA_LOADING_GAME)
        stage = KAGE_VITA_LOADING_BOOT;
    if (elapsed_seconds > KAGE_LOADING_ELAPSED_MAX)
        elapsed_seconds = KAGE_LOADING_ELAPSED_MAX;
    return KAGE_LOADING_SNAPSHOT_ACTIVE |
        ((uint32_t)elapsed_seconds <<
            KAGE_LOADING_SNAPSHOT_ELAPSED_SHIFT) |
        ((uint32_t)(frame % 32u) <<
            KAGE_LOADING_SNAPSHOT_FRAME_SHIFT) |
        (uint32_t)stage;
}

/* Serialize the main-thread state transition with a possible defensive
 * finish from another thread.  The display callback never takes this lock;
 * it only consumes the independent snapshot above. */
static void kage_loading_control_lock(void)
{
#if defined(_MSC_VER)
    while (_InterlockedCompareExchange(
               (volatile long *)&s_control_lock, 1, 0) != 0) {
    }
#else
    while (__atomic_exchange_n(
               &s_control_lock, 1u, __ATOMIC_ACQUIRE) != 0u) {
    }
#endif
}

static void kage_loading_control_unlock(void)
{
#if defined(_MSC_VER)
    (void)_InterlockedExchange((volatile long *)&s_control_lock, 0);
#else
    __atomic_store_n(&s_control_lock, 0u, __ATOMIC_RELEASE);
#endif
}

#if ISAAC_KAGE_VITA_LOADING_PRESENTATION
static void kage_loading_fill_rect(uint32_t *framebuffer,
                                   unsigned x, unsigned y,
                                   unsigned width, unsigned height,
                                   uint32_t color)
{
    unsigned row;
    unsigned column;

    if (!framebuffer || x >= KAGE_LOADING_WIDTH ||
        y >= KAGE_LOADING_HEIGHT)
        return;
    if (width > KAGE_LOADING_WIDTH - x)
        width = KAGE_LOADING_WIDTH - x;
    if (height > KAGE_LOADING_HEIGHT - y)
        height = KAGE_LOADING_HEIGHT - y;
    for (row = 0u; row < height; ++row) {
        uint32_t *output = framebuffer +
            (y + row) * KAGE_LOADING_STRIDE + x;
        for (column = 0u; column < width; ++column)
            output[column] = color;
    }
}

#if defined(ISAAC_KAGE_VITA_LOADING_SPECIALIST)
# include "kage_vita_loading_specialist.inc"
# define KAGE_LOADING_ANIMATION_FRAME_COUNT KAGE_LOADING_DANCE_FRAME_COUNT
# define KAGE_LOADING_PRESENTATION_TEXT "SPECIALIST DANCE"

_Static_assert(KAGE_LOADING_DANCE_WIDTH * KAGE_LOADING_DANCE_HEIGHT <
                   0x8000u,
               "one dance frame must fit the 15-bit RLE count");

static uint32_t kage_loading_rgb565(uint16_t packed)
{
    uint32_t red = (packed >> 11u) & 0x1fu;
    uint32_t green = (packed >> 5u) & 0x3fu;
    uint32_t blue = packed & 0x1fu;

    red = (red << 3u) | (red >> 2u);
    green = (green << 2u) | (green >> 4u);
    blue = (blue << 3u) | (blue >> 2u);
    return KAGE_LOADING_RGBA8(red, green, blue, 0xffu);
}

/* The source frames are pre-composited over KAGE_LOADING_PANEL by the
 * deterministic converter.  Transparent runs leave the panel untouched;
 * literal pixels are expanded with nearest-neighbour scaling. */
static void kage_loading_draw_animation(uint32_t *framebuffer,
                                        unsigned frame_index,
                                        unsigned x, unsigned y,
                                        unsigned scale)
{
    uint32_t cursor;
    uint32_t end;
    unsigned pixel = 0u;
    const unsigned pixel_count =
        KAGE_LOADING_DANCE_WIDTH * KAGE_LOADING_DANCE_HEIGHT;

    if (!framebuffer || !scale)
        return;
    frame_index %= KAGE_LOADING_DANCE_FRAME_COUNT;
    cursor = s_loading_dance_offsets[frame_index];
    end = s_loading_dance_offsets[frame_index + 1u];
    if (cursor > end || end > KAGE_LOADING_DANCE_WORD_COUNT)
        return;

    while (cursor < end && pixel < pixel_count) {
        uint16_t token = s_loading_dance_words[cursor++];
        unsigned count = token & 0x7fffu;
        if (!count || count > pixel_count - pixel)
            return;
        if (token & 0x8000u) {
            pixel += count;
            continue;
        }
        if (count > end - cursor)
            return;
        while (count--) {
            unsigned source_x = pixel % KAGE_LOADING_DANCE_WIDTH;
            unsigned source_y = pixel / KAGE_LOADING_DANCE_WIDTH;
            uint32_t color = kage_loading_rgb565(
                s_loading_dance_words[cursor++]);
            kage_loading_fill_rect(framebuffer,
                                   x + source_x * scale,
                                   y + source_y * scale,
                                   scale, scale, color);
            ++pixel;
        }
    }
}
#else
# include "kage_vita_loading_fallback.inc"
#endif
#else
# define KAGE_LOADING_ANIMATION_FRAME_COUNT 32u
#endif

_Static_assert(KAGE_LOADING_ANIMATION_FRAME_COUNT == 32u,
               "loading animation cadence assumes 32 sampled frames");
_Static_assert((KAGE_LOADING_SNAPSHOT_FRAME >>
                    KAGE_LOADING_SNAPSHOT_FRAME_SHIFT) + 1u ==
                   KAGE_LOADING_ANIMATION_FRAME_COUNT,
               "loading snapshot must encode every animation frame");
_Static_assert((KAGE_LOADING_SNAPSHOT_FRAME &
                    KAGE_LOADING_SNAPSHOT_STAGE) == 0u &&
               (KAGE_LOADING_SNAPSHOT_ELAPSED &
                    (KAGE_LOADING_SNAPSHOT_FRAME |
                     KAGE_LOADING_SNAPSHOT_STAGE)) == 0u,
               "loading snapshot fields must not overlap");
_Static_assert(KAGE_VITA_LOADING_GAME <= KAGE_LOADING_SNAPSHOT_STAGE,
               "loading snapshot must encode every stage");

#if ISAAC_KAGE_VITA_LOADING_PRESENTATION
/* Compact 5x7 uppercase font.  Keeping it local means the display callback
 * only reads immutable data and writes bounded pixels. */
static const uint8_t s_loading_digits[10][7] = {
    { 0x0eu, 0x11u, 0x13u, 0x15u, 0x19u, 0x11u, 0x0eu },
    { 0x04u, 0x0cu, 0x04u, 0x04u, 0x04u, 0x04u, 0x0eu },
    { 0x0eu, 0x11u, 0x01u, 0x02u, 0x04u, 0x08u, 0x1fu },
    { 0x1eu, 0x01u, 0x01u, 0x0eu, 0x01u, 0x01u, 0x1eu },
    { 0x02u, 0x06u, 0x0au, 0x12u, 0x1fu, 0x02u, 0x02u },
    { 0x1fu, 0x10u, 0x10u, 0x1eu, 0x01u, 0x01u, 0x1eu },
    { 0x06u, 0x08u, 0x10u, 0x1eu, 0x11u, 0x11u, 0x0eu },
    { 0x1fu, 0x01u, 0x02u, 0x04u, 0x08u, 0x08u, 0x08u },
    { 0x0eu, 0x11u, 0x11u, 0x0eu, 0x11u, 0x11u, 0x0eu },
    { 0x0eu, 0x11u, 0x11u, 0x0fu, 0x01u, 0x02u, 0x0cu }
};

static const uint8_t s_loading_letters[26][7] = {
    { 0x0eu, 0x11u, 0x11u, 0x1fu, 0x11u, 0x11u, 0x11u }, /* A */
    { 0x1eu, 0x11u, 0x11u, 0x1eu, 0x11u, 0x11u, 0x1eu }, /* B */
    { 0x0fu, 0x10u, 0x10u, 0x10u, 0x10u, 0x10u, 0x0fu }, /* C */
    { 0x1eu, 0x11u, 0x11u, 0x11u, 0x11u, 0x11u, 0x1eu }, /* D */
    { 0x1fu, 0x10u, 0x10u, 0x1eu, 0x10u, 0x10u, 0x1fu }, /* E */
    { 0x1fu, 0x10u, 0x10u, 0x1eu, 0x10u, 0x10u, 0x10u }, /* F */
    { 0x0fu, 0x10u, 0x10u, 0x17u, 0x11u, 0x11u, 0x0fu }, /* G */
    { 0x11u, 0x11u, 0x11u, 0x1fu, 0x11u, 0x11u, 0x11u }, /* H */
    { 0x1fu, 0x04u, 0x04u, 0x04u, 0x04u, 0x04u, 0x1fu }, /* I */
    { 0x07u, 0x02u, 0x02u, 0x02u, 0x12u, 0x12u, 0x0cu }, /* J */
    { 0x11u, 0x12u, 0x14u, 0x18u, 0x14u, 0x12u, 0x11u }, /* K */
    { 0x10u, 0x10u, 0x10u, 0x10u, 0x10u, 0x10u, 0x1fu }, /* L */
    { 0x11u, 0x1bu, 0x15u, 0x15u, 0x11u, 0x11u, 0x11u }, /* M */
    { 0x11u, 0x19u, 0x15u, 0x13u, 0x11u, 0x11u, 0x11u }, /* N */
    { 0x0eu, 0x11u, 0x11u, 0x11u, 0x11u, 0x11u, 0x0eu }, /* O */
    { 0x1eu, 0x11u, 0x11u, 0x1eu, 0x10u, 0x10u, 0x10u }, /* P */
    { 0x0eu, 0x11u, 0x11u, 0x11u, 0x15u, 0x12u, 0x0du }, /* Q */
    { 0x1eu, 0x11u, 0x11u, 0x1eu, 0x14u, 0x12u, 0x11u }, /* R */
    { 0x0fu, 0x10u, 0x10u, 0x0eu, 0x01u, 0x01u, 0x1eu }, /* S */
    { 0x1fu, 0x04u, 0x04u, 0x04u, 0x04u, 0x04u, 0x04u }, /* T */
    { 0x11u, 0x11u, 0x11u, 0x11u, 0x11u, 0x11u, 0x0eu }, /* U */
    { 0x11u, 0x11u, 0x11u, 0x11u, 0x11u, 0x0au, 0x04u }, /* V */
    { 0x11u, 0x11u, 0x15u, 0x15u, 0x15u, 0x1bu, 0x11u }, /* W */
    { 0x11u, 0x0au, 0x04u, 0x04u, 0x04u, 0x0au, 0x11u }, /* X */
    { 0x11u, 0x0au, 0x04u, 0x04u, 0x04u, 0x04u, 0x04u }, /* Y */
    { 0x1fu, 0x01u, 0x02u, 0x04u, 0x08u, 0x10u, 0x1fu }  /* Z */
};

static const uint8_t s_loading_blank[7] = {
    0u, 0u, 0u, 0u, 0u, 0u, 0u
};

static const uint8_t *kage_loading_glyph(char character)
{
    if (character >= '0' && character <= '9')
        return s_loading_digits[(unsigned)(character - '0')];
    if (character >= 'A' && character <= 'Z')
        return s_loading_letters[(unsigned)(character - 'A')];
    return s_loading_blank;
}

static void kage_loading_draw_glyph(uint32_t *framebuffer,
                                    char character, unsigned x, unsigned y,
                                    unsigned scale, uint32_t color)
{
    const uint8_t *rows = kage_loading_glyph(character);
    unsigned row;

    for (row = 0u; row < 7u; ++row) {
        unsigned column = 0u;
        while (column < 5u) {
            unsigned first;
            while (column < 5u &&
                   !(rows[row] & (uint8_t)(1u << (4u - column))))
                ++column;
            first = column;
            while (column < 5u &&
                   (rows[row] & (uint8_t)(1u << (4u - column))))
                ++column;
            if (column != first)
                kage_loading_fill_rect(
                    framebuffer, x + first * scale, y + row * scale,
                    (column - first) * scale, scale, color);
        }
    }
}

static unsigned kage_loading_text_width(const char *text, unsigned scale)
{
    unsigned count = 0u;
    while (text[count])
        ++count;
    return count ? count * 6u * scale - scale : 0u;
}

static void kage_loading_draw_text(uint32_t *framebuffer, const char *text,
                                   unsigned x, unsigned y, unsigned scale,
                                   uint32_t color)
{
    unsigned index = 0u;
    while (text[index]) {
        kage_loading_draw_glyph(
            framebuffer, text[index], x + index * 6u * scale,
            y, scale, color);
        ++index;
    }
}

static void kage_loading_draw_centered(uint32_t *framebuffer,
                                       const char *text, unsigned y,
                                       unsigned scale, uint32_t color)
{
    unsigned width = kage_loading_text_width(text, scale);
    unsigned x = width < KAGE_LOADING_WIDTH
        ? (KAGE_LOADING_WIDTH - width) / 2u : 0u;
    kage_loading_draw_text(framebuffer, text, x, y, scale, color);
}

static const char *kage_loading_stage_text(unsigned stage)
{
    switch (stage) {
    case KAGE_VITA_LOADING_VERIFY: return "VERIFYING FILES";
    case KAGE_VITA_LOADING_ARCHIVES: return "READING ARCHIVES";
    case KAGE_VITA_LOADING_SHADERS: return "PREPARING SHADERS";
    case KAGE_VITA_LOADING_GAME: return "STARTING GAME";
    case KAGE_VITA_LOADING_BOOT:
    default: return "STARTING";
    }
}

static void kage_loading_display_callback(void *framebuffer_pointer)
{
    uint32_t *framebuffer = (uint32_t *)framebuffer_pointer;
    uint32_t snapshot = kage_loading_snapshot_load();
    unsigned stage;
    unsigned elapsed_seconds;
    unsigned animation_frame;
    unsigned activity_phase;
    unsigned activity_x;
    char elapsed_text[] = "ELAPSED 0000 SEC";
    unsigned digit;

    if (!(snapshot & KAGE_LOADING_SNAPSHOT_ACTIVE) || !framebuffer)
        return;
    stage = snapshot & KAGE_LOADING_SNAPSHOT_STAGE;
    elapsed_seconds = (snapshot & KAGE_LOADING_SNAPSHOT_ELAPSED) >>
        KAGE_LOADING_SNAPSHOT_ELAPSED_SHIFT;
    animation_frame = (snapshot & KAGE_LOADING_SNAPSHOT_FRAME) >>
        KAGE_LOADING_SNAPSHOT_FRAME_SHIFT;
    if (elapsed_seconds > 9999u)
        elapsed_seconds = 9999u;
    for (digit = 0u; digit < 4u; ++digit) {
        unsigned divisor = digit == 0u ? 1000u :
            (digit == 1u ? 100u : (digit == 2u ? 10u : 1u));
        unsigned value = elapsed_seconds / divisor;
        elapsed_text[8u + digit] = (char)('0' + value);
        elapsed_seconds %= divisor;
    }

    kage_loading_fill_rect(framebuffer, 0u, 0u,
                           KAGE_LOADING_WIDTH, KAGE_LOADING_HEIGHT,
                           KAGE_LOADING_BACKGROUND);
    kage_loading_fill_rect(framebuffer, 0u, 0u,
                           KAGE_LOADING_WIDTH, KAGE_LOADING_AT(8u),
                           KAGE_LOADING_ACCENT);
    kage_loading_fill_rect(framebuffer,
                           KAGE_LOADING_AT(84u), KAGE_LOADING_AT(26u),
                           KAGE_LOADING_AT(792u), KAGE_LOADING_AT(492u),
                           KAGE_LOADING_PANEL);
    kage_loading_draw_animation(
        framebuffer, animation_frame,
        KAGE_LOADING_AT(368u), KAGE_LOADING_AT(46u), KAGE_LOADING_PX(4u));
    kage_loading_draw_centered(framebuffer, "REPENTOGXM",
                               KAGE_LOADING_AT(252u), KAGE_LOADING_PX(4u),
                               KAGE_LOADING_TEXT);
    kage_loading_draw_centered(
        framebuffer, kage_loading_stage_text(stage),
        KAGE_LOADING_AT(306u), KAGE_LOADING_PX(3u),
        KAGE_LOADING_TEXT);

    kage_loading_fill_rect(framebuffer,
                           KAGE_LOADING_AT(156u), KAGE_LOADING_AT(350u),
                           KAGE_LOADING_AT(648u), KAGE_LOADING_AT(36u),
                           KAGE_LOADING_TEXT);
    kage_loading_fill_rect(framebuffer,
                           KAGE_LOADING_AT(160u), KAGE_LOADING_AT(354u),
                           KAGE_LOADING_AT(640u), KAGE_LOADING_AT(28u),
                           KAGE_LOADING_TRACK);
    activity_phase = animation_frame < 16u
        ? animation_frame : 31u - animation_frame;
    activity_x = KAGE_LOADING_AT(164u + activity_phase * 512u / 15u);
    kage_loading_fill_rect(framebuffer, activity_x, KAGE_LOADING_AT(358u),
                           KAGE_LOADING_AT(120u), KAGE_LOADING_AT(20u),
                           KAGE_LOADING_FILL);

    kage_loading_draw_centered(framebuffer, elapsed_text,
                               KAGE_LOADING_AT(416u), KAGE_LOADING_PX(2u),
                               KAGE_LOADING_MUTED);
    kage_loading_draw_centered(
        framebuffer, KAGE_LOADING_PRESENTATION_TEXT,
        KAGE_LOADING_AT(462u), KAGE_LOADING_PX(1u),
        KAGE_LOADING_MUTED);

    /* The guest renders a centered, raster-scaled 960x540 viewport.  Leave
     * every physical row outside it pure black (two above and two below at
     * 960x544, one above and two below at 720x408, one and one at 480x272)
     * so a later viewport-only first frame cannot inherit the loading
     * screen's background or accent. */
    kage_loading_fill_rect(framebuffer, 0u, 0u,
                           KAGE_LOADING_WIDTH, KAGE_LOADING_VIEWPORT_Y,
                           KAGE_LOADING_BORDER);
    kage_loading_fill_rect(framebuffer, 0u, KAGE_LOADING_VIEWPORT_END,
                           KAGE_LOADING_WIDTH,
                           KAGE_LOADING_HEIGHT - KAGE_LOADING_VIEWPORT_END,
                           KAGE_LOADING_BORDER);
}

static void kage_loading_swap(uint32_t completed_calls)
{
    (void)completed_calls;
    KAGE_VITA_STALL_TRACE_SYNC(
        KAGE_VITA_STALL_SYNC_LOADING_SWAP_ENTER, completed_calls);
    vglSwapBuffers(GL_FALSE);
    KAGE_VITA_STALL_TRACE_SYNC(
        KAGE_VITA_STALL_SYNC_LOADING_SWAP_RETURN, completed_calls);
}
#endif

void kage_vita_loading_start(void)
{
    uint64_t now;

    kage_loading_control_lock();
    if (kage_vita_loading_active()) {
        kage_loading_control_unlock();
        return;
    }

    s_owner_thread = sceKernelGetThreadId();
    now = sceKernelGetProcessTimeWide();
    s_start_time = now;
    s_last_present_time = now;
    s_stage = KAGE_VITA_LOADING_BOOT;
    s_animation_frame = 0u;
    s_loading_swaps = 0u;
    s_foreign_thread_notes = 0u;
    s_verify_notes = 0u;
    s_archive_notes = 0u;
    s_shader_notes = 0u;
    kage_loading_snapshot_store(kage_loading_snapshot(
        s_stage, s_animation_frame, 0u));
    kage_vita_io_profile_begin();
#if ISAAC_KAGE_VITA_LOADING_PRESENTATION
    vglSetDisplayCallback(kage_loading_display_callback);
    kage_loading_swap(0u);
    s_loading_swaps = 1u;
#endif
    /* STALL starts before this function.  With presentation enabled, publish
     * only after the swap really returned; with it disabled, zero is the
     * truthful completed-swap value for the whole loading lifecycle. */
    kage_loading_publish_swap_count();
    sceClibPrintf(
        "[kage-vita-loading] active: stage=boot activity=indeterminate "
        "elapsed_s=0 animation_interval_us=%u owner=%d\n",
        KAGE_LOADING_FRAME_INTERVAL_US, s_owner_thread);
    kage_loading_control_unlock();
}

static void kage_loading_note_stage(unsigned stage, uint32_t activity_token)
{
    uint64_t now;
    uint64_t elapsed;
    uint64_t elapsed_frames;
    unsigned elapsed_seconds;
    int stage_changed;
#if ISAAC_KAGE_VITA_LOADING_PRESENTATION
    int present_frame = 0;
#endif
    int current_thread = sceKernelGetThreadId();

    (void)activity_token;

    kage_loading_control_lock();
    if (!kage_vita_loading_active()) {
        kage_loading_control_unlock();
        return;
    }
    if (current_thread != s_owner_thread) {
        ++s_foreign_thread_notes;
        kage_loading_control_unlock();
        return;
    }
    if (stage < KAGE_VITA_LOADING_BOOT ||
            stage > KAGE_VITA_LOADING_SHADERS)
        stage = KAGE_VITA_LOADING_BOOT;
    if (stage == KAGE_VITA_LOADING_VERIFY)
        ++s_verify_notes;
    else if (stage == KAGE_VITA_LOADING_ARCHIVES)
        ++s_archive_notes;
    else if (stage == KAGE_VITA_LOADING_SHADERS)
        ++s_shader_notes;
    stage_changed = stage != s_stage;
    s_stage = stage;

    /* All activity notes remain on the vitaGL owner thread.  Advance against
     * an anchored wall clock and issue at most one swap per note: a stalled
     * load catches up its animation without a burst of queued presents. */
    now = sceKernelGetProcessTimeWide();
    elapsed_seconds = kage_loading_elapsed_at(now);
    if (stage_changed && stage == KAGE_VITA_LOADING_SHADERS) {
        s_last_present_time = now;
#if ISAAC_KAGE_VITA_LOADING_PRESENTATION
        present_frame = 1;
#endif
    } else if (now < s_last_present_time) {
        s_last_present_time = now;
    } else {
        elapsed = now - s_last_present_time;
        if (elapsed >= KAGE_LOADING_FRAME_INTERVAL_US) {
            elapsed_frames = elapsed / KAGE_LOADING_FRAME_INTERVAL_US;
            s_last_present_time = now -
                elapsed % KAGE_LOADING_FRAME_INTERVAL_US;
            s_animation_frame = (s_animation_frame +
                (unsigned)(elapsed_frames %
                    KAGE_LOADING_ANIMATION_FRAME_COUNT)) %
                KAGE_LOADING_ANIMATION_FRAME_COUNT;
#if ISAAC_KAGE_VITA_LOADING_PRESENTATION
            present_frame = 1;
#endif
        }
    }

    kage_loading_snapshot_store(kage_loading_snapshot(
        s_stage, s_animation_frame, elapsed_seconds));
#if ISAAC_KAGE_VITA_LOADING_PRESENTATION
    if (present_frame) {
        kage_loading_swap(activity_token);
        ++s_loading_swaps;
    }
#endif
    /* Only the loading owner writes this plain counter.  Publish its value
     * atomically for the watchdog instead of making that thread read it or
     * acquire the presenter lock.  A swap which never returns is therefore
     * intentionally absent from the completed count. */
    kage_loading_publish_swap_count();
    kage_loading_control_unlock();
}

void kage_vita_loading_note_verify(void)
{
    kage_loading_note_stage(KAGE_VITA_LOADING_VERIFY, 0u);
}

void kage_vita_loading_note_fread(uint32_t completed_calls)
{
    kage_loading_note_stage(KAGE_VITA_LOADING_ARCHIVES, completed_calls);
}

void kage_vita_loading_note_shader(void)
{
    kage_loading_note_stage(KAGE_VITA_LOADING_SHADERS, 0u);
}

void kage_vita_loading_finish(void)
{
    unsigned previous_stage;
    unsigned elapsed_seconds;
    unsigned swaps;
    unsigned foreign_notes;
    unsigned verify_notes;
    unsigned archive_notes;
    unsigned shader_notes;
    int owner_thread;
    int finish_thread;
#if ISAAC_KAGE_VITA_LOADING_PRESENTATION
    int queue_finish_result;
#endif

    kage_loading_control_lock();
    if (!kage_vita_loading_active()) {
        kage_loading_control_unlock();
        return;
    }
    previous_stage = s_stage;
    elapsed_seconds = kage_loading_elapsed_at(
        sceKernelGetProcessTimeWide());
    s_stage = KAGE_VITA_LOADING_GAME;
    swaps = s_loading_swaps;
    foreign_notes = s_foreign_thread_notes;
    verify_notes = s_verify_notes;
    archive_notes = s_archive_notes;
    shader_notes = s_shader_notes;
    owner_thread = s_owner_thread;
    finish_thread = sceKernelGetThreadId();

    /* Publish the terminal stage, then keep the lifecycle active and retain
     * the control lock until the callback is removed and every queued or
     * in-flight callback is drained.  A serial caller can therefore trust
     * active()==false as the completed-handoff boundary, not merely as a
     * request to finish.  The callback itself is lock-free, so retaining this
     * lock across the queue drain cannot deadlock it.  The disabled A/B path
     * has installed and queued nothing and deliberately performs no display
     * operation here. */
    kage_loading_snapshot_store(kage_loading_snapshot(
        KAGE_VITA_LOADING_GAME, s_animation_frame, elapsed_seconds));
#if ISAAC_KAGE_VITA_LOADING_PRESENTATION
    vglSetDisplayCallback(NULL);
    queue_finish_result = sceGxmDisplayQueueFinish();
#endif
    kage_loading_snapshot_store(0u);
    kage_loading_control_unlock();
#if ISAAC_KAGE_VITA_LOADING_PRESENTATION
    sceClibPrintf(
        "[kage-vita-loading] activity: verify_notes=%u archive_notes=%u "
        "shader_notes=%u\n",
        verify_notes, archive_notes, shader_notes);
    sceClibPrintf(
        "[kage-vita-loading] finished: stage=game previous_stage=%u "
        "elapsed_s=%u loading_swaps=%u "
        "owner=%d finish=%d owner_match=%u foreign_notes=%u "
        "queue_finish=0x%08x\n",
        previous_stage, elapsed_seconds, swaps, owner_thread, finish_thread,
        (unsigned)(owner_thread == finish_thread), foreign_notes,
        (unsigned)queue_finish_result);
#else
    sceClibPrintf(
        "[kage-vita-loading] activity: verify_notes=%u archive_notes=%u "
        "shader_notes=%u\n",
        verify_notes, archive_notes, shader_notes);
    sceClibPrintf(
        "[kage-vita-loading] finished: stage=game previous_stage=%u "
        "elapsed_s=%u loading_swaps=%u "
        "owner=%d finish=%d owner_match=%u foreign_notes=%u "
        "queue_finish=skipped\n",
        previous_stage, elapsed_seconds, swaps, owner_thread, finish_thread,
        (unsigned)(owner_thread == finish_thread), foreign_notes);
#endif
    kage_vita_io_profile_report("loading-complete");
#if defined(ISAAC_VITA_CRT_SEEK_SHADOW)
    isaac_vita_crt_seek_shadow_report("loading-complete");
#endif
}

int kage_vita_loading_active(void)
{
    return (kage_loading_snapshot_load() &
            KAGE_LOADING_SNAPSHOT_ACTIVE) != 0u;
}

unsigned kage_vita_loading_stage(void)
{
    return kage_loading_snapshot_load() &
           KAGE_LOADING_SNAPSHOT_STAGE;
}

unsigned kage_vita_loading_elapsed_seconds(void)
{
    return (kage_loading_snapshot_load() &
            KAGE_LOADING_SNAPSHOT_ELAPSED) >>
           KAGE_LOADING_SNAPSHOT_ELAPSED_SHIFT;
}

#if defined(ISAAC_KAGE_VITA_LOADING_ORACLE)
unsigned kage_vita_loading_oracle_animation_frame(void)
{
    return (kage_loading_snapshot_load() &
            KAGE_LOADING_SNAPSHOT_FRAME) >>
           KAGE_LOADING_SNAPSHOT_FRAME_SHIFT;
}
#endif

unsigned kage_vita_loading_swap_count(void)
{
    return s_loading_swaps;
}
