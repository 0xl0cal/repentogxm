/*
 * Minimal native front-end for the LiveArea manager entry.
 *
 * The direct CDRAM framebuffer setup intentionally follows VitaSDK's working
 * debug-screen sample rather than bringing the game's graphics stack into this
 * tiny process:
 * https://github.com/vitasdk/samples/blob/fe8fbef570f3280586c0c20157146e3faefb2181/common/debugScreen.c#L398-L415
 */

#include "manager_ftp_server.h"
#include "manager_storage.h"
#include "manager_storage_vita.h"

#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MANAGER_WIDTH 960u
#define MANAGER_HEIGHT 544u
#define MANAGER_PITCH 960u
#define MANAGER_FRAMEBUFFER_BYTES (2u * 1024u * 1024u)
#define MANAGER_MAX_MODS 96u
#define MANAGER_MAX_BACKUPS 96u
#define MANAGER_MOD_ROWS 7u
#define MANAGER_BACKUP_ROWS 5u

static uint32_t *manager_framebuffer;
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
};

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

    for (row = 0; row < height; ++row) {
        uint32_t *pixel = manager_framebuffer +
                          (y + row) * MANAGER_PITCH + x;
        for (column = 0; column < width; ++column)
            pixel[column] = color;
    }
}

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
            (void)snprintf(line, sizeof(line), "SLOT %d: %lu BYTES%s",
                           slot + 1, (unsigned long)manager_slots[slot].size,
                           manager_backup_slot == slot + 1 ? "  <" : "");
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
    SceDisplayFrameBuf frame;
    uint32_t previous_buttons = 0u;
    char ftp_ip[16] = "";
    unsigned short ftp_port = 0u;
    int ftp_start_failed = 0;
    enum manager_page page = MANAGER_PAGE_HOME;
    int result;

    framebuffer_block = sceKernelAllocMemBlock(
        "isaac_manager_display", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
        MANAGER_FRAMEBUFFER_BYTES, NULL);
    if (framebuffer_block < 0) {
        manager_ftp_server_stop();
        return 2;
    }
    if (sceKernelGetMemBlockBase(framebuffer_block,
                                 (void **)&manager_framebuffer) < 0) {
        manager_ftp_server_stop();
        sceKernelFreeMemBlock(framebuffer_block);
        return 3;
    }

    manager_storage_io = manager_storage_vita_io();
    result = manager_storage_recover(manager_storage_io);
    if (result < 0)
        manager_set_result("RECOVERY", result);
    else if (result == MANAGER_STORAGE_SYNC_WARNING)
        manager_set_result("RECOVERY", result);
    else
        (void)snprintf(manager_status, sizeof(manager_status), "READY");
    manager_draw_home(0, ftp_ip, ftp_port, 0);
    memset(&frame, 0, sizeof(frame));
    frame.size = sizeof(frame);
    frame.base = manager_framebuffer;
    frame.pitch = MANAGER_PITCH;
    frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    frame.width = MANAGER_WIDTH;
    frame.height = MANAGER_HEIGHT;
    result = sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
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
                } else if ((pressed & SCE_CTRL_CROSS) != 0u) {
                    if (ftp_port != 0u) {
                        manager_fill_rect(64u, 180u, 832u, 96u, 0xff252525u);
                        manager_draw_text(96u, 192u, 2u, 0xffa8a8a8u,
                                          "FTP: STOPPING");
                        sceDisplayWaitVblankStart();
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
                    sceDisplayWaitVblankStart();
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
                        result = manager_storage_backup_slot(
                            manager_storage_io, manager_backup_slot, &created);
                        manager_set_result("SAVE BACKUP", result);
                        (void)manager_refresh_saves();
                    }
                } else if ((pressed & SCE_CTRL_CROSS) != 0u &&
                           manager_backup_count != 0u) {
                    if (manager_restore_armed ==
                        (int)manager_backup_selection) {
                        result = manager_storage_restore_backup(
                            manager_storage_io,
                            &manager_backups[manager_backup_selection]);
                        manager_set_result("SAVE RESTORE", result);
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
                manager_draw_page(page, 0, ftp_ip, ftp_port, 0);
            } else {
                if ((pressed & SCE_CTRL_CIRCLE) != 0u) {
                    page = MANAGER_PAGE_HOME;
                    (void)snprintf(manager_status, sizeof(manager_status),
                                   "READY");
                }
                manager_draw_page(page, 0, ftp_ip, ftp_port, 0);
            }
        }
        sceDisplayWaitVblankStart();
    }
}
