/*
 * Manager settings core:
 * port.ini load / pending edits / save / defaults / undo over the
 * manager_storage_io vtable, and the per-boot log head reader behind the
 * ISAAC SETTINGS page.  SDK-free: this translation unit is compiled into the
 * manager (with MANAGER_SETTINGS_UI=1) and into the host test.  Everything
 * declares to nothing without the knob so the unit may never leak into an
 * OFF build.
 */
#ifndef ISAAC_MANAGER_SETTINGS_H
#define ISAAC_MANAGER_SETTINGS_H

#if defined(MANAGER_SETTINGS_UI)

#include "host_vita_settings_parse.h"
#include "manager_storage.h"

#include <stddef.h>
#include <stdint.h>

/* The reader accepts up to 4097 bytes (by design: "the reader requests
 * 4097; more -> too-large") so a 4097-byte file reaches the parser and is
 * reported as too-large by it; longer files are refused by the storage read. */
#define MANAGER_SETTINGS_FILE_CAPACITY (ISAAC_VITA_SETTINGS_MAX_BYTES + 2u)
/* First 16 KiB of the per-boot game log (platform.c rotates and truncates
 * it at every game boot; the manager is LoadExec'd before that reset). */
#define MANAGER_SETTINGS_LOG_HEAD_BYTES (16u * 1024u)
#define MANAGER_SETTINGS_LOG_PATH MANAGER_STORAGE_ROOT "/first-arm-fault.log"
/* Every text helper below fits this many bytes including the NUL and stays
 * within the manager's 58-character status row. */
#define MANAGER_SETTINGS_TEXT_CAPACITY 64u

enum manager_settings_load_status {
    MANAGER_SETTINGS_LOAD_ABSENT = 0,
    MANAGER_SETTINGS_LOAD_OK,
    MANAGER_SETTINGS_LOAD_OK_WITH_IGNORED, /* OK; rejected+unknown > 0 */
    MANAGER_SETTINGS_LOAD_INVALID,  /* schema / garbage / too-large: defaults */
    MANAGER_SETTINGS_LOAD_IO_ERROR, /* read failed other than ENOENT */
};

enum manager_settings_action {
    MANAGER_SETTINGS_ACTION_SAVE = 0,
    MANAGER_SETTINGS_ACTION_DEFAULTS,
    MANAGER_SETTINGS_ACTION_UNDO,
    MANAGER_SETTINGS_ACTION_COUNT
};

enum manager_settings_code {
    MANAGER_SETTINGS_CODE_OK = 0,
    MANAGER_SETTINGS_CODE_SYNC_WARNING,      /* written; post-archive sync failed */
    MANAGER_SETTINGS_CODE_NO_FILE,           /* DEFAULTS with no port.ini */
    MANAGER_SETTINGS_CODE_FTP_LISTENING,     /* every write refused */
    MANAGER_SETTINGS_CODE_NEED_A_AND_B,      /* remap guard */
    MANAGER_SETTINGS_CODE_BAD_FILE,          /* not serializable / INVALID */
    MANAGER_SETTINGS_CODE_NO_FREE_BACKUP_SLOT, /* CAPACITY */
    MANAGER_SETTINGS_CODE_HASH_MISMATCH,
    MANAGER_SETTINGS_CODE_FILE_CONFLICT,
    MANAGER_SETTINGS_CODE_IO_ERROR,
    MANAGER_SETTINGS_CODE_NOTHING_TO_UNDO,   /* no archive / equal hashes */
    MANAGER_SETTINGS_CODE_BAD_ARCHIVE,       /* archive does not parse */
    MANAGER_SETTINGS_CODE_COUNT
};

struct manager_settings_result {
    uint8_t action; /* enum manager_settings_action */
    uint8_t code;   /* enum manager_settings_code */
};

/* A header-only helper: clang's -Wunused-function fires on a static inline
 * that a translation unit never calls (the host probe compiles this header
 * as its main TU under -Werror); gcc never warns.  Marked unused for both. */
static inline int __attribute__((unused)) manager_settings_result_ok(
    const struct manager_settings_result *result)
{
    return result->code == MANAGER_SETTINGS_CODE_OK ||
           result->code == MANAGER_SETTINGS_CODE_SYNC_WARNING ||
           result->code == MANAGER_SETTINGS_CODE_NO_FILE;
}

struct manager_settings_state {
    struct isaac_vita_settings saved;   /* what port.ini holds (defaults when
                                         * absent/invalid; remap reset on guard) */
    struct isaac_vita_settings pending; /* the edited copy shown on the page */
    struct isaac_vita_settings_receipt receipt; /* parser receipt; status
                                                 * ABSENT/IO_ERROR/TOO_LARGE
                                                 * filled in by the loader */
    enum manager_settings_load_status status;
    int storage_result;     /* enum manager_storage_result of the read */
    uint32_t ignored;       /* receipt.rejected + receipt.unknown */
    int undo_available;     /* newest archive exists and differs from the
                             * live file (a garbage archive is offered and
                             * refused on the press: BAD ARCHIVE) */
    unsigned int undo_index; /* its NNNN when undo_available */
};

enum manager_settings_last_boot_kind {
    MANAGER_SETTINGS_LAST_BOOT_NO_LOG = 0, /* no log, unreadable, or no "build " line */
    MANAGER_SETTINGS_LAST_BOOT_IGNORES,    /* "build " line, no settings receipt */
    MANAGER_SETTINGS_LAST_BOOT_RECEIPT,    /* "ISAAC VITA SETTINGS: " line parsed */
};

enum manager_settings_remap_word {
    MANAGER_SETTINGS_REMAP_DEFAULT = 0,
    MANAGER_SETTINGS_REMAP_DEFAULT_GUARD, /* remap=default(guard) */
    MANAGER_SETTINGS_REMAP_CUSTOM,
};

struct manager_settings_last_boot {
    enum manager_settings_last_boot_kind kind;
    uint16_t fps_lock;                    /* receipt fps_lock= (requested) */
    uint8_t fps_ignored;                  /* fps_lock=N(ignored:...): the
                                           * game build could not apply it */
    enum manager_settings_remap_word remap;
    char status[ISAAC_VITA_SETTINGS_VALUE_MAX + 1u]; /* receipt status= token */
};

/* read_text + parse + validate.  Returns the storage result of the read
 * (OK, ABSENT, TOO_LARGE, IO_ERROR, ...); state->status classifies it,
 * saved/pending hold the file (or defaults) and undo availability is
 * refreshed.  Never fails hard: a NULL io yields INVALID / LOAD_IO_ERROR. */
int manager_settings_load(const struct manager_storage_io *io,
                          struct manager_settings_state *state);
/* newest port-NNNN.ini exists and its SHA-256 differs from port.ini's (an
 * absent port.ini differs; an archive too large or not a regular file cannot
 * be compared and is offered) -> undo_available/undo_index.  Whether the
 * archive parses is reported on the press (UNDO REFUSED - BAD ARCHIVE). */
void manager_settings_refresh_undo(const struct manager_storage_io *io,
                                   struct manager_settings_state *state);

/* Pending-edit model: parser acceptance rules; 0 ok, -1 rejected. */
int manager_settings_set_pending_text(struct manager_settings_state *state,
                                      enum isaac_vita_settings_key key,
                                      const char *text, size_t length);
int manager_settings_set_fps_lock(struct manager_settings_state *state,
                                  uint16_t fps_lock);
/* 60 -> 45 -> 40 -> 30 for +1, back for -1, clamped at both ends; an
 * out-of-set current value steps to 60. */
uint16_t manager_settings_fps_lock_step(uint16_t current, int direction);
/* render_scale (the game's MaxRenderScale the runtime writes into
 * options.ini at boot; port default 1): 1 = 480x270 world, 2 = the stock
 * 960x540.  set: 0 ok, -1 rejected; step: 1 <-> 2, clamped, an
 * out-of-set current value steps to 1. */
int manager_settings_set_render_scale(struct manager_settings_state *state,
                                      uint8_t render_scale);
uint8_t manager_settings_render_scale_step(uint8_t current, int direction);
/* Keys whose canonical text differs between saved and pending. */
unsigned int manager_settings_pending_count(
    const struct manager_settings_state *state);
void manager_settings_discard(struct manager_settings_state *state);

/* SAVE: guard, serialize, manager_storage_write_text_atomic, reload.
 * DEFAULTS: manager_storage_archive_and_remove, reload.
 * UNDO: newest archive -> parse -> write its exact bytes (which archives
 * the current file under the next index, so UNDO twice = redo whenever the
 * undone SAVE/UNDO replaced a live file; after DEFAULTS + UNDO nothing was
 * replaced, no archive is created and the row hides - DEFAULTS again is the
 * way back), reload.
 * ftp_listening != 0 refuses every write before touching storage.  On
 * failure the state (including pending) is left as it was. */
struct manager_settings_result manager_settings_save(
    const struct manager_storage_io *io, struct manager_settings_state *state,
    int ftp_listening);
struct manager_settings_result manager_settings_defaults(
    const struct manager_storage_io *io, struct manager_settings_state *state,
    int ftp_listening);
struct manager_settings_result manager_settings_undo(
    const struct manager_storage_io *io, struct manager_settings_state *state,
    int ftp_listening);

/* Status strings for manager_status: narrow glyph set only (A-Z 0-9 space
 * . - _), never ':' (manager_display_text folds it to '?').  "SAVE OK",
 * "SAVE FAILED - BAD FILE", "SAVE FAILED - NO FREE BACKUP SLOT",
 * "SAVE FAILED - IO ERROR", "UNDO REFUSED - BAD ARCHIVE", ... */
void manager_settings_result_text(const struct manager_settings_result *result,
                                  char *out, size_t capacity);
/* "PORT.INI ABSENT" | "PORT.INI OK" | "PORT.INI OK - N LINES IGNORED" |
 * "PORT.INI INVALID - DEFAULTS SHOWN" | "PORT.INI IO ERROR". */
void manager_settings_port_ini_text(const struct manager_settings_state *state,
                                    char *out, size_t capacity);

/* Head (first 16 KiB) of the per-boot game log; path NULL selects
 * MANAGER_SETTINGS_LOG_PATH.  Returns the storage result of the read; kind
 * is NO_LOG unless the read succeeded and a "build " or receipt line was
 * found.  A line cut by the 16 KiB cap is ignored. */
int manager_settings_read_last_boot(const struct manager_storage_io *io,
                                    const char *path,
                                    struct manager_settings_last_boot *out);
/* Receipt with status ok/absent: "FPS 60 REMAP DEFAULT", "FPS 45 IGNORED
 * REMAP CUSTOM" (the game logged fps_lock=45(ignored:...)); any other status
 * (schema, garbage, too-large, io-error): "FILE SCHEMA - DEFAULTS USED";
 * "GAME BUILD IGNORES PORT.INI" | "NO LOG". */
void manager_settings_last_boot_text(
    const struct manager_settings_last_boot *last_boot, char *out,
    size_t capacity);

/* 1 when text uses only A-Z 0-9 space . - _ (the manager_display_text
 * pass-through set). */
int manager_settings_text_is_narrow(const char *text);

#endif /* MANAGER_SETTINGS_UI */

#endif
