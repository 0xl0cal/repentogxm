/*
 * Manager UI2 settings file ux0:data/isaacr001/manager.ini (owner
 * decision 2026-09-27 for `theme` and `lang = auto`).
 *
 *   lang       = auto | en | ru        (default auto: the system language,
 *                                       Russian -> ru, anything else -> en)
 *   autolaunch = off | 5 | 10 | 30     (default off; seconds)
 *   theme      = game | dark | light   (default game)
 *
 * A manager-only file: port.ini is read by the game, whose parser would
 * report an unknown key at every boot.  Written with
 * manager_storage_write_text_atomic(io, "manager.ini", ...), so the previous
 * file is archived as .manager-backups/manager-NNNN.ini.  Parsing is
 * forgiving: an unknown key, an unknown value or a line without '=' is
 * counted and skipped, the key keeps its default.  SDK-free.
 */
#ifndef ISAAC_MANAGER_INI_H
#define ISAAC_MANAGER_INI_H

#if defined(MANAGER_UI2)

#include "manager_storage.h"
#include "manager_theme.h"

#include <stddef.h>
#include <stdint.h>

#define MANAGER_INI_NAME "manager.ini"
#define MANAGER_INI_PATH MANAGER_STORAGE_ROOT "/" MANAGER_INI_NAME
#define MANAGER_INI_MAX_BYTES 4096u

enum manager_ini_lang {
    MANAGER_INI_LANG_AUTO = 0,
    MANAGER_INI_LANG_EN,
    MANAGER_INI_LANG_RU,
    MANAGER_INI_LANG_COUNT
};

enum manager_ini_status {
    MANAGER_INI_ABSENT = 0,      /* no file: defaults */
    MANAGER_INI_OK,
    MANAGER_INI_OK_WITH_IGNORED, /* read; some lines skipped */
    MANAGER_INI_UNREADABLE       /* I/O error, too large, not a file */
};

struct manager_ini {
    uint8_t lang;       /* enum manager_ini_lang */
    uint8_t autolaunch; /* seconds: 0 (off), 5, 10, 30 */
    uint8_t theme;      /* enum manager_theme_id */
};

void manager_ini_defaults(struct manager_ini *ini);
int manager_ini_equal(const struct manager_ini *a, const struct manager_ini *b);
/* Parses from defaults; returns the number of skipped lines. */
unsigned int manager_ini_parse(const char *text, size_t length,
                               struct manager_ini *ini);
/* Canonical text (comment line + the three keys); returns its length, 0
 * when it does not fit. */
size_t manager_ini_serialize(const struct manager_ini *ini, char *out,
                             size_t capacity);
/* enum manager_ini_status; *ini holds defaults unless the file was read. */
int manager_ini_load(const struct manager_storage_io *io,
                     struct manager_ini *ini, unsigned int *ignored);
/* enum manager_storage_result of the atomic write. */
int manager_ini_save(const struct manager_storage_io *io,
                     const struct manager_ini *ini);
/* off -> 5 -> 10 -> 30 (clamped at both ends). */
uint8_t manager_ini_autolaunch_step(uint8_t current, int direction);

#endif /* MANAGER_UI2 */

#endif
