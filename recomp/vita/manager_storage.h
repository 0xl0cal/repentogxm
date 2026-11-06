#ifndef ISAAC_MANAGER_STORAGE_H
#define ISAAC_MANAGER_STORAGE_H

#include <stddef.h>
#include <stdint.h>

#define MANAGER_STORAGE_ROOT "ux0:/data/isaacr001"
#define MANAGER_STORAGE_MOD_ROOT MANAGER_STORAGE_ROOT "/mods"
#define MANAGER_STORAGE_SAVE_ROOT                                              \
    MANAGER_STORAGE_ROOT "/Documents/My Games/Binding of Isaac Repentance"
#define MANAGER_STORAGE_BACKUP_ROOT MANAGER_STORAGE_ROOT "/.manager-backups"
#define MANAGER_STORAGE_SYNC_BACKUP_ROOT MANAGER_STORAGE_ROOT "/.sync-backups"

#define MANAGER_STORAGE_PATH_CAPACITY 512u
#define MANAGER_STORAGE_NAME_CAPACITY 128u
#define MANAGER_STORAGE_LABEL_CAPACITY 96u
#define MANAGER_STORAGE_MAX_SAVE_BYTES (8u * 1024u * 1024u)

enum manager_storage_result {
    MANAGER_STORAGE_OK = 0,
    MANAGER_STORAGE_SYNC_WARNING = 1,
    MANAGER_STORAGE_INVALID = -1,
    MANAGER_STORAGE_IO_ERROR = -2,
    MANAGER_STORAGE_CONFLICT = -3,
    MANAGER_STORAGE_HASH_MISMATCH = -4,
    MANAGER_STORAGE_TOO_LARGE = -5,
    MANAGER_STORAGE_CAPACITY = -6,
#if defined(MANAGER_SETTINGS_UI)
    /* Text primitives only: the path does not exist (stat_path returned 1).
     * Positive like SYNC_WARNING so `result < 0` still means failure. */
    MANAGER_STORAGE_ABSENT = 2,
#endif
};

enum manager_storage_node_kind {
    MANAGER_STORAGE_NODE_FILE = 1,
    MANAGER_STORAGE_NODE_DIRECTORY = 2,
    MANAGER_STORAGE_NODE_OTHER = 3,
};

struct manager_storage_dir_entry {
    char name[MANAGER_STORAGE_NAME_CAPACITY];
    enum manager_storage_node_kind kind;
    uint64_t size;
};

/* stat_path returns 0 for a node, 1 when absent, and a negative value on I/O
 * failure. dir_read returns 1 for an entry, 0 at EOF, and negative on failure. */
struct manager_storage_io {
    void *context;
    int (*stat_path)(void *context, const char *path,
                     enum manager_storage_node_kind *kind, uint64_t *size);
    int (*open_read)(void *context, const char *path);
    int (*open_write_exclusive)(void *context, const char *path);
    int (*read)(void *context, int handle, void *data, size_t size);
    int (*write)(void *context, int handle, const void *data, size_t size);
    int (*sync_file)(void *context, int handle);
    int (*close_file)(void *context, int handle);
    int (*remove_file)(void *context, const char *path);
    int (*rename_path)(void *context, const char *source,
                       const char *destination);
    int (*make_directory)(void *context, const char *path);
    int (*sync_device)(void *context);
    int (*open_directory)(void *context, const char *path);
    int (*read_directory)(void *context, int handle,
                          struct manager_storage_dir_entry *entry);
    int (*close_directory)(void *context, int handle);
};

struct manager_mod_entry {
    char name[MANAGER_STORAGE_NAME_CAPACITY];
    int disabled;
};

struct manager_save_slot {
    int slot;
    int present;
    uint64_t size;
#if defined(ISAAC_VITA_MANAGER_SAVE_GAMESTATE)
    /* gamestateN.dat (the per-slot run state) next to the persistent file. */
    int gamestate_present;
    uint64_t gamestate_size;
#endif
};

enum manager_save_backup_origin {
    MANAGER_SAVE_BACKUP_DATED = 1,
    MANAGER_SAVE_BACKUP_LOCAL = 2,
    MANAGER_SAVE_BACKUP_SYNC = 3,
};

struct manager_save_backup {
    int slot;
    enum manager_save_backup_origin origin;
    uint64_t size;
    char label[MANAGER_STORAGE_LABEL_CAPACITY];
    char path[MANAGER_STORAGE_PATH_CAPACITY];
#if defined(ISAAC_VITA_MANAGER_SAVE_GAMESTATE)
    /* A LOCAL backup "<prefix>N-NNNN.dat" may carry the slot's run state as
     * "<prefix>N-NNNN.gamestate.dat". Dated and sync backups never do. */
    int has_gamestate;
    uint64_t gamestate_size;
#endif
};

int manager_storage_list_mods(const struct manager_storage_io *io,
                              struct manager_mod_entry *entries,
                              size_t capacity, size_t *count,
                              size_t *rejected);
int manager_storage_set_mod_enabled(const struct manager_storage_io *io,
                                    const char *name, int enabled);
int manager_storage_get_save_slots(const struct manager_storage_io *io,
                                   struct manager_save_slot slots[3]);
int manager_storage_list_save_backups(const struct manager_storage_io *io,
                                      struct manager_save_backup *entries,
                                      size_t capacity, size_t *count,
                                      size_t *rejected);
int manager_storage_backup_slot(const struct manager_storage_io *io, int slot,
                                struct manager_save_backup *created);
int manager_storage_restore_backup(const struct manager_storage_io *io,
                                   const struct manager_save_backup *backup);
int manager_storage_recover(const struct manager_storage_io *io);

#if defined(MANAGER_SETTINGS_UI)
/* Manager-owned text files (the settings store):
 * port.ini lives directly under MANAGER_STORAGE_ROOT; its
 * sidecars are <name>.tmp (O_EXCL in-flight temp) and <name>.rollback
 * (in-flight marker, exists only during a promotion); previous versions are
 * archived as MANAGER_STORAGE_BACKUP_ROOT "/<prefix>-NNNN<ext>" and a
 * quarantined file as "/<prefix>-recovered-NNNN<ext>".  Renames onto an
 * existing path are never issued.  The runtime opens only <name>. */
#define MANAGER_STORAGE_PORT_INI_NAME "port.ini"
#define MANAGER_STORAGE_PORT_INI_PATH \
    MANAGER_STORAGE_ROOT "/" MANAGER_STORAGE_PORT_INI_NAME
#define MANAGER_STORAGE_TEXT_INDEX_MAX 9999u
#define MANAGER_STORAGE_TEXT_MAX_BYTES (64u * 1024u)

/* Whole file into buffer[0..capacity-1), NUL-terminated, *length set.
 * OK, ABSENT, CONFLICT (not a regular file), TOO_LARGE (more than
 * capacity-1 bytes; nothing is kept), IO_ERROR, INVALID. */
int manager_storage_read_text(const struct manager_storage_io *io,
                              const char *path, char *buffer,
                              size_t capacity, size_t *length);
/* Like read_text but a longer file is cut at capacity-1 bytes and reported
 * through *truncated (may be NULL) instead of TOO_LARGE. */
int manager_storage_read_head(const struct manager_storage_io *io,
                              const char *path, char *buffer,
                              size_t capacity, size_t *length,
                              int *truncated);
/* Section 1.3 write protocol for MANAGER_STORAGE_ROOT "/<name>": recover
 * the name's sidecars, reserve the archive index, O_EXCL temp -> write ->
 * sync -> close -> SHA-256 readback, <name> -> <name>.rollback (when it
 * existed) -> sync, temp -> <name> -> re-hash -> sync, rollback -> archive
 * -> sync.  Any failure restores the previous file and removes the temp.
 * OK, SYNC_WARNING (only the post-archive sync failed), CAPACITY (no free
 * archive index), CONFLICT (a sidecar or <name> is not a regular file, or
 * the temp name is taken by a file that appeared after the recovery step),
 * HASH_MISMATCH, TOO_LARGE, IO_ERROR (any other failure, a refused O_EXCL
 * open with no temp in the way included), INVALID. */
int manager_storage_write_text_atomic(const struct manager_storage_io *io,
                                      const char *name, const void *bytes,
                                      size_t length);
/* DEFAULTS: <name> -> next archive index (one rename) -> sync; leaves no
 * sidecar.  ABSENT when there was no file. */
int manager_storage_archive_and_remove(const struct manager_storage_io *io,
                                       const char *name);
/* First free MANAGER_STORAGE_BACKUP_ROOT "/<prefix>-NNNN<ext>" above the
 * newest existing index (0000 when none), so an archive written here is
 * always the newest; CAPACITY past MANAGER_STORAGE_TEXT_INDEX_MAX. */
int manager_storage_find_free_named(const struct manager_storage_io *io,
                                    const char *prefix, const char *extension,
                                    char *path, size_t capacity,
                                    unsigned int *selected);
/* Highest existing "/<prefix>-NNNN<ext>" in the backup root: OK with
 * *index and path, ABSENT when there is none (or no backup root). */
int manager_storage_newest_named(const struct manager_storage_io *io,
                                 const char *prefix, const char *extension,
                                 unsigned int *index, char *path,
                                 size_t capacity);
/* Folded into manager_storage_recover: a stranded port.ini.rollback wins
 * (any port.ini is quarantined to port-recovered-NNNN.ini first), then a
 * stale port.ini.tmp is removed.  Archives are never touched. */
int manager_storage_recover_port(const struct manager_storage_io *io);
#endif
#endif
