/*
 * Manager UI2 page controller (pages, key repeat, confirmation boxes;
 * knob ISAAC_VITA_MANAGER_UI2 -> MANAGER_UI2=1).
 *
 * Pages HOME, MODS, SAVES, SETTINGS, ACHIEVEMENTS (empty state), CONTROLS
 * and ABOUT in C's page model.  Input is the held-button mask once per
 * vblank plus a tick counter (60 per second); the controller derives the
 * pressed edges, repeats UP/DOWN/LEFT/RIGHT after 24 ticks every 5 ticks,
 * runs the optional auto-launch countdown and emits a display list
 * (manager_ui_draw.h) - never pixels - so every page and state is testable
 * on the host.  Storage goes through the manager_storage_io vtable, the FTP
 * server through three host callbacks; LoadExec stays in manager_main.c.
 *
 * Buttons are the SCE_CTRL_* bits (enum manager_settings_ui_button, which
 * manager_main.c static-asserts against the SDK).  L/R switch pages from
 * anywhere, O returns to HOME, X acts; destructive actions open a
 * confirmation box that keeps the settings model's arming enum
 * (MANAGER_SETTINGS_UI_ARM_*).  While the FTP server listens, MODS, SAVES
 * and SETTINGS open read-only (writes refused with a status line).
 *
 * SDK-free.
 */
#ifndef ISAAC_MANAGER_UI_H
#define ISAAC_MANAGER_UI_H

#if defined(MANAGER_UI2)

#include "manager_ini.h"
#include "manager_mods_meta.h"
#include "manager_settings_ui.h"
#include "manager_storage.h"
#include "manager_strings.h"
#include "manager_theme.h"
#include "manager_ui_draw.h"

#include <stddef.h>
#include <stdint.h>

#define MANAGER_UI_MAX_MODS 96u
#define MANAGER_UI_MAX_BACKUPS 96u
#define MANAGER_UI_MOD_ROWS 9u     /* mock C: y 92 + n*36, nine rows */
#define MANAGER_UI_BACKUP_ROWS 7u  /* y 200 + n*36 */
#define MANAGER_UI_TICKS_PER_SECOND 60u
#define MANAGER_UI_REPEAT_DELAY 24u    /* 400 ms */
#define MANAGER_UI_REPEAT_INTERVAL 5u  /* 83 ms (design: 80 ms) */
#define MANAGER_UI_EID_DIRECTORY "836319872"
#define MANAGER_UI_STATUS_CAPACITY 256u
#define MANAGER_UI_RESOURCE_ANIMATIONS_BYTES 660301u    /* INSTALL.md */
#define MANAGER_UI_RESOURCE_REPENTANCE_BYTES 385003320u /* INSTALL.md */

enum manager_ui_page {
    MANAGER_UI_PAGE_HOME = 0,
    MANAGER_UI_PAGE_MODS,
    MANAGER_UI_PAGE_SAVES,
    MANAGER_UI_PAGE_SETTINGS,
    MANAGER_UI_PAGE_ACHIEVEMENTS,
    MANAGER_UI_PAGE_CONTROLS,
    MANAGER_UI_PAGE_ABOUT,
    MANAGER_UI_PAGE_COUNT
};

enum manager_ui_home_row {
    MANAGER_UI_HOME_LAUNCH = 0,
    MANAGER_UI_HOME_FTP,
    MANAGER_UI_HOME_MODS,
    MANAGER_UI_HOME_SAVES,
    MANAGER_UI_HOME_SETTINGS,
    MANAGER_UI_HOME_ACHIEVEMENTS,
    MANAGER_UI_HOME_CONTROLS,
    MANAGER_UI_HOME_ABOUT,
    MANAGER_UI_HOME_COUNT
};

enum manager_ui_settings_row {
    MANAGER_UI_SET_FPS = 0,
    MANAGER_UI_SET_RENDER,
    MANAGER_UI_SET_EID,        /* only when mods/836319872 exists */
    MANAGER_UI_SET_LANG,
    MANAGER_UI_SET_AUTOLAUNCH,
    MANAGER_UI_SET_THEME,
    MANAGER_UI_SET_UNDO,       /* only while an older port.ini exists */
    MANAGER_UI_SET_DEFAULTS,
#if defined(MANAGER_DEBUG_OVERLAY_ROW)
    /* port.ini debug_overlay, shown in the Game section after EID only when
     * the shared port.ini parser knows the key (manager_ui.c
     * ui_overlay_key) */
    MANAGER_UI_SET_DEBUG_OVERLAY,
#endif
    MANAGER_UI_SET_COUNT
};

enum manager_ui_confirm {
    MANAGER_UI_CONFIRM_NONE = 0,
    MANAGER_UI_CONFIRM_LAUNCH_FTP, /* launch while the FTP server listens */
    MANAGER_UI_CONFIRM_MODS_ALL,   /* enable all / disable all */
    MANAGER_UI_CONFIRM_RESTORE,    /* restore the selected backup */
    MANAGER_UI_CONFIRM_SETTINGS    /* settings.armed: SAVE / DEFAULTS / DISCARD */
};

enum manager_ui_resource {
    MANAGER_UI_RESOURCE_MISSING = 0,
    MANAGER_UI_RESOURCE_OK,
    MANAGER_UI_RESOURCE_DIFFERENT /* present, not the INSTALL.md size */
};

/* manager_ui_update() result bits. */
#define MANAGER_UI_REDRAW 1u
#define MANAGER_UI_LAUNCH 2u

struct manager_ui_host {
    void *context;
    /* Start the server; fills the dotted IP and port; <0 on failure. */
    int (*ftp_start)(void *context, char *ip, size_t capacity,
                     unsigned short *port);
    void (*ftp_stop)(void *context);
    int (*ftp_listening)(void *context);
};

struct manager_ui_config {
    const char *app_version;     /* ISAAC_APP_VERSION, "01.01" */
    const char *build_id;        /* ISAAC_VITA_BUILD_ID */
    enum manager_lang system_lang; /* lang=auto resolves to this */
    int recovery_result;         /* manager_storage_recover() at start */
    int fonts_fallback;          /* 1: IMF atlases missing, 5x7 in use */
};

struct manager_ui_mod {
    struct manager_mod_entry entry;
    struct manager_mod_meta meta;
};

#if defined(MANAGER_TROPHY_IMPORT)
/* The game runtime's trophy import contract (recomp/vita/TROPHIES.md,
 * import request): request = one ASCII digit 1..3 with optional space, tab,
 * CR or LF around it, at most 16 bytes; result = one line
 * "TROPHY IMPORT: slot=<n> status=<s> saved=<k> mapped=<m> unlocked=<u>
 * already=<a> failed=<f> noset=<x> request=<deleted|kept>". */
#define MANAGER_TROPHY_REQUEST_PATH MANAGER_STORAGE_ROOT "/trophy-import.req"
#define MANAGER_TROPHY_REQUEST_NAME "trophy-import.req"
#define MANAGER_TROPHY_RESULT_PATH MANAGER_STORAGE_ROOT "/trophy-import.res"
#define MANAGER_TROPHY_REQUEST_MAX_BYTES 16u
#define MANAGER_TROPHY_RESULT_MAX_BYTES 256u

struct manager_trophy_result {
    unsigned int slot;
    char status[16]; /* the runtime's token: done, partial, unavailable, ... */
    unsigned int saved;
    unsigned int mapped;
    unsigned int unlocked;
    unsigned int already;
    unsigned int failed;
    unsigned int noset;
    int request_kept; /* 1 kept, 0 deleted or not given */
};

/* 1..3 for a valid request text, 0 otherwise (the runtime's rule). */
int manager_ui_trophy_parse_request(const char *text, size_t length);
/* 1 when the text is a result line (slot and status present, every given
 * number in range), 0 otherwise; *result is zeroed first. */
int manager_ui_trophy_parse_result(const char *text,
                                   struct manager_trophy_result *result);
#endif

struct manager_ui {
    const struct manager_storage_io *io;
    struct manager_ui_host host;
    struct manager_ui_config config;
    struct manager_theme_set *themes;
    const struct manager_theme *theme;
    enum manager_lang lang;
    enum manager_ui_page page;
    enum manager_ui_confirm confirm;
    int detail_open;
    uint32_t now;

    /* manager.ini */
    struct manager_ini ini_saved;
    struct manager_ini ini_pending;
    int ini_status;

    /* HOME */
    unsigned int home_row;
    int ftp_on;
    int ftp_failed;
    char ftp_ip[16];
    unsigned short ftp_port;
    int countdown_active;
    uint32_t countdown_deadline;
    unsigned int countdown_shown;

    /* MODS */
    struct manager_ui_mod mods[MANAGER_UI_MAX_MODS];
    size_t mod_count;
    size_t mod_rejected;
    size_t mod_selection;
    int mod_scan_result;
    int mod_meta_loaded;

    /* SAVES */
    struct manager_save_slot slots[3];
    struct manager_save_backup backups[MANAGER_UI_MAX_BACKUPS];
    size_t backup_count;
    size_t backup_rejected;
    size_t backup_selection; /* index into the filtered view */
    int save_scan_result;
    int save_slot;           /* 1..3 */
    int show_all_backups;

    /* SETTINGS */
    struct manager_settings_ui settings; /* state, last_boot, armed */
    unsigned int settings_row;           /* enum manager_ui_settings_row */
    int eid_present;
    int eid_saved;   /* enabled on disk */
    int eid_pending;
    enum manager_ui_page leave_target;   /* after a confirmed discard */
#if defined(MANAGER_DEBUG_OVERLAY_ROW)
    /* enum isaac_vita_settings_key of "debug_overlay", or -1 when the
     * port.ini parser does not know it (the row is then absent) */
    int overlay_key;
#endif

    /* ABOUT */
    uint8_t resource_animations; /* enum manager_ui_resource */
    uint8_t resource_repentance;

#if defined(MANAGER_TROPHY_IMPORT)
    /* ACHIEVEMENTS: trophy import row */
    int trophy_slot;    /* 1..3, the row's value */
    int trophy_pending; /* trophy-import.req: 0 absent, 1..3, -1 not a slot */
    int trophy_result_state; /* trophy-import.res: 0 absent, 1 parsed,
                                -1 unreadable */
    struct manager_trophy_result trophy_result;
#endif

    /* status row */
    char status[MANAGER_UI_STATUS_CAPACITY];
    uint8_t status_tone; /* enum manager_tone */

    /* input */
    uint32_t held_previous;
    uint32_t repeat_button;
    uint32_t repeat_at;
};

/* Loads manager.ini, the mods list, the save slots and the settings model,
 * picks the theme and language, arms the countdown when allowed. */
void manager_ui_init(struct manager_ui *ui, const struct manager_storage_io *io,
                     const struct manager_ui_host *host,
                     const struct manager_ui_config *config,
                     struct manager_theme_set *themes, uint32_t now);
/* One vblank: held buttons, tick.  Returns MANAGER_UI_REDRAW and/or
 * MANAGER_UI_LAUNCH (the caller then runs manager_ui_prepare_launch). */
unsigned int manager_ui_update(struct manager_ui *ui, uint32_t held,
                               uint32_t now);
void manager_ui_build(const struct manager_ui *ui,
                      struct manager_display_list *dl);
const struct manager_theme *manager_ui_theme(const struct manager_ui *ui);
/* Stops FTP and runs recovery; 0 = ready for LoadExec (status "Returning
 * to the game..."), <0 = recovery failed (status says why). */
int manager_ui_prepare_launch(struct manager_ui *ui);
/* LoadExec returned: status "The game did not start ...". */
void manager_ui_launch_failed(struct manager_ui *ui);
/* Number of visible settings rows and the row at a position. */
unsigned int manager_ui_settings_rows(const struct manager_ui *ui,
                                      uint8_t rows[MANAGER_UI_SET_COUNT]);
/* Pending edits on the SETTINGS page (port.ini + EID + manager.ini). */
unsigned int manager_ui_pending_count(const struct manager_ui *ui);

#endif /* MANAGER_UI2 */

#endif
