/*
 * Minimal native front-end for the LiveArea manager entry.
 *
 * The direct CDRAM framebuffer setup intentionally follows VitaSDK's working
 * debug-screen sample rather than bringing the game's graphics stack into this
 * tiny process:
 * https://github.com/vitasdk/samples/blob/fe8fbef570f3280586c0c20157146e3faefb2181/common/debugScreen.c#L398-L415
 *
 * Unlike that sample, nothing draws into the buffer the display is scanning.
 * Every draw call targets a RAM canvas; manager_present copies a finished
 * frame into the CDRAM buffer that is not on screen and flips with
 * SCE_DISPLAY_SETBUF_NEXTFRAME.  Drawing straight into the live buffer raced
 * the beam: the sub-pages were redrawn every frame, so the rows above the
 * point where the panel and text writes overtook the scan-out showed the
 * freshly cleared background and the header never landed before the beam
 * had passed (issue #1 photos: "half of the screen" missing).
 */

#include "manager_ftp_server.h"
#include "manager_storage.h"
#include "manager_storage_vita.h"
#if defined(MANAGER_SETTINGS_UI)
#include "manager_settings_ui.h"
#endif
#if defined(MANAGER_UI2)
#include "manager_assets_vita.h"
#include "manager_ui.h"
#if defined(MANAGER_UI2_GAME_SKIN)
#include "manager_blob.h"
#include "manager_skin_game.h"
#endif
#endif

#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>
#if defined(MANAGER_UI2)
#include <psp2/apputil.h>
#include <psp2/system_param.h>
#if defined(MANAGER_UI2_GAME_SKIN)
#include <psp2/kernel/processmgr.h>
#endif
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MANAGER_WIDTH 960u
#define MANAGER_HEIGHT 544u
#define MANAGER_PITCH 960u
#define MANAGER_FRAMEBUFFER_BYTES (2u * 1024u * 1024u)
#define MANAGER_DISPLAY_BUFFERS 2u
#define MANAGER_CANVAS_PIXELS (MANAGER_PITCH * MANAGER_HEIGHT)
#define MANAGER_MAX_MODS 96u
#define MANAGER_MAX_BACKUPS 96u
#define MANAGER_MOD_ROWS 7u
#define MANAGER_BACKUP_ROWS 5u

/* Draw target (.bss, never scanned out); see the header comment. */
static uint32_t manager_canvas[MANAGER_CANVAS_PIXELS];
/* Two CDRAM display buffers inside one memblock; the one at
 * manager_display_back is off screen and receives the next present. */
static uint32_t *manager_display_buffers[MANAGER_DISPLAY_BUFFERS];
static unsigned int manager_display_back;
static int manager_canvas_dirty;
#if !defined(MANAGER_UI2)
static const struct manager_storage_io *manager_storage_io;
static struct manager_mod_entry manager_mods[MANAGER_MAX_MODS];
static struct manager_save_slot manager_slots[3];
static struct manager_save_backup manager_backups[MANAGER_MAX_BACKUPS];
static size_t manager_mod_count;
static size_t manager_mod_rejected;
static size_t manager_mod_selection;
static size_t manager_backup_count;
static size_t manager_backup_rejected;
static size_t manager_backup_selection;
static int manager_backup_slot = 1;
static int manager_restore_armed = -1;
static char manager_status[96] = "READY";

enum manager_page {
    MANAGER_PAGE_HOME = 0,
    MANAGER_PAGE_MODS,
    MANAGER_PAGE_SAVES,
    MANAGER_PAGE_CONTROLS,
#if defined(MANAGER_SETTINGS_UI)
    MANAGER_PAGE_SETTINGS,
#endif
};
#endif /* !MANAGER_UI2: the pre-UI2 page state */
#if defined(MANAGER_SETTINGS_UI)
/* ISAAC SETTINGS page model (manager_settings_ui.c).  Rows are drawn from
 * y=166 at a 30 px pitch inside the 116..460 panel, so at most ten fit. */
#define MANAGER_SETTINGS_VISIBLE_ROWS 10u
#if !defined(MANAGER_UI2)
static struct manager_settings_ui manager_settings_page;
#endif
/* The model's button bits are the SDK values, so the pressed-edge mask is
 * handed over unchanged.  Both sides are enumerators of different enum
 * types (the SDK's SceCtrlButtons and the model's); compared as unsigned so
 * the ARM gcc -Wenum-compare stays silent (the host stand-in SDK header
 * spells the SCE_CTRL_* values as plain integers and never warned). */
#define MANAGER_SETTINGS_BUTTON_IS(model, sdk) \
    ((unsigned)(model) == (unsigned)(sdk))
_Static_assert(MANAGER_SETTINGS_BUTTON_IS(MANAGER_SETTINGS_UI_BUTTON_UP, SCE_CTRL_UP) &&
                   MANAGER_SETTINGS_BUTTON_IS(MANAGER_SETTINGS_UI_BUTTON_DOWN, SCE_CTRL_DOWN) &&
                   MANAGER_SETTINGS_BUTTON_IS(MANAGER_SETTINGS_UI_BUTTON_LEFT, SCE_CTRL_LEFT) &&
                   MANAGER_SETTINGS_BUTTON_IS(MANAGER_SETTINGS_UI_BUTTON_RIGHT, SCE_CTRL_RIGHT) &&
                   MANAGER_SETTINGS_BUTTON_IS(MANAGER_SETTINGS_UI_BUTTON_CROSS, SCE_CTRL_CROSS) &&
                   MANAGER_SETTINGS_BUTTON_IS(MANAGER_SETTINGS_UI_BUTTON_CIRCLE, SCE_CTRL_CIRCLE) &&
                   MANAGER_SETTINGS_BUTTON_IS(MANAGER_SETTINGS_UI_BUTTON_TRIANGLE, SCE_CTRL_TRIANGLE) &&
                   MANAGER_SETTINGS_BUTTON_IS(MANAGER_SETTINGS_UI_BUTTON_SQUARE, SCE_CTRL_SQUARE) &&
                   MANAGER_SETTINGS_BUTTON_IS(MANAGER_SETTINGS_UI_BUTTON_START, SCE_CTRL_START) &&
                   MANAGER_SETTINGS_BUTTON_IS(MANAGER_SETTINGS_UI_BUTTON_SELECT, SCE_CTRL_SELECT),
               "manager_settings_ui button bits must equal SCE_CTRL_*");
#undef MANAGER_SETTINGS_BUTTON_IS
#endif

#if !defined(MANAGER_UI2)
/* Compact 5x7 uppercase font. Bit 4 is the leftmost pixel. */
static const uint8_t manager_letters[26][7] = {
    {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}, /* A */
    {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e}, /* B */
    {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}, /* C */
    {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e}, /* D */
    {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}, /* E */
    {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}, /* F */
    {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f}, /* G */
    {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}, /* H */
    {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e}, /* I */
    {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0c}, /* J */
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, /* K */
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f}, /* L */
    {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}, /* M */
    {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}, /* N */
    {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}, /* O */
    {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}, /* P */
    {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d}, /* Q */
    {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}, /* R */
    {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}, /* S */
    {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, /* T */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}, /* U */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04}, /* V */
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a}, /* W */
    {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11}, /* X */
    {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04}, /* Y */
    {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f}, /* Z */
};

static const uint8_t manager_digits[10][7] = {
    {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}, /* 0 */
    {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}, /* 1 */
    {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}, /* 2 */
    {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e}, /* 3 */
    {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}, /* 4 */
    {0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e}, /* 5 */
    {0x0e, 0x10, 0x10, 0x1e, 0x11, 0x11, 0x0e}, /* 6 */
    {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, /* 7 */
    {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}, /* 8 */
    {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x01, 0x0e}, /* 9 */
};

static uint8_t manager_glyph_row(char character, unsigned int row)
{
    if (character >= 'A' && character <= 'Z')
        return manager_letters[(unsigned int)(character - 'A')][row];
    if (character >= '0' && character <= '9')
        return manager_digits[(unsigned int)(character - '0')][row];
    if (character == ':') {
        static const uint8_t colon[7] = {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00};
        return colon[row];
    }
    if (character == '.') {
        static const uint8_t period[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c};
        return period[row];
    }
    if (character == '-') {
        static const uint8_t dash[7] = {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00};
        return dash[row];
    }
    if (character == '_') {
        static const uint8_t underscore[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f};
        return underscore[row];
    }
    if (character == '>') {
        static const uint8_t arrow[7] = {0x10, 0x08, 0x04, 0x02, 0x04, 0x08, 0x10};
        return arrow[row];
    }
    if (character == '<') {
        static const uint8_t arrow[7] = {0x01, 0x02, 0x04, 0x08, 0x04, 0x02, 0x01};
        return arrow[row];
    }
    if (character == '?') {
        static const uint8_t question[7] = {0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04};
        return question[row];
    }
    return 0;
}

static void manager_fill_rect(unsigned int x, unsigned int y,
                              unsigned int width, unsigned int height,
                              uint32_t color)
{
    unsigned int row;
    unsigned int column;

    if (x >= MANAGER_WIDTH || y >= MANAGER_HEIGHT)
        return;
    if (width > MANAGER_WIDTH - x)
        width = MANAGER_WIDTH - x;
    if (height > MANAGER_HEIGHT - y)
        height = MANAGER_HEIGHT - y;

    manager_canvas_dirty = 1;
    for (row = 0; row < height; ++row) {
        uint32_t *pixel = manager_canvas + (y + row) * MANAGER_PITCH + x;
        for (column = 0; column < width; ++column)
            pixel[column] = color;
    }
}
#endif /* !MANAGER_UI2: 5x7 font and fill (manager_font.c has the copy) */

/* Show the canvas when it changed since the last present, otherwise just
 * pace the loop on the vblank.  The copy goes to the off-screen buffer; the
 * flip is queued for the next vblank and waited for, so the buffer that was
 * on screen is free for the following present. */
static int manager_present(void)
{
    SceDisplayFrameBuf frame;
    int result;

    if (!manager_canvas_dirty) {
        sceDisplayWaitVblankStart();
        return 0;
    }
    memcpy(manager_display_buffers[manager_display_back], manager_canvas,
           sizeof(manager_canvas));
    memset(&frame, 0, sizeof(frame));
    frame.size = sizeof(frame);
    frame.base = manager_display_buffers[manager_display_back];
    frame.pitch = MANAGER_PITCH;
    frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    frame.width = MANAGER_WIDTH;
    frame.height = MANAGER_HEIGHT;
    result = sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
    if (result < 0)
        return result;
    manager_display_back =
        (manager_display_back + 1u) % MANAGER_DISPLAY_BUFFERS;
    manager_canvas_dirty = 0;
    sceDisplayWaitSetFrameBuf();
    return 0;
}

#if !defined(MANAGER_UI2)
static void manager_draw_text(unsigned int x, unsigned int y,
                              unsigned int scale, uint32_t color,
                              const char *text)
{
    const unsigned int origin_x = x;

    while (*text != '\0') {
        unsigned int row;
        unsigned int column;
        uint8_t bits;

        if (*text == '\n') {
            x = origin_x;
            y += 8u * scale;
            ++text;
            continue;
        }

        for (row = 0; row < 7u; ++row) {
            bits = manager_glyph_row(*text, row);
            for (column = 0; column < 5u; ++column) {
                if ((bits & (uint8_t)(0x10u >> column)) != 0u) {
                    manager_fill_rect(x + column * scale, y + row * scale,
                                      scale, scale, color);
                }
            }
        }
        x += 6u * scale;
        ++text;
    }
}

static void manager_display_text(char *output, size_t capacity,
                                 const char *input, size_t max_characters)
{
    size_t used = 0u;
    while (*input != '\0' && used + 1u < capacity && used < max_characters) {
        const unsigned char value = (unsigned char)*input++;
        if (value >= 'a' && value <= 'z')
            output[used++] = (char)(value - 'a' + 'A');
        else if ((value >= 'A' && value <= 'Z') ||
                 (value >= '0' && value <= '9') || value == ' ' ||
                 value == '.' || value == '-' || value == '_')
            output[used++] = (char)value;
        else
            output[used++] = '?';
    }
    output[used] = '\0';
}

static void manager_draw_frame(const char *title)
{
    const uint32_t background = 0xff171717u;
    const uint32_t panel = 0xff252525u;
    const uint32_t accent = 0xff3030a0u;
    const uint32_t white = 0xffeeeeeeu;

    manager_fill_rect(0u, 0u, MANAGER_WIDTH, MANAGER_HEIGHT, background);
    manager_fill_rect(0u, 0u, MANAGER_WIDTH, 104u, accent);
    manager_fill_rect(64u, 116u, 832u, 344u, panel);
    manager_fill_rect(0u, 472u, MANAGER_WIDTH, 72u, accent);
    manager_draw_text(72u, 34u, 4u, white, title);
}

static void manager_draw_home(int ftp_listening, const char *ftp_ip,
                              unsigned short ftp_port,
                              int ftp_start_failed)
{
    const uint32_t white = 0xffeeeeeeu;
    const uint32_t muted = 0xffa8a8a8u;
    char endpoint[64];
    char status[72];

    manager_draw_frame("ISAAC MANAGER");
    manager_draw_text(96u, 140u, 2u, white,
                      "STATUS: NATIVE MANAGER RUNNING");
    manager_draw_text(96u, 188u, 2u, ftp_listening ? white : muted,
                      ftp_listening ? "FTP: LISTENING" : "FTP: STOPPED");
    if (ftp_listening) {
        (void)snprintf(endpoint, sizeof(endpoint), "IP: %s:%u",
                       ftp_ip, (unsigned int)ftp_port);
    } else if (ftp_start_failed) {
        (void)snprintf(endpoint, sizeof(endpoint), "IP: START FAILED");
    } else {
        (void)snprintf(endpoint, sizeof(endpoint), "IP: NOT LISTENING");
    }
    manager_draw_text(96u, 224u, 2u, ftp_listening ? white : muted, endpoint);
    manager_draw_text(96u, 270u, 2u, muted, "ROOT MAP: ISAAC DATA");
    manager_draw_text(96u, 292u, 2u, muted, "TRIANGLE: MODS");
    manager_draw_text(96u, 326u, 2u, muted, "SQUARE: SAVES AND BACKUPS");
    manager_draw_text(96u, 360u, 2u, muted, "SELECT: GAME CONTROLS");
    manager_display_text(status, sizeof(status), manager_status, 58u);
    manager_draw_text(96u, 406u, 2u, white, status);
#if defined(MANAGER_SETTINGS_UI)
    manager_draw_text(96u, 436u, 2u, muted, "START: SETTINGS");
#endif
    manager_draw_text(72u, 492u, 2u, white,
                      "O  TOGGLE FTP     X  RETURN TO GAME");
}

static void manager_draw_mods(void)
{
    const uint32_t white = 0xffeeeeeeu;
    const uint32_t muted = 0xffa8a8a8u;
    const uint32_t selected = 0xff80e0ffu;
    const size_t first = manager_mod_selection / MANAGER_MOD_ROWS *
                         MANAGER_MOD_ROWS;
    char line[80];
    char name[52];
    char status[72];
    size_t row;

    manager_draw_frame("ISAAC MODS");
    manager_display_text(status, sizeof(status), manager_status, 58u);
    manager_draw_text(88u, 126u, 2u, white, status);
    if (manager_mod_count == 0u) {
        manager_draw_text(88u, 184u, 2u, muted, "NO MOD DIRECTORIES FOUND");
    } else {
        (void)snprintf(line, sizeof(line), "MOD %u OF %u  SKIPPED %u",
                       (unsigned int)(manager_mod_selection + 1u),
                       (unsigned int)manager_mod_count,
                       (unsigned int)manager_mod_rejected);
        manager_draw_text(88u, 166u, 2u, muted, line);
        for (row = 0u; row < MANAGER_MOD_ROWS; ++row) {
            const size_t index = first + row;
            if (index >= manager_mod_count)
                break;
            manager_display_text(name, sizeof(name), manager_mods[index].name,
                                 45u);
            (void)snprintf(line, sizeof(line), "%c %s  %s",
                           index == manager_mod_selection ? '>' : ' ',
                           manager_mods[index].disabled ? "OFF" : "ON ", name);
            manager_draw_text(88u, 204u + (unsigned int)row * 34u, 2u,
                              index == manager_mod_selection ? selected : white,
                              line);
        }
    }
    manager_draw_text(72u, 492u, 2u, white,
                      "UP DOWN  SELECT     X  TOGGLE     O  BACK");
}

static void manager_draw_saves(void)
{
    const uint32_t white = 0xffeeeeeeu;
    const uint32_t muted = 0xffa8a8a8u;
    const uint32_t selected = 0xff80e0ffu;
    const size_t first = manager_backup_selection / MANAGER_BACKUP_ROWS *
                         MANAGER_BACKUP_ROWS;
    char line[80];
    char label[54];
    char status[72];
    size_t row;
    int slot;

    manager_draw_frame("ISAAC SAVES");
    manager_display_text(status, sizeof(status), manager_status, 58u);
    manager_draw_text(88u, 122u, 2u, white, status);
    for (slot = 0; slot < 3; ++slot) {
        if (manager_slots[slot].present) {
#if defined(ISAAC_VITA_MANAGER_SAVE_GAMESTATE)
            if (manager_slots[slot].gamestate_present) {
                (void)snprintf(
                    line, sizeof(line), "SLOT %d: %lu BYTES  RUN %lu BYTES%s",
                    slot + 1, (unsigned long)manager_slots[slot].size,
                    (unsigned long)manager_slots[slot].gamestate_size,
                    manager_backup_slot == slot + 1 ? "  <" : "");
            } else {
                (void)snprintf(line, sizeof(line),
                               "SLOT %d: %lu BYTES  NO RUN%s", slot + 1,
                               (unsigned long)manager_slots[slot].size,
                               manager_backup_slot == slot + 1 ? "  <" : "");
            }
#else
            (void)snprintf(line, sizeof(line), "SLOT %d: %lu BYTES%s",
                           slot + 1, (unsigned long)manager_slots[slot].size,
                           manager_backup_slot == slot + 1 ? "  <" : "");
#endif
        } else {
            (void)snprintf(line, sizeof(line), "SLOT %d: MISSING%s", slot + 1,
                           manager_backup_slot == slot + 1 ? "  <" : "");
        }
        manager_draw_text(88u, 158u + (unsigned int)slot * 30u, 2u,
                          manager_backup_slot == slot + 1 ? selected : muted,
                          line);
    }
    if (manager_backup_count == 0u) {
        manager_draw_text(88u, 272u, 2u, muted, "NO RESTORABLE BACKUPS FOUND");
    } else {
        (void)snprintf(line, sizeof(line), "BACKUP %u OF %u  SKIPPED %u",
                       (unsigned int)(manager_backup_selection + 1u),
                       (unsigned int)manager_backup_count,
                       (unsigned int)manager_backup_rejected);
        manager_draw_text(88u, 258u, 2u, muted, line);
        for (row = 0u; row < MANAGER_BACKUP_ROWS; ++row) {
            const size_t index = first + row;
            if (index >= manager_backup_count)
                break;
            manager_display_text(label, sizeof(label),
                                 manager_backups[index].label, 47u);
            (void)snprintf(line, sizeof(line), "%c %s",
                           index == manager_backup_selection ? '>' : ' ', label);
            manager_draw_text(88u, 292u + (unsigned int)row * 30u, 2u,
                              index == manager_backup_selection ? selected
                                                                : white,
                              line);
        }
    }
    manager_draw_text(48u, 488u, 2u, white,
                      "LEFT RIGHT SLOT  SQUARE BACKUP  X RESTORE  O BACK");
}

static void manager_draw_controls(void)
{
    const uint32_t white = 0xffeeeeeeu;
    const uint32_t muted = 0xffa8a8a8u;

    manager_draw_frame("ISAAC CONTROLS");
    manager_draw_text(88u, 130u, 2u, white,
                      "R OR TOP LEFT TOUCH: USE ACTIVE");
    manager_draw_text(88u, 166u, 2u, white,
                      "TOP RIGHT TOUCH: HOLD MAP");
    manager_draw_text(88u, 202u, 2u, white,
                      "BOTTOM RIGHT TOUCH: USE POCKET");
    manager_draw_text(88u, 248u, 2u, white,
                      "SELECT TAP: SWAP HELD SLOTS");
    manager_draw_text(88u, 284u, 2u, white,
                      "SELECT HOLD: DROP POCKET OR TRINKET");
    manager_draw_text(88u, 330u, 2u, muted, "L: BOMB");
    manager_draw_text(88u, 366u, 2u, muted,
                      "START: PAUSE  START+L: POCKET");
    manager_draw_text(88u, 402u, 2u, muted,
                      "START+R: HOLD MAP");
    manager_draw_text(72u, 492u, 2u, white, "O  BACK");
    manager_draw_text(300u, 492u, 2u, white,
                      "ISAAC DECIDES SWAP CONTEXT");
}

#if defined(MANAGER_SETTINGS_UI)
/* Status through manager_display_text (narrow set), rows and legend as
 * literals (wide set); the model guarantees both sets and the widths. */
static void manager_draw_settings(void)
{
    const uint32_t white = 0xffeeeeeeu;
    const uint32_t muted = 0xffa8a8a8u;
    const uint32_t selected = 0xff80e0ffu;
    char status[72];
    unsigned int row;

    manager_draw_frame(manager_settings_ui_title());
    manager_display_text(status, sizeof(status), manager_status, 58u);
    manager_draw_text(88u, 126u, 2u, white, status);
    for (row = 0u; row < manager_settings_page.row_count &&
                   row < MANAGER_SETTINGS_VISIBLE_ROWS;
         ++row) {
        const uint8_t style = manager_settings_page.row_style[row];
        uint32_t color = muted;
        if (style == (uint8_t)MANAGER_SETTINGS_UI_STYLE_SELECTED)
            color = selected;
        else if (style == (uint8_t)MANAGER_SETTINGS_UI_STYLE_ITEM)
            color = white;
        manager_draw_text(88u, 166u + row * 30u, 2u, color,
                          manager_settings_page.rows[row]);
    }
    manager_draw_text(72u, 492u, 2u, white, manager_settings_ui_legend());
}
#endif
static void manager_set_result(const char *action, int result)
{
    const char *suffix;
    if (result == MANAGER_STORAGE_OK)
        suffix = "OK";
    else if (result == MANAGER_STORAGE_SYNC_WARNING)
        suffix = "OK - SYNC WARNING";
    else if (result == MANAGER_STORAGE_HASH_MISMATCH)
        suffix = "FAILED - HASH MISMATCH";
    else if (result == MANAGER_STORAGE_CONFLICT)
        suffix = "FAILED - FILE CONFLICT";
    else if (result == MANAGER_STORAGE_TOO_LARGE)
        suffix = "FAILED - SAVE TOO LARGE";
    else
        suffix = "FAILED - IO ERROR";
    (void)snprintf(manager_status, sizeof(manager_status), "%s: %s", action,
                   suffix);
}
#if defined(ISAAC_VITA_MANAGER_SAVE_GAMESTATE)
/* Appends what happened to the slot's run state; only on success, the failure
 * text already says the slot is unchanged. */
static void manager_set_pair_result(const char *action, int result,
                                    const char *run_state)
{
    size_t used;
    manager_set_result(action, result);
    if (result < 0)
        return;
    used = strlen(manager_status);
    if (used < sizeof(manager_status))
        (void)snprintf(manager_status + used, sizeof(manager_status) - used,
                       " - %s", run_state);
}
#endif

static int manager_refresh_mods(void)
{
    int result;
    manager_mod_count = 0u;
    manager_mod_rejected = 0u;
    result = manager_storage_list_mods(
        manager_storage_io, manager_mods, MANAGER_MAX_MODS, &manager_mod_count,
        &manager_mod_rejected);
    if (manager_mod_count == 0u)
        manager_mod_selection = 0u;
    else if (manager_mod_selection >= manager_mod_count)
        manager_mod_selection = manager_mod_count - 1u;
    if (result < 0)
        manager_set_result("MOD SCAN", result);
    return result;
}

static int manager_refresh_saves(void)
{
    int result;
    memset(manager_slots, 0, sizeof(manager_slots));
    manager_backup_count = 0u;
    manager_backup_rejected = 0u;
    result = manager_storage_get_save_slots(manager_storage_io,
                                            manager_slots);
    if (result >= 0) {
        result = manager_storage_list_save_backups(
            manager_storage_io, manager_backups, MANAGER_MAX_BACKUPS,
            &manager_backup_count, &manager_backup_rejected);
    }
    if (manager_backup_count == 0u)
        manager_backup_selection = 0u;
    else if (manager_backup_selection >= manager_backup_count)
        manager_backup_selection = manager_backup_count - 1u;
    if (result < 0)
        manager_set_result("SAVE SCAN", result);
    return result;
}

static void manager_draw_page(enum manager_page page, int ftp_listening,
                              const char *ftp_ip, unsigned short ftp_port,
                              int ftp_start_failed)
{
    if (page == MANAGER_PAGE_MODS)
        manager_draw_mods();
    else if (page == MANAGER_PAGE_SAVES)
        manager_draw_saves();
    else if (page == MANAGER_PAGE_CONTROLS)
        manager_draw_controls();
#if defined(MANAGER_SETTINGS_UI)
    else if (page == MANAGER_PAGE_SETTINGS)
        manager_draw_settings();
#endif
    else
        manager_draw_home(ftp_listening, ftp_ip, ftp_port, ftp_start_failed);
}

static void manager_draw_return_failure(void)
{
    const uint32_t panel = 0xff252525u;
    const uint32_t warning = 0xff9090ffu;

    manager_fill_rect(64u, 132u, 832u, 58u, panel);
    manager_draw_text(96u, 148u, 2u, warning,
                      "STATUS: RETURN FAILED  PRESS X TO RETRY");
}

int main(void)
{
    SceUID framebuffer_block;
    uint32_t *display_memory;
    unsigned int buffer;
    uint32_t previous_buttons = 0u;
    char ftp_ip[16] = "";
    unsigned short ftp_port = 0u;
    int ftp_start_failed = 0;
    enum manager_page page = MANAGER_PAGE_HOME;
    int result;

    framebuffer_block = sceKernelAllocMemBlock(
        "isaac_manager_display", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
        MANAGER_FRAMEBUFFER_BYTES * MANAGER_DISPLAY_BUFFERS, NULL);
    if (framebuffer_block < 0) {
        manager_ftp_server_stop();
        return 2;
    }
    if (sceKernelGetMemBlockBase(framebuffer_block,
                                 (void **)&display_memory) < 0) {
        manager_ftp_server_stop();
        sceKernelFreeMemBlock(framebuffer_block);
        return 3;
    }
    for (buffer = 0u; buffer < MANAGER_DISPLAY_BUFFERS; ++buffer)
        manager_display_buffers[buffer] =
            display_memory + buffer * (MANAGER_FRAMEBUFFER_BYTES / 4u);

    manager_storage_io = manager_storage_vita_io();
    result = manager_storage_recover(manager_storage_io);
    if (result < 0)
        manager_set_result("RECOVERY", result);
    else if (result == MANAGER_STORAGE_SYNC_WARNING)
        manager_set_result("RECOVERY", result);
    else
        (void)snprintf(manager_status, sizeof(manager_status), "READY");
    manager_draw_home(0, ftp_ip, ftp_port, 0);
    result = manager_present();
    if (result < 0) {
        manager_ftp_server_stop();
        sceKernelFreeMemBlock(framebuffer_block);
        return 4;
    }

    (void)sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
    for (;;) {
        SceCtrlData pad;
        uint32_t pressed;

        memset(&pad, 0, sizeof(pad));
        if (ftp_port != 0u && !manager_ftp_server_is_listening()) {
            manager_ftp_server_stop();
            ftp_port = 0u;
            ftp_ip[0] = '\0';
            ftp_start_failed = 1;
            (void)snprintf(manager_status, sizeof(manager_status),
                           "FTP STOPPED AFTER SERVER ERROR");
            manager_draw_home(0, ftp_ip, ftp_port, ftp_start_failed);
        }
        if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0) {
            pressed = pad.buttons & ~previous_buttons;
            previous_buttons = pad.buttons;
            if (page == MANAGER_PAGE_HOME) {
                if ((pressed & SCE_CTRL_CIRCLE) != 0u) {
                    if (ftp_port != 0u) {
                        manager_ftp_server_stop();
                        ftp_port = 0u;
                        ftp_ip[0] = '\0';
                        ftp_start_failed = 0;
                        (void)snprintf(manager_status, sizeof(manager_status),
                                       "FTP STOPPED");
                    } else {
                        result = manager_ftp_server_start(
                            ftp_ip, sizeof(ftp_ip), &ftp_port);
                        if (result < 0) {
                            manager_ftp_server_stop();
                            ftp_port = 0u;
                            ftp_ip[0] = '\0';
                            ftp_start_failed = 1;
                            (void)snprintf(manager_status,
                                           sizeof(manager_status),
                                           "FTP START FAILED");
                        } else {
                            ftp_start_failed = 0;
                            (void)snprintf(manager_status,
                                           sizeof(manager_status),
                                           "FTP LISTENING - LAN ACCESS OPEN");
                        }
                    }
                    manager_draw_home(ftp_port != 0u, ftp_ip, ftp_port,
                                      ftp_start_failed);
                } else if ((pressed & SCE_CTRL_TRIANGLE) != 0u) {
                    if (ftp_port != 0u) {
                        (void)snprintf(manager_status, sizeof(manager_status),
                                       "STOP FTP BEFORE LOCAL CHANGES");
                        manager_draw_home(1, ftp_ip, ftp_port,
                                          ftp_start_failed);
                    } else {
                        (void)snprintf(manager_status, sizeof(manager_status),
                                       "X TOGGLES SELECTED MOD");
                        (void)manager_refresh_mods();
                        page = MANAGER_PAGE_MODS;
                        manager_draw_mods();
                    }
                } else if ((pressed & SCE_CTRL_SQUARE) != 0u) {
                    if (ftp_port != 0u) {
                        (void)snprintf(manager_status, sizeof(manager_status),
                                       "STOP FTP BEFORE LOCAL CHANGES");
                        manager_draw_home(1, ftp_ip, ftp_port,
                                          ftp_start_failed);
                    } else {
                        (void)snprintf(manager_status, sizeof(manager_status),
                                       "SELECT BACKUP OR CREATE ONE");
                        manager_restore_armed = -1;
                        (void)manager_refresh_saves();
                        page = MANAGER_PAGE_SAVES;
                        manager_draw_saves();
                    }
                } else if ((pressed & SCE_CTRL_SELECT) != 0u) {
                    page = MANAGER_PAGE_CONTROLS;
                    manager_draw_controls();
#if defined(MANAGER_SETTINGS_UI)
                } else if ((pressed & SCE_CTRL_START) != 0u) {
                    /* Viewing is allowed while FTP listens; every write of
                     * the page is refused until it is stopped. */
                    manager_settings_ui_open(&manager_settings_page,
                                             manager_storage_io);
                    (void)snprintf(manager_status, sizeof(manager_status),
                                   "%s", manager_settings_page.status);
                    page = MANAGER_PAGE_SETTINGS;
                    manager_draw_settings();
#endif
                } else if ((pressed & SCE_CTRL_CROSS) != 0u) {
                    if (ftp_port != 0u) {
                        manager_fill_rect(64u, 180u, 832u, 96u, 0xff252525u);
                        manager_draw_text(96u, 192u, 2u, 0xffa8a8a8u,
                                          "FTP: STOPPING");
                        (void)manager_present();
                    }
                    manager_ftp_server_stop();
                    ftp_port = 0u;
                    ftp_ip[0] = '\0';
                    result = manager_storage_recover(manager_storage_io);
                    if (result < 0) {
                        manager_set_result("RECOVERY", result);
                        manager_draw_home(0, ftp_ip, ftp_port, 0);
                        continue;
                    }
                    manager_draw_home(0, ftp_ip, ftp_port, 0);
                    manager_fill_rect(64u, 116u, 832u, 58u, 0xff252525u);
                    manager_draw_text(96u, 132u, 2u, 0xffeeeeeeu,
                                      "STATUS: RETURNING TO GAME");
                    (void)manager_present();
                    result = sceAppMgrLoadExec("app0:/eboot.bin", NULL, NULL);
                    if (result >= 0) {
                        manager_ftp_server_stop();
                        sceKernelFreeMemBlock(framebuffer_block);
                        return 0;
                    }
                    manager_draw_return_failure();
                }
            } else if (page == MANAGER_PAGE_MODS) {
                if ((pressed & SCE_CTRL_CIRCLE) != 0u) {
                    page = MANAGER_PAGE_HOME;
                    (void)snprintf(manager_status, sizeof(manager_status),
                                   "READY");
                } else if ((pressed & SCE_CTRL_UP) != 0u &&
                           manager_mod_selection != 0u) {
                    --manager_mod_selection;
                } else if ((pressed & SCE_CTRL_DOWN) != 0u &&
                           manager_mod_selection + 1u < manager_mod_count) {
                    ++manager_mod_selection;
                } else if ((pressed & SCE_CTRL_CROSS) != 0u &&
                           manager_mod_count != 0u) {
                    const int enable =
                        manager_mods[manager_mod_selection].disabled;
                    result = manager_storage_set_mod_enabled(
                        manager_storage_io,
                        manager_mods[manager_mod_selection].name, enable);
                    manager_set_result(enable ? "MOD ENABLE" : "MOD DISABLE",
                                       result);
                    (void)manager_refresh_mods();
                }
                if (pressed != 0u)
                    manager_draw_page(page, 0, ftp_ip, ftp_port, 0);
            } else if (page == MANAGER_PAGE_SAVES) {
                if ((pressed & SCE_CTRL_CIRCLE) != 0u) {
                    manager_restore_armed = -1;
                    page = MANAGER_PAGE_HOME;
                    (void)snprintf(manager_status, sizeof(manager_status),
                                   "READY");
                } else if ((pressed & SCE_CTRL_LEFT) != 0u) {
                    manager_restore_armed = -1;
                    manager_backup_slot = manager_backup_slot == 1
                                              ? 3
                                              : manager_backup_slot - 1;
                } else if ((pressed & SCE_CTRL_RIGHT) != 0u) {
                    manager_restore_armed = -1;
                    manager_backup_slot = manager_backup_slot == 3
                                              ? 1
                                              : manager_backup_slot + 1;
                } else if ((pressed & SCE_CTRL_UP) != 0u &&
                           manager_backup_selection != 0u) {
                    manager_restore_armed = -1;
                    --manager_backup_selection;
                } else if ((pressed & SCE_CTRL_DOWN) != 0u &&
                           manager_backup_selection + 1u <
                               manager_backup_count) {
                    manager_restore_armed = -1;
                    ++manager_backup_selection;
                } else if ((pressed & SCE_CTRL_SQUARE) != 0u) {
                    struct manager_save_backup created;
                    manager_restore_armed = -1;
                    if (!manager_slots[manager_backup_slot - 1].present) {
                        (void)snprintf(manager_status,
                                       sizeof(manager_status),
                                       "SLOT %d IS MISSING",
                                       manager_backup_slot);
                    } else {
#if defined(ISAAC_VITA_MANAGER_SAVE_GAMESTATE)
                        memset(&created, 0, sizeof(created));
#endif
                        result = manager_storage_backup_slot(
                            manager_storage_io, manager_backup_slot, &created);
#if defined(ISAAC_VITA_MANAGER_SAVE_GAMESTATE)
                        manager_set_pair_result("SAVE BACKUP", result,
                                                created.has_gamestate
                                                    ? "RUN STATE SAVED"
                                                    : "NO RUN STATE");
#else
                        manager_set_result("SAVE BACKUP", result);
#endif
                        (void)manager_refresh_saves();
                    }
                } else if ((pressed & SCE_CTRL_CROSS) != 0u &&
                           manager_backup_count != 0u) {
                    if (manager_restore_armed ==
                        (int)manager_backup_selection) {
                        result = manager_storage_restore_backup(
                            manager_storage_io,
                            &manager_backups[manager_backup_selection]);
#if defined(ISAAC_VITA_MANAGER_SAVE_GAMESTATE)
                        /* A backup without run state leaves the live
                         * gamestateN.dat as it was. */
                        manager_set_pair_result(
                            "SAVE RESTORE", result,
                            manager_backups[manager_backup_selection]
                                    .has_gamestate
                                ? "RUN STATE RESTORED"
                                : "RUN STATE KEPT");
#else
                        manager_set_result("SAVE RESTORE", result);
#endif
                        manager_restore_armed = -1;
                        (void)manager_refresh_saves();
                    } else {
                        manager_restore_armed =
                            (int)manager_backup_selection;
                        (void)snprintf(
                            manager_status, sizeof(manager_status),
                            "PRESS X AGAIN TO RESTORE SLOT %d",
                            manager_backups[manager_backup_selection].slot);
                    }
                }
                if (pressed != 0u)
                    manager_draw_page(page, 0, ftp_ip, ftp_port, 0);
#if defined(MANAGER_SETTINGS_UI)
            } else if (page == MANAGER_PAGE_SETTINGS) {
                if (manager_settings_ui_handle(&manager_settings_page,
                                               manager_storage_io, pressed,
                                               ftp_port != 0u)) {
                    page = MANAGER_PAGE_HOME;
                }
                /* The page owns the status on every edge, including the
                 * leaving one (READY, or CHANGES DISCARDED after a second O)
                 * so HOME shows why the page closed. */
                (void)snprintf(manager_status, sizeof(manager_status),
                               "%s", manager_settings_page.status);
                if (pressed != 0u)
                    manager_draw_page(page, ftp_port != 0u, ftp_ip, ftp_port,
                                      ftp_start_failed);
#endif
            } else {
                if ((pressed & SCE_CTRL_CIRCLE) != 0u) {
                    page = MANAGER_PAGE_HOME;
                    (void)snprintf(manager_status, sizeof(manager_status),
                                   "READY");
                }
                if (pressed != 0u)
                    manager_draw_page(page, 0, ftp_ip, ftp_port, 0);
            }
        }
        (void)manager_present();
    }
}
#else /* MANAGER_UI2 */

/*
 * Manager UI2.  The page controller
 * (manager_ui.c) owns the pages, the key repeat, the confirmation boxes and
 * the auto-launch countdown and emits a display list; this glue reads the
 * pad once per vblank, paints the list into manager_canvas when the
 * controller asks for a redraw and presents it through the same off-screen
 * CDRAM buffer path as before.  Storage, the FTP server and LoadExec stay
 * here.  The controller's button bits are the SDK values (the assert above
 * covers ten of them, the one below the page triggers).
 */

#ifndef ISAAC_VITA_BUILD_ID
#define ISAAC_VITA_BUILD_ID ""
#endif
#ifndef MANAGER_APP_VERSION
#define MANAGER_APP_VERSION ""
#endif
/* A stick deflection further than 64 from the centre (128) reads as the
 * d-pad direction; the digital pad wins when both are held. */
#define MANAGER_UI2_STICK_LOW 64u
#define MANAGER_UI2_STICK_HIGH 192u
#define MANAGER_UI2_BUTTONS                                                    \
    (SCE_CTRL_SELECT | SCE_CTRL_START | SCE_CTRL_UP | SCE_CTRL_RIGHT |       \
     SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER |  \
     SCE_CTRL_TRIANGLE | SCE_CTRL_CIRCLE | SCE_CTRL_CROSS | SCE_CTRL_SQUARE)

#if defined(MANAGER_SETTINGS_UI)
_Static_assert((unsigned)MANAGER_SETTINGS_UI_BUTTON_LTRIGGER ==
                       (unsigned)SCE_CTRL_LTRIGGER &&
                   (unsigned)MANAGER_SETTINGS_UI_BUTTON_RTRIGGER ==
                       (unsigned)SCE_CTRL_RTRIGGER,
               "manager_ui page triggers must equal SCE_CTRL_L/RTRIGGER");
#endif

static struct manager_theme_fonts manager_ui2_fonts;
static struct manager_theme_set manager_ui2_themes;
static struct manager_ui manager_ui2;
static struct manager_display_list manager_ui2_list;

#if defined(MANAGER_UI2_GAME_SKIN)
/* The game skin (manager_skin_game.c) reads the user's
 * ux0:data/isaacr001/resources/packed archives through the sceIo blob layer
 * and keeps everything it decodes in one USER_RW memblock,
 * allocated the first time theme=game is selected and freed again when the
 * skin fails to load (the light theme and a status line take over). */
static struct manager_skin_game_context manager_ui2_skin;
static SceUID manager_ui2_skin_block = -1;

static void *manager_ui2_skin_acquire(void *opaque, size_t bytes)
{
    void *base = NULL;
    SceUID block;
    (void)opaque;
    if (manager_ui2_skin_block >= 0)
        return NULL;
    bytes = (bytes + 0xfffu) & ~(size_t)0xfffu; /* USER_RW: 4 KiB units */
    block = sceKernelAllocMemBlock("isaac_manager_skin",
                                   SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, bytes,
                                   NULL);
    if (block < 0)
        return NULL;
    if (sceKernelGetMemBlockBase(block, &base) < 0 || base == NULL) {
        sceKernelFreeMemBlock(block);
        return NULL;
    }
    manager_ui2_skin_block = block;
    return base;
}

static void manager_ui2_skin_release(void *opaque, void *block, size_t bytes)
{
    (void)opaque;
    (void)block;
    (void)bytes;
    if (manager_ui2_skin_block >= 0) {
        sceKernelFreeMemBlock(manager_ui2_skin_block);
        manager_ui2_skin_block = -1;
    }
}

static uint64_t manager_ui2_skin_clock(void *opaque)
{
    (void)opaque;
    return (uint64_t)sceKernelGetProcessTimeWide();
}
#endif

static int manager_ui2_ftp_start(void *context, char *ip, size_t capacity,
                                 unsigned short *port)
{
    (void)context;
    return manager_ftp_server_start(ip, capacity, port);
}

static void manager_ui2_ftp_stop(void *context)
{
    (void)context;
    manager_ftp_server_stop();
}

static int manager_ui2_ftp_listening(void *context)
{
    (void)context;
    return manager_ftp_server_is_listening();
}

/* lang=auto follows the system language: Russian, else English.  SceAppUtil
 * is initialised for this one read and shut down again; any failure means
 * English. */
static enum manager_lang manager_ui2_system_lang(void)
{
    SceAppUtilInitParam init_param;
    SceAppUtilBootParam boot_param;
    int value = -1;
    memset(&init_param, 0, sizeof(init_param));
    memset(&boot_param, 0, sizeof(boot_param));
    if (sceAppUtilInit(&init_param, &boot_param) < 0)
        return MANAGER_LANG_EN;
    if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &value) < 0)
        value = -1;
    (void)sceAppUtilShutdown();
    return value == SCE_SYSTEM_PARAM_LANG_RUSSIAN ? MANAGER_LANG_RU
                                                  : MANAGER_LANG_EN;
}

/* Held buttons: the twelve the controller knows, the DS3's L1/R1 (PS TV)
 * as the page triggers, the left stick as the d-pad. */
static uint32_t manager_ui2_held(const SceCtrlData *pad)
{
    uint32_t held = pad->buttons & (uint32_t)MANAGER_UI2_BUTTONS;
    if ((pad->buttons & SCE_CTRL_L1) != 0u)
        held |= SCE_CTRL_LTRIGGER;
    if ((pad->buttons & SCE_CTRL_R1) != 0u)
        held |= SCE_CTRL_RTRIGGER;
    if ((held & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) == 0u) {
        if (pad->lx < MANAGER_UI2_STICK_LOW)
            held |= SCE_CTRL_LEFT;
        else if (pad->lx > MANAGER_UI2_STICK_HIGH)
            held |= SCE_CTRL_RIGHT;
    }
    if ((held & (SCE_CTRL_UP | SCE_CTRL_DOWN)) == 0u) {
        if (pad->ly < MANAGER_UI2_STICK_LOW)
            held |= SCE_CTRL_UP;
        else if (pad->ly > MANAGER_UI2_STICK_HIGH)
            held |= SCE_CTRL_DOWN;
    }
    return held;
}

static void manager_ui2_draw(void)
{
    struct manager_canvas canvas;
    manager_ui_build(&manager_ui2, &manager_ui2_list);
    manager_canvas_init(&canvas, manager_canvas, (int)MANAGER_WIDTH,
                        (int)MANAGER_HEIGHT, (int)MANAGER_PITCH);
    manager_ui_draw(&manager_ui2_list, manager_ui_theme(&manager_ui2),
                    &canvas);
    manager_canvas_dirty = 1;
}

int main(void)
{
    SceUID framebuffer_block;
    uint32_t *display_memory;
    unsigned int buffer;
    const struct manager_storage_io *io;
    struct manager_ui_config config;
    struct manager_ui_host host;
    uint32_t tick = 0u;

    framebuffer_block = sceKernelAllocMemBlock(
        "isaac_manager_display", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
        MANAGER_FRAMEBUFFER_BYTES * MANAGER_DISPLAY_BUFFERS, NULL);
    if (framebuffer_block < 0) {
        manager_ftp_server_stop();
        return 2;
    }
    if (sceKernelGetMemBlockBase(framebuffer_block,
                                 (void **)&display_memory) < 0) {
        manager_ftp_server_stop();
        sceKernelFreeMemBlock(framebuffer_block);
        return 3;
    }
    for (buffer = 0u; buffer < MANAGER_DISPLAY_BUFFERS; ++buffer)
        manager_display_buffers[buffer] =
            display_memory + buffer * (MANAGER_FRAMEBUFFER_BYTES / 4u);

    memset(&config, 0, sizeof(config));
    config.fonts_fallback = manager_assets_load_fonts(&manager_ui2_fonts);
#if defined(MANAGER_UI2_GAME_SKIN)
    memset(&manager_ui2_skin, 0, sizeof(manager_ui2_skin));
    manager_ui2_skin.io = &manager_blob_vita_io;
    manager_ui2_skin.packed_dir = NULL; /* MANAGER_ARCH_PACKED_DIR */
    manager_ui2_skin.acquire = manager_ui2_skin_acquire;
    manager_ui2_skin.release = manager_ui2_skin_release;
    manager_ui2_skin.clock_us = manager_ui2_skin_clock;
    manager_theme_set_init(&manager_ui2_themes, &manager_ui2_fonts,
                           &manager_ui2_skin);
#else
    /* The game skin, when compiled, reads its own files: no context. */
    manager_theme_set_init(&manager_ui2_themes, &manager_ui2_fonts, NULL);
#endif
    io = manager_storage_vita_io();
    config.app_version = MANAGER_APP_VERSION;
    config.build_id = ISAAC_VITA_BUILD_ID;
    config.system_lang = manager_ui2_system_lang();
    config.recovery_result = manager_storage_recover(io);
    host.context = NULL;
    host.ftp_start = manager_ui2_ftp_start;
    host.ftp_stop = manager_ui2_ftp_stop;
    host.ftp_listening = manager_ui2_ftp_listening;
    manager_ui_init(&manager_ui2, io, &host, &config, &manager_ui2_themes,
                    tick);
    manager_ui2_draw();
    if (manager_present() < 0) {
        manager_ftp_server_stop();
        sceKernelFreeMemBlock(framebuffer_block);
        return 4;
    }

    (void)sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    for (;;) {
        SceCtrlData pad;
        uint32_t held = 0u;
        unsigned int result;

        memset(&pad, 0, sizeof(pad));
        if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0)
            held = manager_ui2_held(&pad);
        /* One pass per vblank (manager_present waits for it). */
        result = manager_ui_update(&manager_ui2, held, ++tick);
        if ((result & MANAGER_UI_LAUNCH) != 0u) {
            /* Stops FTP and runs recovery; the status says why not. */
            if (manager_ui_prepare_launch(&manager_ui2) == 0) {
                manager_ui2_draw();
                (void)manager_present();
                manager_ftp_server_stop();
                if (sceAppMgrLoadExec("app0:/eboot.bin", NULL, NULL) >= 0) {
                    sceKernelFreeMemBlock(framebuffer_block);
                    return 0;
                }
                manager_ui_launch_failed(&manager_ui2);
            }
            result |= MANAGER_UI_REDRAW;
        }
        if ((result & MANAGER_UI_REDRAW) != 0u)
            manager_ui2_draw();
        (void)manager_present();
    }
}
#endif /* MANAGER_UI2 */
