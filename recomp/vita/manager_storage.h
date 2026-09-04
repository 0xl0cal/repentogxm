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

#endif
