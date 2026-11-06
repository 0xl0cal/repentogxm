#include "manager_storage.h"

#include "manager_sha256.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define MANAGER_STORAGE_COPY_BUFFER 32768u

static int manager_storage_io_valid(const struct manager_storage_io *io)
{
    return io != NULL && io->stat_path != NULL && io->open_read != NULL &&
           io->open_write_exclusive != NULL && io->read != NULL &&
           io->write != NULL && io->sync_file != NULL &&
           io->close_file != NULL && io->remove_file != NULL &&
           io->rename_path != NULL && io->make_directory != NULL &&
           io->sync_device != NULL && io->open_directory != NULL &&
           io->read_directory != NULL && io->close_directory != NULL;
}

static int manager_storage_format(char *output, size_t capacity,
                                  const char *format, int slot,
                                  unsigned int index)
{
    const int length = snprintf(output, capacity, format, slot, index);
    return length >= 0 && (size_t)length < capacity ? MANAGER_STORAGE_OK
                                                    : MANAGER_STORAGE_INVALID;
}

static int manager_storage_slot_path(char *output, size_t capacity, int slot)
{
    return manager_storage_format(output, capacity,
                                  MANAGER_STORAGE_SAVE_ROOT
                                  "/persistentgamedata%d.dat",
                                  slot, 0u);
}

static int manager_storage_restore_temp_path(char *output, size_t capacity,
                                             int slot)
{
    return manager_storage_format(output, capacity,
                                  MANAGER_STORAGE_SAVE_ROOT
                                  "/.manager-restore-slot%d.tmp",
                                  slot, 0u);
}

static int manager_storage_rollback_path(char *output, size_t capacity,
                                         int slot)
{
    return manager_storage_format(output, capacity,
                                  MANAGER_STORAGE_SAVE_ROOT
                                  "/.manager-rollback-slot%d.dat",
                                  slot, 0u);
}

static int manager_storage_backup_temp_path(char *output, size_t capacity,
                                            int slot)
{
    return manager_storage_format(output, capacity,
                                  MANAGER_STORAGE_BACKUP_ROOT
                                  "/.manager-backup-slot%d.tmp",
                                  slot, 0u);
}
#if defined(ISAAC_VITA_MANAGER_SAVE_GAMESTATE)
/* Slot pair: persistentgamedataN.dat (mandatory) + gamestateN.dat (the run
 * state, optional). One backup index covers both files:
 *   <prefix>N-NNNN.dat            the persistent file
 *   <prefix>N-NNNN.gamestate.dat  its run state companion, when the slot had one
 * The companion name is 24 characters long, so the legacy 14-character
 * "slotN-NNNN.dat" parser never lists it on its own. */
static int manager_storage_gamestate_path(char *output, size_t capacity,
                                          int slot)
{
    return manager_storage_format(output, capacity,
                                  MANAGER_STORAGE_SAVE_ROOT "/gamestate%d.dat",
                                  slot, 0u);
}

static int manager_storage_gamestate_temp_path(char *output, size_t capacity,
                                               int slot)
{
    return manager_storage_format(output, capacity,
                                  MANAGER_STORAGE_SAVE_ROOT
                                  "/.manager-restore-slot%d.gamestate.tmp",
                                  slot, 0u);
}

static int manager_storage_pair_rollback_path(char *output, size_t capacity,
                                              int slot)
{
    return manager_storage_format(output, capacity,
                                  MANAGER_STORAGE_SAVE_ROOT
                                  "/.manager-pair-rollback-slot%d.dat",
                                  slot, 0u);
}

static int manager_storage_pair_rollback_gamestate_path(char *output,
                                                        size_t capacity,
                                                        int slot)
{
    return manager_storage_format(
        output, capacity,
        MANAGER_STORAGE_SAVE_ROOT "/.manager-pair-rollback-slot%d.gamestate.dat",
        slot, 0u);
}

static int manager_storage_pair_marker_path(char *output, size_t capacity,
                                            int slot)
{
    return manager_storage_format(output, capacity,
                                  MANAGER_STORAGE_SAVE_ROOT
                                  "/.manager-pair-slot%d.inflight",
                                  slot, 0u);
}

static int manager_storage_backup_gamestate_temp_path(char *output,
                                                      size_t capacity, int slot)
{
    return manager_storage_format(output, capacity,
                                  MANAGER_STORAGE_BACKUP_ROOT
                                  "/.manager-backup-slot%d.gamestate.tmp",
                                  slot, 0u);
}

/* "<anything>.dat" -> "<anything>.gamestate.dat". */
static int manager_storage_companion_path(char *output, size_t capacity,
                                          const char *primary)
{
    const size_t length = strlen(primary);
    int written;
    if (length <= 4u || strcmp(primary + length - 4u, ".dat") != 0)
        return MANAGER_STORAGE_INVALID;
    written = snprintf(output, capacity, "%.*s.gamestate.dat",
                       (int)(length - 4u), primary);
    return written >= 0 && (size_t)written < capacity ? MANAGER_STORAGE_OK
                                                      : MANAGER_STORAGE_INVALID;
}

static int manager_storage_local_backup_path(char *output, size_t capacity,
                                             const char *prefix, int slot,
                                             unsigned int index)
{
    const int length = snprintf(output, capacity, "%s/%s%d-%04u.dat",
                                MANAGER_STORAGE_BACKUP_ROOT, prefix, slot,
                                index);
    return length >= 0 && (size_t)length < capacity ? MANAGER_STORAGE_OK
                                                    : MANAGER_STORAGE_INVALID;
}
#endif

static int manager_storage_safe_segment(const char *name)
{
    const unsigned char *cursor = (const unsigned char *)name;
    size_t length = 0u;

    if (name == NULL || name[0] == '\0' || strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0)
        return 0;
    while (*cursor != '\0') {
        if (*cursor < 0x20u || *cursor == 0x7fu || *cursor == '/' ||
            *cursor == '\\' || *cursor == ':')
            return 0;
        ++cursor;
        ++length;
        if (length >= MANAGER_STORAGE_NAME_CAPACITY)
            return 0;
    }
    return 1;
}

static int manager_storage_join(char *output, size_t capacity,
                                const char *root, const char *name,
                                const char *suffix)
{
    const int length = snprintf(output, capacity, "%s/%s%s", root, name,
                                suffix != NULL ? suffix : "");
    return length >= 0 && (size_t)length < capacity ? MANAGER_STORAGE_OK
                                                    : MANAGER_STORAGE_INVALID;
}

static int manager_storage_ascii_equal(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right))
            return 0;
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static int manager_storage_parse_dated_name(const char *name, int *slot)
{
    const char *tail;
    size_t index;

    if (name == NULL || strlen(name) < 8u + 1u + 23u)
        return 0;
    for (index = 0u; index < 8u; ++index) {
        if (!isdigit((unsigned char)name[index]))
            return 0;
    }
    if (name[8] != '.')
        return 0;
    tail = name + 9u;
    if (tolower((unsigned char)tail[0]) == 'r' &&
        tolower((unsigned char)tail[1]) == 'e' &&
        tolower((unsigned char)tail[2]) == 'p' && tail[3] == '_')
        tail += 4u;
    for (index = 1u; index <= 3u; ++index) {
        char expected[32];
        (void)snprintf(expected, sizeof(expected),
                       "persistentgamedata%u.dat", (unsigned int)index);
        if (manager_storage_ascii_equal(tail, expected)) {
            *slot = (int)index;
            return 1;
        }
    }
    return 0;
}

static int manager_storage_parse_local_name(const char *name, int *slot,
                                            unsigned int *index)
{
    unsigned int parsed_index;

    if (name == NULL || strlen(name) != 14u ||
        memcmp(name, "slot", 4u) != 0 || name[4] < '1' || name[4] > '3' ||
        name[5] != '-' || !isdigit((unsigned char)name[6]) ||
        !isdigit((unsigned char)name[7]) ||
        !isdigit((unsigned char)name[8]) ||
        !isdigit((unsigned char)name[9]) ||
        memcmp(name + 10u, ".dat", 4u) != 0)
        return 0;
    parsed_index = (unsigned int)(name[6] - '0') * 1000u +
                   (unsigned int)(name[7] - '0') * 100u +
                   (unsigned int)(name[8] - '0') * 10u +
                   (unsigned int)(name[9] - '0');
    *slot = name[4] - '0';
    if (index != NULL)
        *index = parsed_index;
    return 1;
}

static int manager_storage_stat(const struct manager_storage_io *io,
                                const char *path,
                                enum manager_storage_node_kind *kind,
                                uint64_t *size)
{
    enum manager_storage_node_kind local_kind = MANAGER_STORAGE_NODE_OTHER;
    uint64_t local_size = 0u;
    const int result = io->stat_path(io->context, path, &local_kind,
                                     &local_size);
    if (result == 0) {
        if (kind != NULL)
            *kind = local_kind;
        if (size != NULL)
            *size = local_size;
    }
    return result;
}

static int manager_storage_ensure_backup_root(
    const struct manager_storage_io *io)
{
    enum manager_storage_node_kind kind = MANAGER_STORAGE_NODE_OTHER;
    int result = manager_storage_stat(io, MANAGER_STORAGE_BACKUP_ROOT, &kind,
                                      NULL);
    if (result == 0)
        return kind == MANAGER_STORAGE_NODE_DIRECTORY ? MANAGER_STORAGE_OK
                                                      : MANAGER_STORAGE_CONFLICT;
    if (result < 0)
        return MANAGER_STORAGE_IO_ERROR;
    if (io->make_directory(io->context, MANAGER_STORAGE_BACKUP_ROOT) < 0)
        return MANAGER_STORAGE_IO_ERROR;
    if (io->sync_device(io->context) < 0)
        return MANAGER_STORAGE_SYNC_WARNING;
    result = manager_storage_stat(io, MANAGER_STORAGE_BACKUP_ROOT, &kind, NULL);
    return result == 0 && kind == MANAGER_STORAGE_NODE_DIRECTORY
               ? MANAGER_STORAGE_OK
               : MANAGER_STORAGE_IO_ERROR;
}

static int manager_storage_find_free_backup(
    const struct manager_storage_io *io, const char *prefix, int slot,
    char *path, size_t path_capacity, unsigned int *selected)
{
    unsigned int index;
    for (index = 0u; index <= 9999u; ++index) {
        enum manager_storage_node_kind kind;
        int result;
        const int length = snprintf(path, path_capacity, "%s/%s%d-%04u.dat",
                                    MANAGER_STORAGE_BACKUP_ROOT, prefix, slot,
                                    index);
        if (length < 0 || (size_t)length >= path_capacity)
            return MANAGER_STORAGE_INVALID;
        result = manager_storage_stat(io, path, &kind, NULL);
#if defined(ISAAC_VITA_MANAGER_SAVE_GAMESTATE)
        if (result == 1) {
            /* The index is shared by the pair: its companion must be free
             * too, so an orphaned companion can never be adopted. */
            char companion[MANAGER_STORAGE_PATH_CAPACITY];
            if (manager_storage_companion_path(companion, sizeof(companion),
                                               path) != MANAGER_STORAGE_OK)
                return MANAGER_STORAGE_INVALID;
            result = manager_storage_stat(io, companion, &kind, NULL);
        }
#endif
        if (result == 1) {
            if (selected != NULL)
                *selected = index;
            return MANAGER_STORAGE_OK;
        }
        if (result < 0)
            return MANAGER_STORAGE_IO_ERROR;
    }
    return MANAGER_STORAGE_CAPACITY;
}

static int manager_storage_hash_path(const struct manager_storage_io *io,
                                     const char *path,
                                     unsigned char digest[32],
                                     uint64_t *size_out)
{
    struct manager_sha256_context hash;
    unsigned char buffer[MANAGER_STORAGE_COPY_BUFFER];
    uint64_t total = 0u;
    int handle = io->open_read(io->context, path);
    int result = MANAGER_STORAGE_OK;

    if (handle < 0)
        return MANAGER_STORAGE_IO_ERROR;
    manager_sha256_init(&hash);
    for (;;) {
        const int amount = io->read(io->context, handle, buffer,
                                    sizeof(buffer));
        if (amount < 0) {
            result = MANAGER_STORAGE_IO_ERROR;
            break;
        }
        if (amount == 0)
            break;
        total += (uint64_t)amount;
        if (total > MANAGER_STORAGE_MAX_SAVE_BYTES) {
            result = MANAGER_STORAGE_TOO_LARGE;
            break;
        }
        manager_sha256_update(&hash, buffer, (size_t)amount);
    }
    if (io->close_file(io->context, handle) < 0 && result == MANAGER_STORAGE_OK)
        result = MANAGER_STORAGE_IO_ERROR;
    if (result != MANAGER_STORAGE_OK)
        return result;
    manager_sha256_final(&hash, digest);
    if (size_out != NULL)
        *size_out = total;
    return MANAGER_STORAGE_OK;
}

static int manager_storage_copy_verified(const struct manager_storage_io *io,
                                         const char *source,
                                         const char *temporary,
                                         unsigned char digest[32],
                                         uint64_t *size_out)
{
    struct manager_sha256_context hash;
    unsigned char buffer[MANAGER_STORAGE_COPY_BUFFER];
    unsigned char readback[32];
    uint64_t total = 0u;
    uint64_t readback_size = 0u;
    int source_handle = -1;
    int destination_handle = -1;
    int result = MANAGER_STORAGE_OK;

    source_handle = io->open_read(io->context, source);
    if (source_handle < 0)
        return MANAGER_STORAGE_IO_ERROR;
    destination_handle = io->open_write_exclusive(io->context, temporary);
    if (destination_handle < 0) {
        (void)io->close_file(io->context, source_handle);
        return MANAGER_STORAGE_CONFLICT;
    }
    manager_sha256_init(&hash);
    for (;;) {
        int amount = io->read(io->context, source_handle, buffer,
                              sizeof(buffer));
        size_t offset = 0u;
        if (amount < 0) {
            result = MANAGER_STORAGE_IO_ERROR;
            break;
        }
        if (amount == 0)
            break;
        total += (uint64_t)amount;
        if (total > MANAGER_STORAGE_MAX_SAVE_BYTES) {
            result = MANAGER_STORAGE_TOO_LARGE;
            break;
        }
        manager_sha256_update(&hash, buffer, (size_t)amount);
        while (offset < (size_t)amount) {
            const int written = io->write(io->context, destination_handle,
                                          buffer + offset,
                                          (size_t)amount - offset);
            if (written <= 0) {
                result = MANAGER_STORAGE_IO_ERROR;
                break;
            }
            offset += (size_t)written;
        }
        if (result != MANAGER_STORAGE_OK)
            break;
    }
    if (result == MANAGER_STORAGE_OK &&
        io->sync_file(io->context, destination_handle) < 0)
        result = MANAGER_STORAGE_IO_ERROR;
    if (io->close_file(io->context, destination_handle) < 0 &&
        result == MANAGER_STORAGE_OK)
        result = MANAGER_STORAGE_IO_ERROR;
    destination_handle = -1;
    if (io->close_file(io->context, source_handle) < 0 &&
        result == MANAGER_STORAGE_OK)
        result = MANAGER_STORAGE_IO_ERROR;
    source_handle = -1;
    if (result != MANAGER_STORAGE_OK) {
        (void)io->remove_file(io->context, temporary);
        return result;
    }
    manager_sha256_final(&hash, digest);
    result = manager_storage_hash_path(io, temporary, readback, &readback_size);
    if (result != MANAGER_STORAGE_OK || readback_size != total ||
        memcmp(digest, readback, sizeof(readback)) != 0) {
        (void)io->remove_file(io->context, temporary);
        return result == MANAGER_STORAGE_OK ? MANAGER_STORAGE_HASH_MISMATCH
                                            : result;
    }
    if (size_out != NULL)
        *size_out = total;
    return MANAGER_STORAGE_OK;
}

static int manager_storage_remove_managed_file(
    const struct manager_storage_io *io, const char *path)
{
    enum manager_storage_node_kind kind = MANAGER_STORAGE_NODE_OTHER;
    const int result = manager_storage_stat(io, path, &kind, NULL);
    if (result == 1)
        return MANAGER_STORAGE_OK;
    if (result < 0)
        return MANAGER_STORAGE_IO_ERROR;
    if (kind != MANAGER_STORAGE_NODE_FILE)
        return MANAGER_STORAGE_CONFLICT;
    return io->remove_file(io->context, path) < 0 ? MANAGER_STORAGE_IO_ERROR
                                                  : MANAGER_STORAGE_OK;
}

static void manager_storage_sort_mods(struct manager_mod_entry *entries,
                                      size_t count)
{
    size_t index;
    for (index = 1u; index < count; ++index) {
        struct manager_mod_entry value = entries[index];
        size_t cursor = index;
        while (cursor != 0u &&
               strcmp(entries[cursor - 1u].name, value.name) > 0) {
            entries[cursor] = entries[cursor - 1u];
            --cursor;
        }
        entries[cursor] = value;
    }
}

int manager_storage_list_mods(const struct manager_storage_io *io,
                              struct manager_mod_entry *entries,
                              size_t capacity, size_t *count,
                              size_t *rejected)
{
    enum manager_storage_node_kind root_kind = MANAGER_STORAGE_NODE_OTHER;
    int directory;
    int result = MANAGER_STORAGE_OK;
    int root_status;
    size_t used = 0u;
    size_t skipped = 0u;

    if (!manager_storage_io_valid(io) || entries == NULL || count == NULL ||
        rejected == NULL)
        return MANAGER_STORAGE_INVALID;
    *count = 0u;
    *rejected = 0u;
    root_status = manager_storage_stat(io, MANAGER_STORAGE_MOD_ROOT,
                                       &root_kind, NULL);
    if (root_status == 1) {
        *count = 0u;
        *rejected = 0u;
        return MANAGER_STORAGE_OK;
    }
    if (root_status < 0)
        return MANAGER_STORAGE_IO_ERROR;
    if (root_kind != MANAGER_STORAGE_NODE_DIRECTORY)
        return MANAGER_STORAGE_CONFLICT;
    directory = io->open_directory(io->context, MANAGER_STORAGE_MOD_ROOT);
    if (directory < 0) {
        *count = 0u;
        *rejected = 0u;
        return MANAGER_STORAGE_IO_ERROR;
    }
    for (;;) {
        struct manager_storage_dir_entry entry;
        char marker[MANAGER_STORAGE_PATH_CAPACITY];
        enum manager_storage_node_kind marker_kind =
            MANAGER_STORAGE_NODE_OTHER;
        int read_result;
        int stat_result;

        memset(&entry, 0, sizeof(entry));
        read_result = io->read_directory(io->context, directory, &entry);
        if (read_result <= 0) {
            if (read_result < 0)
                result = MANAGER_STORAGE_IO_ERROR;
            break;
        }
        if (entry.kind != MANAGER_STORAGE_NODE_DIRECTORY ||
            !manager_storage_safe_segment(entry.name)) {
            if (strcmp(entry.name, ".") != 0 && strcmp(entry.name, "..") != 0)
                ++skipped;
            continue;
        }
        if (manager_storage_join(marker, sizeof(marker),
                                 MANAGER_STORAGE_MOD_ROOT, entry.name,
                                 "/disable.it") != MANAGER_STORAGE_OK) {
            ++skipped;
            continue;
        }
        stat_result = manager_storage_stat(io, marker, &marker_kind, NULL);
        if (stat_result < 0) {
            result = MANAGER_STORAGE_IO_ERROR;
            break;
        }
        if (stat_result == 0 && marker_kind != MANAGER_STORAGE_NODE_FILE) {
            ++skipped;
            continue;
        }
        if (used == capacity) {
            ++skipped;
            continue;
        }
        (void)snprintf(entries[used].name, sizeof(entries[used].name), "%s",
                       entry.name);
        entries[used].disabled = stat_result == 0;
        ++used;
    }
    if (io->close_directory(io->context, directory) < 0 &&
        result == MANAGER_STORAGE_OK)
        result = MANAGER_STORAGE_IO_ERROR;
    if (result < 0)
        used = 0u;
    manager_storage_sort_mods(entries, used);
    *count = used;
    *rejected = skipped;
    return result;
}

int manager_storage_set_mod_enabled(const struct manager_storage_io *io,
                                    const char *name, int enabled)
{
    char directory[MANAGER_STORAGE_PATH_CAPACITY];
    char marker[MANAGER_STORAGE_PATH_CAPACITY];
    char stash[MANAGER_STORAGE_PATH_CAPACITY];
    enum manager_storage_node_kind directory_kind =
        MANAGER_STORAGE_NODE_OTHER;
    enum manager_storage_node_kind marker_kind = MANAGER_STORAGE_NODE_OTHER;
    enum manager_storage_node_kind stash_kind = MANAGER_STORAGE_NODE_OTHER;
    int marker_status;
    int stash_status;

    if (!manager_storage_io_valid(io) || !manager_storage_safe_segment(name) ||
        manager_storage_join(directory, sizeof(directory),
                             MANAGER_STORAGE_MOD_ROOT, name, NULL) !=
            MANAGER_STORAGE_OK ||
        manager_storage_join(marker, sizeof(marker), MANAGER_STORAGE_MOD_ROOT,
                             name, "/disable.it") != MANAGER_STORAGE_OK ||
        manager_storage_join(stash, sizeof(stash), MANAGER_STORAGE_MOD_ROOT,
                             name, "/.isaac-manager-disable-marker") !=
            MANAGER_STORAGE_OK)
        return MANAGER_STORAGE_INVALID;
    if (manager_storage_stat(io, directory, &directory_kind, NULL) != 0 ||
        directory_kind != MANAGER_STORAGE_NODE_DIRECTORY)
        return MANAGER_STORAGE_INVALID;
    marker_status = manager_storage_stat(io, marker, &marker_kind, NULL);
    stash_status = manager_storage_stat(io, stash, &stash_kind, NULL);
    if (marker_status < 0 || stash_status < 0)
        return MANAGER_STORAGE_IO_ERROR;
    if ((marker_status == 0 && marker_kind != MANAGER_STORAGE_NODE_FILE) ||
        (stash_status == 0 && stash_kind != MANAGER_STORAGE_NODE_FILE))
        return MANAGER_STORAGE_CONFLICT;

    if (enabled) {
        if (marker_status == 1)
            return MANAGER_STORAGE_OK;
        if (stash_status == 0)
            return MANAGER_STORAGE_CONFLICT;
        if (io->rename_path(io->context, marker, stash) < 0)
            return MANAGER_STORAGE_IO_ERROR;
        if (io->sync_device(io->context) < 0) {
            (void)io->rename_path(io->context, stash, marker);
            return MANAGER_STORAGE_IO_ERROR;
        }
        return MANAGER_STORAGE_OK;
    }

    if (marker_status == 0)
        return MANAGER_STORAGE_OK;
    if (stash_status == 0) {
        if (io->rename_path(io->context, stash, marker) < 0)
            return MANAGER_STORAGE_IO_ERROR;
        if (io->sync_device(io->context) < 0) {
            (void)io->rename_path(io->context, marker, stash);
            return MANAGER_STORAGE_IO_ERROR;
        }
        return MANAGER_STORAGE_OK;
    }
    {
        const int handle = io->open_write_exclusive(io->context, marker);
        int result = MANAGER_STORAGE_OK;
        if (handle < 0)
            return MANAGER_STORAGE_CONFLICT;
        if (io->sync_file(io->context, handle) < 0)
            result = MANAGER_STORAGE_IO_ERROR;
        if (io->close_file(io->context, handle) < 0)
            result = MANAGER_STORAGE_IO_ERROR;
        if (result != MANAGER_STORAGE_OK) {
            (void)io->remove_file(io->context, marker);
            return result;
        }
        return io->sync_device(io->context) < 0
                   ? MANAGER_STORAGE_SYNC_WARNING
                   : MANAGER_STORAGE_OK;
    }
}

int manager_storage_get_save_slots(const struct manager_storage_io *io,
                                   struct manager_save_slot slots[3])
{
    int slot;
    if (!manager_storage_io_valid(io) || slots == NULL)
        return MANAGER_STORAGE_INVALID;
    for (slot = 1; slot <= 3; ++slot) {
        char path[MANAGER_STORAGE_PATH_CAPACITY];
        enum manager_storage_node_kind kind;
        uint64_t size = 0u;
        const int path_result = manager_storage_slot_path(path, sizeof(path),
                                                          slot);
        int stat_result;
        if (path_result != MANAGER_STORAGE_OK)
            return path_result;
        stat_result = manager_storage_stat(io, path, &kind, &size);
        if (stat_result < 0)
            return MANAGER_STORAGE_IO_ERROR;
        if (stat_result == 0 && kind != MANAGER_STORAGE_NODE_FILE)
            return MANAGER_STORAGE_CONFLICT;
        slots[slot - 1].slot = slot;
        slots[slot - 1].present = stat_result == 0;
        slots[slot - 1].size = stat_result == 0 ? size : 0u;
#if defined(ISAAC_VITA_MANAGER_SAVE_GAMESTATE)
        if (manager_storage_gamestate_path(path, sizeof(path), slot) !=
            MANAGER_STORAGE_OK)
            return MANAGER_STORAGE_INVALID;
        size = 0u;
        stat_result = manager_storage_stat(io, path, &kind, &size);
        if (stat_result < 0)
            return MANAGER_STORAGE_IO_ERROR;
        if (stat_result == 0 && kind != MANAGER_STORAGE_NODE_FILE)
            return MANAGER_STORAGE_CONFLICT;
        slots[slot - 1].gamestate_present = stat_result == 0;
        slots[slot - 1].gamestate_size = stat_result == 0 ? size : 0u;
#endif
    }
    return MANAGER_STORAGE_OK;
}

static int manager_storage_add_backup(struct manager_save_backup *entries,
                                      size_t capacity, size_t *used,
                                      size_t *skipped, int slot,
                                      enum manager_save_backup_origin origin,
                                      uint64_t size, const char *label,
                                      const char *path)
{
    int label_length;
    int path_length;

    if (*used == capacity) {
        ++*skipped;
        return MANAGER_STORAGE_OK;
    }
    entries[*used].slot = slot;
    entries[*used].origin = origin;
    entries[*used].size = size;
    label_length = snprintf(entries[*used].label,
                            sizeof(entries[*used].label), "%s", label);
    path_length = snprintf(entries[*used].path,
                           sizeof(entries[*used].path), "%s", path);
    if (label_length < 0 ||
        (size_t)label_length >= sizeof(entries[*used].label) ||
        path_length < 0 ||
        (size_t)path_length >= sizeof(entries[*used].path))
        return MANAGER_STORAGE_INVALID;
    ++*used;
    return MANAGER_STORAGE_OK;
}

static void manager_storage_sort_backups(struct manager_save_backup *entries,
                                         size_t count)
{
    size_t index;
    for (index = 1u; index < count; ++index) {
        struct manager_save_backup value = entries[index];
        size_t cursor = index;
        while (cursor != 0u &&
               strcmp(entries[cursor - 1u].label, value.label) < 0) {
            entries[cursor] = entries[cursor - 1u];
            --cursor;
        }
        entries[cursor] = value;
    }
}

static int manager_storage_scan_flat_backups(
    const struct manager_storage_io *io, const char *root,
    enum manager_save_backup_origin origin,
    struct manager_save_backup *entries, size_t capacity, size_t *used,
    size_t *skipped)
{
    enum manager_storage_node_kind root_kind = MANAGER_STORAGE_NODE_OTHER;
    int directory;
    int result = MANAGER_STORAGE_OK;
    const int root_status = manager_storage_stat(io, root, &root_kind, NULL);
    if (root_status == 1)
        return MANAGER_STORAGE_OK;
    if (root_status < 0)
        return MANAGER_STORAGE_IO_ERROR;
    if (root_kind != MANAGER_STORAGE_NODE_DIRECTORY)
        return MANAGER_STORAGE_CONFLICT;
    directory = io->open_directory(io->context, root);
    if (directory < 0)
        return MANAGER_STORAGE_IO_ERROR;
    for (;;) {
        struct manager_storage_dir_entry entry;
        char path[MANAGER_STORAGE_PATH_CAPACITY];
        char label[MANAGER_STORAGE_LABEL_CAPACITY];
        int slot = 0;
        int read_result;
        memset(&entry, 0, sizeof(entry));
        read_result = io->read_directory(io->context, directory, &entry);
        if (read_result <= 0) {
            if (read_result < 0)
                result = MANAGER_STORAGE_IO_ERROR;
            break;
        }
        if (entry.kind != MANAGER_STORAGE_NODE_FILE ||
            !manager_storage_safe_segment(entry.name) ||
            (origin == MANAGER_SAVE_BACKUP_DATED
                 ? !manager_storage_parse_dated_name(entry.name, &slot)
                 : !manager_storage_parse_local_name(entry.name, &slot,
                                                     NULL))) {
            continue;
        }
        if (entry.size > MANAGER_STORAGE_MAX_SAVE_BYTES ||
            manager_storage_join(path, sizeof(path), root, entry.name, NULL) !=
                MANAGER_STORAGE_OK) {
            ++*skipped;
            continue;
        }
        (void)snprintf(label, sizeof(label), "%s S%d %.72s",
                       origin == MANAGER_SAVE_BACKUP_DATED ? "DATED" : "LOCAL",
                       slot, entry.name);
#if defined(ISAAC_VITA_MANAGER_SAVE_GAMESTATE)
        {
            const size_t before = *used;
            int has_gamestate = 0;
            uint64_t gamestate_size = 0u;
            if (origin == MANAGER_SAVE_BACKUP_LOCAL) {
                char companion[MANAGER_STORAGE_PATH_CAPACITY];
                enum manager_storage_node_kind companion_kind =
                    MANAGER_STORAGE_NODE_OTHER;
                uint64_t companion_size = 0u;
                int companion_status;
                if (manager_storage_companion_path(companion, sizeof(companion),
                                                   path) != MANAGER_STORAGE_OK) {
                    ++*skipped;
                    continue;
                }
                companion_status = manager_storage_stat(io, companion,
                                                        &companion_kind,
                                                        &companion_size);
                if (companion_status < 0) {
                    result = MANAGER_STORAGE_IO_ERROR;
                    break;
                }
                if (companion_status == 0 &&
                    companion_kind == MANAGER_STORAGE_NODE_FILE &&
                    companion_size <= MANAGER_STORAGE_MAX_SAVE_BYTES) {
                    has_gamestate = 1;
                    gamestate_size = companion_size;
                    /* The row filter keeps only A-Z 0-9 space . - _ so the
                     * flag avoids '+' and ':'. */
                    (void)snprintf(label, sizeof(label),
                                   "LOCAL S%d %.60s WITH RUN", slot,
                                   entry.name);
                }
            }
            result = manager_storage_add_backup(entries, capacity, used,
                                                skipped, slot, origin,
                                                entry.size, label, path);
            if (result == MANAGER_STORAGE_OK && *used > before) {
                entries[*used - 1u].has_gamestate = has_gamestate;
                entries[*used - 1u].gamestate_size = gamestate_size;
            }
        }
#else
        result = manager_storage_add_backup(entries, capacity, used, skipped,
                                            slot, origin, entry.size, label,
                                            path);
#endif
        if (result != MANAGER_STORAGE_OK)
            break;
    }
    if (io->close_directory(io->context, directory) < 0 &&
        result == MANAGER_STORAGE_OK)
        result = MANAGER_STORAGE_IO_ERROR;
    return result;
}

static int manager_storage_scan_sync_backups(
    const struct manager_storage_io *io, struct manager_save_backup *entries,
    size_t capacity, size_t *used, size_t *skipped)
{
    enum manager_storage_node_kind root_kind = MANAGER_STORAGE_NODE_OTHER;
    int directory;
    int result = MANAGER_STORAGE_OK;
    const int root_status = manager_storage_stat(
        io, MANAGER_STORAGE_SYNC_BACKUP_ROOT, &root_kind, NULL);
    if (root_status == 1)
        return MANAGER_STORAGE_OK;
    if (root_status < 0)
        return MANAGER_STORAGE_IO_ERROR;
    if (root_kind != MANAGER_STORAGE_NODE_DIRECTORY)
        return MANAGER_STORAGE_CONFLICT;
    directory =
        io->open_directory(io->context, MANAGER_STORAGE_SYNC_BACKUP_ROOT);
    if (directory < 0)
        return MANAGER_STORAGE_IO_ERROR;
    for (;;) {
        struct manager_storage_dir_entry entry;
        int read_result;
        int slot;
        memset(&entry, 0, sizeof(entry));
        read_result = io->read_directory(io->context, directory, &entry);
        if (read_result <= 0) {
            if (read_result < 0)
                result = MANAGER_STORAGE_IO_ERROR;
            break;
        }
        if (entry.kind != MANAGER_STORAGE_NODE_DIRECTORY ||
            !manager_storage_safe_segment(entry.name))
            continue;
        for (slot = 1; slot <= 3; ++slot) {
            char path[MANAGER_STORAGE_PATH_CAPACITY];
            char label[MANAGER_STORAGE_LABEL_CAPACITY];
            enum manager_storage_node_kind kind =
                MANAGER_STORAGE_NODE_OTHER;
            uint64_t size = 0u;
            const int length = snprintf(
                path, sizeof(path),
                "%s/%s/Documents/My Games/Binding of Isaac Repentance/"
                "persistentgamedata%d.dat",
                MANAGER_STORAGE_SYNC_BACKUP_ROOT, entry.name, slot);
            int stat_result;
            if (length < 0 || (size_t)length >= sizeof(path)) {
                ++*skipped;
                continue;
            }
            stat_result = manager_storage_stat(io, path, &kind, &size);
            if (stat_result < 0) {
                result = MANAGER_STORAGE_IO_ERROR;
                break;
            }
            if (stat_result == 1 || kind != MANAGER_STORAGE_NODE_FILE)
                continue;
            if (size > MANAGER_STORAGE_MAX_SAVE_BYTES) {
                ++*skipped;
                continue;
            }
            (void)snprintf(label, sizeof(label), "SYNC S%d %.72s", slot,
                           entry.name);
            result = manager_storage_add_backup(
                entries, capacity, used, skipped, slot,
                MANAGER_SAVE_BACKUP_SYNC, size, label, path);
            if (result != MANAGER_STORAGE_OK)
                break;
        }
        if (result != MANAGER_STORAGE_OK)
            break;
    }
    if (io->close_directory(io->context, directory) < 0 &&
        result == MANAGER_STORAGE_OK)
        result = MANAGER_STORAGE_IO_ERROR;
    return result;
}

int manager_storage_list_save_backups(const struct manager_storage_io *io,
                                      struct manager_save_backup *entries,
                                      size_t capacity, size_t *count,
                                      size_t *rejected)
{
    size_t used = 0u;
    size_t skipped = 0u;
    int result;
    if (!manager_storage_io_valid(io) || entries == NULL || count == NULL ||
        rejected == NULL)
        return MANAGER_STORAGE_INVALID;
    *count = 0u;
    *rejected = 0u;
    result = manager_storage_scan_flat_backups(
        io, MANAGER_STORAGE_SAVE_ROOT, MANAGER_SAVE_BACKUP_DATED, entries,
        capacity, &used, &skipped);
    if (result == MANAGER_STORAGE_OK)
        result = manager_storage_scan_flat_backups(
            io, MANAGER_STORAGE_BACKUP_ROOT, MANAGER_SAVE_BACKUP_LOCAL, entries,
            capacity, &used, &skipped);
    if (result == MANAGER_STORAGE_OK)
        result = manager_storage_scan_sync_backups(
            io, entries, capacity, &used, &skipped);
    if (result < 0)
        used = 0u;
    manager_storage_sort_backups(entries, used);
    *count = used;
    *rejected = skipped;
    return result;
}

static int manager_storage_local_path_allowed(const char *path, int slot)
{
    const size_t prefix = sizeof(MANAGER_STORAGE_BACKUP_ROOT) - 1u;
    int parsed_slot;
    if (strncmp(path, MANAGER_STORAGE_BACKUP_ROOT "/", prefix + 1u) != 0 ||
        !manager_storage_parse_local_name(path + prefix + 1u, &parsed_slot,
                                          NULL))
        return 0;
    return parsed_slot == slot;
}

static int manager_storage_dated_path_allowed(const char *path, int slot)
{
    const size_t prefix = sizeof(MANAGER_STORAGE_SAVE_ROOT) - 1u;
    int parsed_slot;
    if (strncmp(path, MANAGER_STORAGE_SAVE_ROOT "/", prefix + 1u) != 0 ||
        !manager_storage_parse_dated_name(path + prefix + 1u, &parsed_slot))
        return 0;
    return parsed_slot == slot;
}

static int manager_storage_sync_path_allowed(const char *path, int slot)
{
    const size_t prefix = sizeof(MANAGER_STORAGE_SYNC_BACKUP_ROOT) - 1u;
    const char *session;
    const char *slash;
    char expected[160];
    if (strncmp(path, MANAGER_STORAGE_SYNC_BACKUP_ROOT "/", prefix + 1u) != 0)
        return 0;
    session = path + prefix + 1u;
    slash = strchr(session, '/');
    if (slash == NULL || slash == session ||
        (size_t)(slash - session) >= MANAGER_STORAGE_NAME_CAPACITY)
        return 0;
    {
        char name[MANAGER_STORAGE_NAME_CAPACITY];
        memcpy(name, session, (size_t)(slash - session));
        name[slash - session] = '\0';
        if (!manager_storage_safe_segment(name))
            return 0;
    }
    (void)snprintf(expected, sizeof(expected),
                   "/Documents/My Games/Binding of Isaac Repentance/"
                   "persistentgamedata%d.dat",
                   slot);
    return strcmp(slash, expected) == 0;
}

static int manager_storage_backup_allowed(
    const struct manager_save_backup *backup)
{
    if (backup == NULL || backup->slot < 1 || backup->slot > 3 ||
        backup->size > MANAGER_STORAGE_MAX_SAVE_BYTES)
        return 0;
    if (backup->origin == MANAGER_SAVE_BACKUP_DATED)
        return manager_storage_dated_path_allowed(backup->path, backup->slot);
    if (backup->origin == MANAGER_SAVE_BACKUP_LOCAL)
        return manager_storage_local_path_allowed(backup->path, backup->slot);
    if (backup->origin == MANAGER_SAVE_BACKUP_SYNC)
        return manager_storage_sync_path_allowed(backup->path, backup->slot);
    return 0;
}

#if defined(ISAAC_VITA_MANAGER_SAVE_GAMESTATE)
/* Pair restore protocol. Both live files are replaced under one in-flight
 * marker so a crash at any point leaves a state the next recover() maps back
 * to the pre-restore slot ("old wins"), never a half-restored pair:
 *
 *   1  stage verified copies      .manager-restore-slotN.tmp / .gamestate.tmp
 *   2  write + fsync the marker    .manager-pair-slotN.inflight = "PG0007"
 *                                  byte 0: 'P' old persistent existed, 'p' none
 *                                  byte 1: 'G' old run state existed, 'g' none,
 *                                          '-' run state not part of the restore
 *                                  bytes 2-5: archive index reserved for the pair
 *   3  old files aside             persistent -> .manager-pair-rollback-slotN.dat
 *                                  gamestate  -> .manager-pair-rollback-slotN.gamestate.dat
 *   4  promote the staged copies, re-hash both, fsync
 *   5  archive the rollbacks       -> slotN-0007.gamestate.dat, then slotN-0007.dat
 *   6  fsync, remove the marker (the commit), fsync
 *
 * While the marker exists every side is decodable on its own from the flag,
 * the rollback file and the reserved archive name: rollback present -> the
 * aside happened, promote it back; rollback absent but the archive name is
 * taken -> step 5 ran for that side, move the archive back; neither -> the
 * aside has not happened yet and the live file is still the old one; flag
 * 'p'/'g' -> whatever is live is a promoted copy and is set aside. The two
 * sides never need to be ordered against each other, so a crash between the
 * two renames of any step is covered. Without the marker nothing is
 * in flight: a completed restore leaves only the archived pair. Live files
 * displaced by recovery are quarantined as recovered-slotN-NNNN[.gamestate].dat
 * (recovery cannot know where a promoted copy came from); the running restore
 * removes them instead because it knows they are copies of a source that is
 * still present. Backups publish the companion before the .dat, so a listable
 * backup never appears without the run state it was made with; a crash
 * between the two leaves an unlisted companion whose index the pair-aware
 * find_free_backup skips forever. */

struct manager_storage_pair_paths {
    char persistent[MANAGER_STORAGE_PATH_CAPACITY];
    char gamestate[MANAGER_STORAGE_PATH_CAPACITY];
    char persistent_temp[MANAGER_STORAGE_PATH_CAPACITY];
    char gamestate_temp[MANAGER_STORAGE_PATH_CAPACITY];
    char persistent_rollback[MANAGER_STORAGE_PATH_CAPACITY];
    char gamestate_rollback[MANAGER_STORAGE_PATH_CAPACITY];
    char marker[MANAGER_STORAGE_PATH_CAPACITY];
};

struct manager_storage_pair_marker {
    char persistent; /* 'P' or 'p' */
    char gamestate;  /* 'G', 'g' or '-' */
    int has_index;
    unsigned int index;
};

struct manager_storage_quarantine {
    int selected;
    char primary[MANAGER_STORAGE_PATH_CAPACITY];
};

#define MANAGER_STORAGE_PAIR_MARKER_BYTES 6u

static int manager_storage_pair_paths_init(
    struct manager_storage_pair_paths *paths, int slot)
{
    if (manager_storage_slot_path(paths->persistent, sizeof(paths->persistent),
                                  slot) != MANAGER_STORAGE_OK ||
        manager_storage_gamestate_path(paths->gamestate,
                                       sizeof(paths->gamestate), slot) !=
            MANAGER_STORAGE_OK ||
        manager_storage_restore_temp_path(paths->persistent_temp,
                                          sizeof(paths->persistent_temp),
                                          slot) != MANAGER_STORAGE_OK ||
        manager_storage_gamestate_temp_path(paths->gamestate_temp,
                                            sizeof(paths->gamestate_temp),
                                            slot) != MANAGER_STORAGE_OK ||
        manager_storage_pair_rollback_path(paths->persistent_rollback,
                                           sizeof(paths->persistent_rollback),
                                           slot) != MANAGER_STORAGE_OK ||
        manager_storage_pair_rollback_gamestate_path(
            paths->gamestate_rollback, sizeof(paths->gamestate_rollback),
            slot) != MANAGER_STORAGE_OK ||
        manager_storage_pair_marker_path(paths->marker, sizeof(paths->marker),
                                         slot) != MANAGER_STORAGE_OK)
        return MANAGER_STORAGE_INVALID;
    return MANAGER_STORAGE_OK;
}

static int manager_storage_pair_marker_write(
    const struct manager_storage_io *io, const char *path,
    const struct manager_storage_pair_marker *marker)
{
    char text[MANAGER_STORAGE_PAIR_MARKER_BYTES + 1u];
    size_t offset = 0u;
    int handle;
    int result = MANAGER_STORAGE_OK;

    (void)snprintf(text, sizeof(text), "%c%c%04u", marker->persistent,
                   marker->gamestate, marker->index % 10000u);
    handle = io->open_write_exclusive(io->context, path);
    if (handle < 0)
        return MANAGER_STORAGE_CONFLICT;
    while (offset < MANAGER_STORAGE_PAIR_MARKER_BYTES) {
        const int written = io->write(io->context, handle, text + offset,
                                      MANAGER_STORAGE_PAIR_MARKER_BYTES - offset);
        if (written <= 0) {
            result = MANAGER_STORAGE_IO_ERROR;
            break;
        }
        offset += (size_t)written;
    }
    if (result == MANAGER_STORAGE_OK && io->sync_file(io->context, handle) < 0)
        result = MANAGER_STORAGE_IO_ERROR;
    if (io->close_file(io->context, handle) < 0 && result == MANAGER_STORAGE_OK)
        result = MANAGER_STORAGE_IO_ERROR;
    if (result != MANAGER_STORAGE_OK)
        (void)io->remove_file(io->context, path);
    return result;
}

/* A marker that is unreadable or malformed (a crash before its fsync) is read
 * as "PG" without an index: promote whatever rollback exists, leave the live
 * files alone otherwise. */
static int manager_storage_pair_marker_read(
    const struct manager_storage_io *io, const char *path,
    struct manager_storage_pair_marker *marker)
{
    unsigned char text[MANAGER_STORAGE_PAIR_MARKER_BYTES + 2u];
    size_t total = 0u;
    size_t index;
    int handle;
    int result = MANAGER_STORAGE_OK;

    marker->persistent = 'P';
    marker->gamestate = 'G';
    marker->has_index = 0;
    marker->index = 0u;
    handle = io->open_read(io->context, path);
    if (handle < 0)
        return MANAGER_STORAGE_IO_ERROR;
    while (total < sizeof(text)) {
        const int amount = io->read(io->context, handle, text + total,
                                    sizeof(text) - total);
        if (amount < 0) {
            result = MANAGER_STORAGE_IO_ERROR;
            break;
        }
        if (amount == 0)
            break;
        total += (size_t)amount;
    }
    if (io->close_file(io->context, handle) < 0 && result == MANAGER_STORAGE_OK)
        result = MANAGER_STORAGE_IO_ERROR;
    if (result != MANAGER_STORAGE_OK)
        return result;
    if (total != MANAGER_STORAGE_PAIR_MARKER_BYTES ||
        (text[0] != 'P' && text[0] != 'p') ||
        (text[1] != 'G' && text[1] != 'g' && text[1] != '-'))
        return MANAGER_STORAGE_OK;
    for (index = 2u; index < MANAGER_STORAGE_PAIR_MARKER_BYTES; ++index) {
        if (!isdigit(text[index]))
            return MANAGER_STORAGE_OK;
    }
    marker->persistent = (char)text[0];
    marker->gamestate = (char)text[1];
    marker->has_index = 1;
    marker->index = (unsigned int)(text[2] - '0') * 1000u +
                    (unsigned int)(text[3] - '0') * 100u +
                    (unsigned int)(text[4] - '0') * 10u +
                    (unsigned int)(text[5] - '0');
    return MANAGER_STORAGE_OK;
}

/* Sets a promoted live file aside: quarantined under a fresh recovered-slot
 * index shared by both sides of one recovery, or removed. */
static int manager_storage_pair_displace(
    const struct manager_storage_io *io, int slot, const char *live,
    int gamestate_side, int quarantine,
    struct manager_storage_quarantine *state)
{
    char destination[MANAGER_STORAGE_PATH_CAPACITY];
    enum manager_storage_node_kind kind = MANAGER_STORAGE_NODE_OTHER;
    const int status = manager_storage_stat(io, live, &kind, NULL);
    if (status == 1)
        return MANAGER_STORAGE_OK;
    if (status < 0)
        return MANAGER_STORAGE_IO_ERROR;
    if (kind != MANAGER_STORAGE_NODE_FILE)
        return MANAGER_STORAGE_CONFLICT;
    if (!quarantine)
        return io->remove_file(io->context, live) < 0 ? MANAGER_STORAGE_IO_ERROR
                                                      : MANAGER_STORAGE_OK;
    if (!state->selected) {
        int result = manager_storage_ensure_backup_root(io);
        if (result < 0)
            return result;
        result = manager_storage_find_free_backup(io, "recovered-slot", slot,
                                                  state->primary,
                                                  sizeof(state->primary), NULL);
        if (result != MANAGER_STORAGE_OK)
            return result;
        state->selected = 1;
    }
    if (gamestate_side) {
        if (manager_storage_companion_path(destination, sizeof(destination),
                                           state->primary) != MANAGER_STORAGE_OK)
            return MANAGER_STORAGE_INVALID;
    } else {
        (void)snprintf(destination, sizeof(destination), "%s", state->primary);
    }
    return io->rename_path(io->context, live, destination) < 0
               ? MANAGER_STORAGE_IO_ERROR
               : MANAGER_STORAGE_OK;
}

static int manager_storage_pair_unwind_side(
    const struct manager_storage_io *io, int slot, const char *live,
    const char *rollback, const char *archive, char flag, int gamestate_side,
    int quarantine, struct manager_storage_quarantine *state)
{
    enum manager_storage_node_kind kind = MANAGER_STORAGE_NODE_OTHER;
    const char *source = NULL;
    int status;
    int result;

    if (flag == '-')
        return MANAGER_STORAGE_OK;
    if (flag == 'P' || flag == 'G') {
        status = manager_storage_stat(io, rollback, &kind, NULL);
        if (status < 0)
            return MANAGER_STORAGE_IO_ERROR;
        if (status == 0) {
            if (kind != MANAGER_STORAGE_NODE_FILE)
                return MANAGER_STORAGE_CONFLICT;
            source = rollback;
        } else if (archive != NULL) {
            status = manager_storage_stat(io, archive, &kind, NULL);
            if (status < 0)
                return MANAGER_STORAGE_IO_ERROR;
            if (status == 0) {
                if (kind != MANAGER_STORAGE_NODE_FILE)
                    return MANAGER_STORAGE_CONFLICT;
                source = archive;
            }
        }
        if (source == NULL)
            return MANAGER_STORAGE_OK; /* not set aside yet: live is old */
    }
    result = manager_storage_pair_displace(io, slot, live, gamestate_side,
                                           quarantine, state);
    if (result < 0)
        return result;
    if (source != NULL && io->rename_path(io->context, source, live) < 0)
        return MANAGER_STORAGE_IO_ERROR;
    return MANAGER_STORAGE_OK;
}

static int manager_storage_pair_unwind(
    const struct manager_storage_io *io, int slot,
    const struct manager_storage_pair_paths *paths,
    const struct manager_storage_pair_marker *marker, int quarantine)
{
    struct manager_storage_quarantine state;
    char archive_primary[MANAGER_STORAGE_PATH_CAPACITY];
    char archive_gamestate[MANAGER_STORAGE_PATH_CAPACITY];
    const char *primary = NULL;
    const char *companion = NULL;
    int result;
    int warning = 0;

    memset(&state, 0, sizeof(state));
    if (marker->has_index) {
        if (manager_storage_local_backup_path(archive_primary,
                                              sizeof(archive_primary), "slot",
                                              slot, marker->index) !=
                MANAGER_STORAGE_OK ||
            manager_storage_companion_path(archive_gamestate,
                                           sizeof(archive_gamestate),
                                           archive_primary) != MANAGER_STORAGE_OK)
            return MANAGER_STORAGE_INVALID;
        primary = archive_primary;
        companion = archive_gamestate;
    }
    result = manager_storage_pair_unwind_side(
        io, slot, paths->gamestate, paths->gamestate_rollback, companion,
        marker->gamestate, 1, quarantine, &state);
    if (result < 0)
        return result;
    result = manager_storage_pair_unwind_side(
        io, slot, paths->persistent, paths->persistent_rollback, primary,
        marker->persistent, 0, quarantine, &state);
    if (result < 0)
        return result;
    result = manager_storage_remove_managed_file(io, paths->gamestate_temp);
    if (result < 0)
        return result;
    result = manager_storage_remove_managed_file(io, paths->persistent_temp);
    if (result < 0)
        return result;
    result = manager_storage_remove_managed_file(io, paths->marker);
    if (result < 0)
        return result;
    if (io->sync_device(io->context) < 0)
        warning = 1;
    return warning ? MANAGER_STORAGE_SYNC_WARNING : MANAGER_STORAGE_OK;
}

static int manager_storage_pair_fail(
    const struct manager_storage_io *io, int slot,
    const struct manager_storage_pair_paths *paths,
    const struct manager_storage_pair_marker *marker, int error)
{
    (void)manager_storage_pair_unwind(io, slot, paths, marker, 0);
    return error;
}

static int manager_storage_recover_pair(const struct manager_storage_io *io,
                                        int slot)
{
    struct manager_storage_pair_paths paths;
    struct manager_storage_pair_marker marker;
    char backup_temporary[MANAGER_STORAGE_PATH_CAPACITY];
    enum manager_storage_node_kind kind = MANAGER_STORAGE_NODE_OTHER;
    int status;
    int result;

    if (manager_storage_pair_paths_init(&paths, slot) != MANAGER_STORAGE_OK ||
        manager_storage_backup_gamestate_temp_path(
            backup_temporary, sizeof(backup_temporary), slot) !=
            MANAGER_STORAGE_OK)
        return MANAGER_STORAGE_INVALID;
    status = manager_storage_stat(io, paths.marker, &kind, NULL);
    if (status < 0)
        return MANAGER_STORAGE_IO_ERROR;
    if (status == 0) {
        if (kind != MANAGER_STORAGE_NODE_FILE)
            return MANAGER_STORAGE_CONFLICT;
        result = manager_storage_pair_marker_read(io, paths.marker, &marker);
        if (result < 0)
            return result;
        return manager_storage_pair_unwind(io, slot, &paths, &marker, 1);
    }
    /* No marker. Rollback files cannot be left behind by this protocol, but a
     * stray one is treated like the marker-less legacy rollback: old wins. */
    status = manager_storage_stat(io, paths.persistent_rollback, &kind, NULL);
    if (status < 0)
        return MANAGER_STORAGE_IO_ERROR;
    if (status == 1) {
        status = manager_storage_stat(io, paths.gamestate_rollback, &kind,
                                      NULL);
        if (status < 0)
            return MANAGER_STORAGE_IO_ERROR;
    }
    if (status == 0) {
        marker.persistent = 'P';
        marker.gamestate = 'G';
        marker.has_index = 0;
        marker.index = 0u;
        return manager_storage_pair_unwind(io, slot, &paths, &marker, 1);
    }
    result = manager_storage_remove_managed_file(io, paths.gamestate_temp);
    if (result < 0)
        return result;
    return manager_storage_remove_managed_file(io, backup_temporary);
}
#endif
static int manager_storage_recover_slot(const struct manager_storage_io *io,
                                        int slot)
{
    char target[MANAGER_STORAGE_PATH_CAPACITY];
    char temporary[MANAGER_STORAGE_PATH_CAPACITY];
    char backup_temporary[MANAGER_STORAGE_PATH_CAPACITY];
    char rollback[MANAGER_STORAGE_PATH_CAPACITY];
    char quarantine[MANAGER_STORAGE_PATH_CAPACITY];
    enum manager_storage_node_kind target_kind;
    enum manager_storage_node_kind rollback_kind;
    int target_status;
    int rollback_status;
    int warning = 0;

    if (manager_storage_slot_path(target, sizeof(target), slot) !=
            MANAGER_STORAGE_OK ||
        manager_storage_restore_temp_path(temporary, sizeof(temporary), slot) !=
            MANAGER_STORAGE_OK ||
        manager_storage_backup_temp_path(backup_temporary,
                                           sizeof(backup_temporary), slot) !=
            MANAGER_STORAGE_OK ||
        manager_storage_rollback_path(rollback, sizeof(rollback), slot) !=
            MANAGER_STORAGE_OK)
        return MANAGER_STORAGE_INVALID;
    target_status = manager_storage_stat(io, target, &target_kind, NULL);
    rollback_status = manager_storage_stat(io, rollback, &rollback_kind, NULL);
    if (target_status < 0 || rollback_status < 0)
        return MANAGER_STORAGE_IO_ERROR;
    if (rollback_status == 0) {
        int moved_target = 0;
        if (rollback_kind != MANAGER_STORAGE_NODE_FILE)
            return MANAGER_STORAGE_CONFLICT;
        if (target_status == 0) {
            int result;
            if (target_kind != MANAGER_STORAGE_NODE_FILE)
                return MANAGER_STORAGE_CONFLICT;
            result = manager_storage_ensure_backup_root(io);
            if (result < 0)
                return result;
            if (result == MANAGER_STORAGE_SYNC_WARNING)
                warning = 1;
            result = manager_storage_find_free_backup(
                io, "recovered-slot", slot, quarantine, sizeof(quarantine),
                NULL);
            if (result != MANAGER_STORAGE_OK)
                return result;
            if (io->rename_path(io->context, target, quarantine) < 0)
                return MANAGER_STORAGE_IO_ERROR;
            moved_target = 1;
        } else if (target_status != 1) {
            return MANAGER_STORAGE_IO_ERROR;
        }
        if (io->rename_path(io->context, rollback, target) < 0) {
            if (moved_target)
                (void)io->rename_path(io->context, quarantine, target);
            return MANAGER_STORAGE_IO_ERROR;
        }
        if (io->sync_device(io->context) < 0)
            warning = 1;
    } else if (target_status == 0 &&
               target_kind != MANAGER_STORAGE_NODE_FILE) {
        return MANAGER_STORAGE_CONFLICT;
    }
#if defined(ISAAC_VITA_MANAGER_SAVE_GAMESTATE)
    {
        const int result = manager_storage_recover_pair(io, slot);
        if (result < 0)
            return result;
        if (result == MANAGER_STORAGE_SYNC_WARNING)
            warning = 1;
    }
#endif
    {
        int result = manager_storage_remove_managed_file(io, temporary);
        if (result < 0)
            return result;
        result = manager_storage_remove_managed_file(io, backup_temporary);
        if (result < 0)
            return result;
    }
    if (io->sync_device(io->context) < 0)
        warning = 1;
    return warning ? MANAGER_STORAGE_SYNC_WARNING : MANAGER_STORAGE_OK;
}
#if defined(MANAGER_SETTINGS_UI)
/* ---- manager-owned text files (port.ini) ---------------------------------
 * One write primitive
 * modelled on manager_storage_restore_backup below, no fixed .bak, previous
 * versions archived as MANAGER_STORAGE_BACKUP_ROOT "/<prefix>-NNNN<ext>",
 * an in-flight marker <name>.rollback that exists only during a promotion,
 * and a stale <name>.tmp that recover removes.  Renames onto an existing
 * path are never issued (Vita overwrite semantics unverified; the host fake
 * refuses them). */

struct manager_storage_text_paths {
    char target[MANAGER_STORAGE_PATH_CAPACITY];
    char temporary[MANAGER_STORAGE_PATH_CAPACITY];
    char rollback[MANAGER_STORAGE_PATH_CAPACITY];
    char prefix[MANAGER_STORAGE_NAME_CAPACITY];
    char extension[MANAGER_STORAGE_NAME_CAPACITY];
};

/* "<prefix><ext>" directly under MANAGER_STORAGE_ROOT; the archive prefix is
 * the name up to its last '.', the extension the rest ("port.ini" ->
 * "port", ".ini").  Hidden names and names without an extension are
 * rejected so a sidecar can never be named as a target. */
static int manager_storage_text_paths_init(
    struct manager_storage_text_paths *paths, const char *name)
{
    const char *dot;
    size_t prefix_length;
    size_t extension_length;

    if (!manager_storage_safe_segment(name) || name[0] == '.')
        return MANAGER_STORAGE_INVALID;
    dot = strrchr(name, '.');
    if (dot == NULL || dot == name || dot[1] == '\0')
        return MANAGER_STORAGE_INVALID;
    prefix_length = (size_t)(dot - name);
    extension_length = strlen(dot);
    if (prefix_length >= sizeof(paths->prefix) ||
        extension_length >= sizeof(paths->extension))
        return MANAGER_STORAGE_INVALID;
    memcpy(paths->prefix, name, prefix_length);
    paths->prefix[prefix_length] = '\0';
    memcpy(paths->extension, dot, extension_length + 1u);
    if (manager_storage_join(paths->target, sizeof(paths->target),
                             MANAGER_STORAGE_ROOT, name, NULL) !=
            MANAGER_STORAGE_OK ||
        manager_storage_join(paths->temporary, sizeof(paths->temporary),
                             MANAGER_STORAGE_ROOT, name, ".tmp") !=
            MANAGER_STORAGE_OK ||
        manager_storage_join(paths->rollback, sizeof(paths->rollback),
                             MANAGER_STORAGE_ROOT, name, ".rollback") !=
            MANAGER_STORAGE_OK)
        return MANAGER_STORAGE_INVALID;
    return MANAGER_STORAGE_OK;
}

static int manager_storage_named_valid(const char *prefix,
                                       const char *extension)
{
    return manager_storage_safe_segment(prefix) && prefix[0] != '.' &&
           extension != NULL && extension[0] == '.' &&
           manager_storage_safe_segment(extension);
}

static int manager_storage_named_path(char *path, size_t capacity,
                                      const char *prefix, unsigned int index,
                                      const char *extension)
{
    const int length = snprintf(path, capacity, "%s/%s-%04u%s",
                                MANAGER_STORAGE_BACKUP_ROOT, prefix, index,
                                extension);
    return length >= 0 && (size_t)length < capacity ? MANAGER_STORAGE_OK
                                                    : MANAGER_STORAGE_INVALID;
}

/* "<prefix>-NNNN<ext>" exactly; quarantine names ("<prefix>-recovered-...")
 * fail the four-digit test and are never mistaken for archives. */
static int manager_storage_parse_named(const char *name, const char *prefix,
                                       const char *extension,
                                       unsigned int *index)
{
    const size_t prefix_length = strlen(prefix);
    const size_t extension_length = strlen(extension);
    const char *digits;
    size_t position;
    unsigned int parsed = 0u;

    if (name == NULL ||
        strlen(name) != prefix_length + 5u + extension_length ||
        memcmp(name, prefix, prefix_length) != 0 ||
        name[prefix_length] != '-' ||
        strcmp(name + prefix_length + 5u, extension) != 0)
        return 0;
    digits = name + prefix_length + 1u;
    for (position = 0u; position < 4u; ++position) {
        if (digits[position] < '0' || digits[position] > '9')
            return 0;
        parsed = parsed * 10u + (unsigned int)(digits[position] - '0');
    }
    *index = parsed;
    return 1;
}

int manager_storage_newest_named(const struct manager_storage_io *io,
                                 const char *prefix, const char *extension,
                                 unsigned int *index, char *path,
                                 size_t capacity)
{
    enum manager_storage_node_kind root_kind = MANAGER_STORAGE_NODE_OTHER;
    int root_status;
    int directory;
    int result = MANAGER_STORAGE_ABSENT;
    unsigned int best = 0u;

    if (!manager_storage_io_valid(io) ||
        !manager_storage_named_valid(prefix, extension) || index == NULL)
        return MANAGER_STORAGE_INVALID;
    *index = 0u;
    if (path != NULL && capacity != 0u)
        path[0] = '\0';
    root_status = manager_storage_stat(io, MANAGER_STORAGE_BACKUP_ROOT,
                                       &root_kind, NULL);
    if (root_status == 1)
        return MANAGER_STORAGE_ABSENT;
    if (root_status < 0)
        return MANAGER_STORAGE_IO_ERROR;
    if (root_kind != MANAGER_STORAGE_NODE_DIRECTORY)
        return MANAGER_STORAGE_CONFLICT;
    directory = io->open_directory(io->context, MANAGER_STORAGE_BACKUP_ROOT);
    if (directory < 0)
        return MANAGER_STORAGE_IO_ERROR;
    for (;;) {
        struct manager_storage_dir_entry entry;
        unsigned int parsed;
        int read_result;
        memset(&entry, 0, sizeof(entry));
        read_result = io->read_directory(io->context, directory, &entry);
        if (read_result <= 0) {
            if (read_result < 0)
                result = MANAGER_STORAGE_IO_ERROR;
            break;
        }
        if (entry.kind != MANAGER_STORAGE_NODE_FILE ||
            !manager_storage_safe_segment(entry.name) ||
            !manager_storage_parse_named(entry.name, prefix, extension,
                                         &parsed))
            continue;
        if (result == MANAGER_STORAGE_ABSENT || parsed > best) {
            best = parsed;
            result = MANAGER_STORAGE_OK;
        }
    }
    if (io->close_directory(io->context, directory) < 0 &&
        result != MANAGER_STORAGE_IO_ERROR)
        result = MANAGER_STORAGE_IO_ERROR;
    if (result != MANAGER_STORAGE_OK)
        return result;
    if (path != NULL &&
        manager_storage_named_path(path, capacity, prefix, best, extension) !=
            MANAGER_STORAGE_OK)
        return MANAGER_STORAGE_INVALID;
    *index = best;
    return MANAGER_STORAGE_OK;
}

int manager_storage_find_free_named(const struct manager_storage_io *io,
                                    const char *prefix, const char *extension,
                                    char *path, size_t capacity,
                                    unsigned int *selected)
{
    unsigned int newest = 0u;
    unsigned int index;
    int result;

    if (!manager_storage_io_valid(io) ||
        !manager_storage_named_valid(prefix, extension) || path == NULL)
        return MANAGER_STORAGE_INVALID;
    /* Start above the newest archive so the reserved index is always the
     * newest one (UNDO = newest); gaps left by hand-deleted archives are
     * never reused.  Without a backup root the scan starts at 0000. */
    result = manager_storage_newest_named(io, prefix, extension, &newest,
                                          NULL, 0u);
    if (result < 0)
        return result;
    if (result == MANAGER_STORAGE_OK) {
        if (newest >= MANAGER_STORAGE_TEXT_INDEX_MAX)
            return MANAGER_STORAGE_CAPACITY;
        newest += 1u;
    } else {
        newest = 0u;
    }
    for (index = newest; index <= MANAGER_STORAGE_TEXT_INDEX_MAX; ++index) {
        enum manager_storage_node_kind kind;
        int status;
        if (manager_storage_named_path(path, capacity, prefix, index,
                                       extension) != MANAGER_STORAGE_OK)
            return MANAGER_STORAGE_INVALID;
        status = manager_storage_stat(io, path, &kind, NULL);
        if (status == 1) {
            if (selected != NULL)
                *selected = index;
            return MANAGER_STORAGE_OK;
        }
        if (status < 0)
            return MANAGER_STORAGE_IO_ERROR;
    }
    return MANAGER_STORAGE_CAPACITY;
}

static int manager_storage_read_bytes(const struct manager_storage_io *io,
                                      const char *path, char *buffer,
                                      size_t capacity, size_t *length,
                                      int allow_truncation, int *truncated)
{
    enum manager_storage_node_kind kind = MANAGER_STORAGE_NODE_OTHER;
    uint64_t size = 0u;
    size_t used = 0u;
    int status;
    int handle;
    int result = MANAGER_STORAGE_OK;

    if (truncated != NULL)
        *truncated = 0;
    if (!manager_storage_io_valid(io) || path == NULL || buffer == NULL ||
        capacity == 0u || length == NULL)
        return MANAGER_STORAGE_INVALID;
    *length = 0u;
    buffer[0] = '\0';
    status = manager_storage_stat(io, path, &kind, &size);
    if (status == 1)
        return MANAGER_STORAGE_ABSENT;
    if (status < 0)
        return MANAGER_STORAGE_IO_ERROR;
    if (kind != MANAGER_STORAGE_NODE_FILE)
        return MANAGER_STORAGE_CONFLICT;
    /* The reported size is advisory (the read below is authoritative); it
     * only spares a large file the read when the caller refuses it. */
    if (!allow_truncation && size > (uint64_t)(capacity - 1u))
        return MANAGER_STORAGE_TOO_LARGE;
    handle = io->open_read(io->context, path);
    if (handle < 0)
        return MANAGER_STORAGE_IO_ERROR;
    for (;;) {
        int amount;
        if (used == capacity - 1u) {
            /* Buffer full: one probe byte tells whether the file goes on. */
            unsigned char probe;
            amount = io->read(io->context, handle, &probe, 1u);
            if (amount < 0) {
                result = MANAGER_STORAGE_IO_ERROR;
            } else if (amount > 0) {
                if (allow_truncation) {
                    if (truncated != NULL)
                        *truncated = 1;
                } else {
                    result = MANAGER_STORAGE_TOO_LARGE;
                }
            }
            break;
        }
        amount = io->read(io->context, handle, buffer + used,
                          capacity - 1u - used);
        if (amount < 0) {
            result = MANAGER_STORAGE_IO_ERROR;
            break;
        }
        if (amount == 0)
            break;
        if ((size_t)amount > capacity - 1u - used) {
            result = MANAGER_STORAGE_IO_ERROR;
            break;
        }
        used += (size_t)amount;
    }
    if (io->close_file(io->context, handle) < 0 && result == MANAGER_STORAGE_OK)
        result = MANAGER_STORAGE_IO_ERROR;
    if (result != MANAGER_STORAGE_OK) {
        buffer[0] = '\0';
        if (truncated != NULL)
            *truncated = 0;
        return result;
    }
    buffer[used] = '\0';
    *length = used;
    return MANAGER_STORAGE_OK;
}

int manager_storage_read_text(const struct manager_storage_io *io,
                              const char *path, char *buffer,
                              size_t capacity, size_t *length)
{
    return manager_storage_read_bytes(io, path, buffer, capacity, length, 0,
                                      NULL);
}

int manager_storage_read_head(const struct manager_storage_io *io,
                              const char *path, char *buffer,
                              size_t capacity, size_t *length,
                              int *truncated)
{
    return manager_storage_read_bytes(io, path, buffer, capacity, length, 1,
                                      truncated);
}

static void manager_storage_hash_bytes(const void *bytes, size_t length,
                                       unsigned char digest[32])
{
    struct manager_sha256_context hash;
    manager_sha256_init(&hash);
    if (length != 0u)
        manager_sha256_update(&hash, bytes, length);
    manager_sha256_final(&hash, digest);
}

/* Mirror of manager_storage_recover_slot for one text file: a stranded
 * rollback marker wins (the old settings), any target present next to it
 * is quarantined under "<prefix>-recovered-NNNN<ext>", then a stale temp is
 * removed.  Archives are never touched, so DEFAULTS (archive + remove, no
 * sidecar) is stable across recover. */
static int manager_storage_recover_text(
    const struct manager_storage_io *io,
    const struct manager_storage_text_paths *paths)
{
    enum manager_storage_node_kind target_kind = MANAGER_STORAGE_NODE_OTHER;
    enum manager_storage_node_kind rollback_kind = MANAGER_STORAGE_NODE_OTHER;
    int target_status;
    int rollback_status;
    int warning = 0;

    target_status = manager_storage_stat(io, paths->target, &target_kind,
                                         NULL);
    rollback_status = manager_storage_stat(io, paths->rollback,
                                           &rollback_kind, NULL);
    if (target_status < 0 || rollback_status < 0)
        return MANAGER_STORAGE_IO_ERROR;
    if (rollback_status == 0) {
        char quarantine[MANAGER_STORAGE_PATH_CAPACITY];
        int moved_target = 0;
        if (rollback_kind != MANAGER_STORAGE_NODE_FILE)
            return MANAGER_STORAGE_CONFLICT;
        if (target_status == 0) {
            char quarantine_prefix[MANAGER_STORAGE_NAME_CAPACITY];
            int result;
            int length;
            if (target_kind != MANAGER_STORAGE_NODE_FILE)
                return MANAGER_STORAGE_CONFLICT;
            result = manager_storage_ensure_backup_root(io);
            if (result < 0)
                return result;
            if (result == MANAGER_STORAGE_SYNC_WARNING)
                warning = 1;
            length = snprintf(quarantine_prefix, sizeof(quarantine_prefix),
                              "%s-recovered", paths->prefix);
            if (length < 0 || (size_t)length >= sizeof(quarantine_prefix))
                return MANAGER_STORAGE_INVALID;
            result = manager_storage_find_free_named(
                io, quarantine_prefix, paths->extension, quarantine,
                sizeof(quarantine), NULL);
            if (result != MANAGER_STORAGE_OK)
                return result;
            if (io->rename_path(io->context, paths->target, quarantine) < 0)
                return MANAGER_STORAGE_IO_ERROR;
            moved_target = 1;
        }
        if (io->rename_path(io->context, paths->rollback, paths->target) < 0) {
            if (moved_target)
                (void)io->rename_path(io->context, quarantine, paths->target);
            return MANAGER_STORAGE_IO_ERROR;
        }
        if (io->sync_device(io->context) < 0)
            warning = 1;
    } else if (target_status == 0 &&
               target_kind != MANAGER_STORAGE_NODE_FILE) {
        return MANAGER_STORAGE_CONFLICT;
    }
    {
        const int result = manager_storage_remove_managed_file(
            io, paths->temporary);
        if (result < 0)
            return result;
    }
    if (io->sync_device(io->context) < 0)
        warning = 1;
    return warning ? MANAGER_STORAGE_SYNC_WARNING : MANAGER_STORAGE_OK;
}

int manager_storage_recover_port(const struct manager_storage_io *io)
{
    struct manager_storage_text_paths paths;
    if (!manager_storage_io_valid(io))
        return MANAGER_STORAGE_INVALID;
    if (manager_storage_text_paths_init(&paths,
                                        MANAGER_STORAGE_PORT_INI_NAME) !=
        MANAGER_STORAGE_OK)
        return MANAGER_STORAGE_INVALID;
    return manager_storage_recover_text(io, &paths);
}

/* Same shape as the slot restore's rollback_promotion: the new file (if it
 * is the target) goes back to the temp name, the rollback marker returns
 * to the target, the temp is removed. */
static void manager_storage_text_rollback_promotion(
    const struct manager_storage_io *io,
    const struct manager_storage_text_paths *paths, int had_old)
{
    enum manager_storage_node_kind kind = MANAGER_STORAGE_NODE_OTHER;
    const int target_status = manager_storage_stat(io, paths->target, &kind,
                                                   NULL);
    if (target_status == 0 && kind == MANAGER_STORAGE_NODE_FILE) {
        if (io->rename_path(io->context, paths->target, paths->temporary) < 0)
            return;
    }
    if (had_old) {
        if (io->rename_path(io->context, paths->rollback, paths->target) < 0) {
            (void)io->rename_path(io->context, paths->temporary,
                                  paths->target);
            return;
        }
    }
    (void)io->remove_file(io->context, paths->temporary);
    (void)io->sync_device(io->context);
}

int manager_storage_write_text_atomic(const struct manager_storage_io *io,
                                      const char *name, const void *bytes,
                                      size_t length)
{
    struct manager_storage_text_paths paths;
    char archive[MANAGER_STORAGE_PATH_CAPACITY];
    unsigned char expected[32];
    unsigned char actual[32];
    uint64_t actual_size = 0u;
    enum manager_storage_node_kind target_kind = MANAGER_STORAGE_NODE_OTHER;
    int target_status;
    int had_old;
    int handle;
    int result;
    int warning = 0;
    size_t offset = 0u;

    if (!manager_storage_io_valid(io) || (bytes == NULL && length != 0u))
        return MANAGER_STORAGE_INVALID;
    if (length > MANAGER_STORAGE_TEXT_MAX_BYTES)
        return MANAGER_STORAGE_TOO_LARGE;
    if (manager_storage_text_paths_init(&paths, name) != MANAGER_STORAGE_OK)
        return MANAGER_STORAGE_INVALID;
    /* 1. No rollback marker and no temp exist after this. */
    result = manager_storage_recover_text(io, &paths);
    if (result < 0)
        return result;
    if (result == MANAGER_STORAGE_SYNC_WARNING)
        warning = 1;
    /* 2. Reserve the archive name before anything is written. */
    result = manager_storage_ensure_backup_root(io);
    if (result < 0)
        return result;
    if (result == MANAGER_STORAGE_SYNC_WARNING)
        warning = 1;
    result = manager_storage_find_free_named(io, paths.prefix, paths.extension,
                                             archive, sizeof(archive), NULL);
    if (result != MANAGER_STORAGE_OK)
        return result;
    /* 3. Exclusive temp, write, sync, close, verified readback.  The vtable
     * carries no error class, so a failed O_EXCL open is classified by a
     * stat: the temp exists -> CONFLICT (something appeared since step 1),
     * absent or unstattable -> IO_ERROR (ENOSPC, EIO, a lost descriptor). */
    handle = io->open_write_exclusive(io->context, paths.temporary);
    if (handle < 0)
        return manager_storage_stat(io, paths.temporary, NULL, NULL) == 0
                   ? MANAGER_STORAGE_CONFLICT
                   : MANAGER_STORAGE_IO_ERROR;
    while (offset < length) {
        const int written = io->write(io->context, handle,
                                      (const unsigned char *)bytes + offset,
                                      length - offset);
        if (written <= 0 || (size_t)written > length - offset) {
            result = MANAGER_STORAGE_IO_ERROR;
            break;
        }
        offset += (size_t)written;
    }
    if (result == MANAGER_STORAGE_OK && io->sync_file(io->context, handle) < 0)
        result = MANAGER_STORAGE_IO_ERROR;
    if (io->close_file(io->context, handle) < 0 && result == MANAGER_STORAGE_OK)
        result = MANAGER_STORAGE_IO_ERROR;
    if (result != MANAGER_STORAGE_OK) {
        (void)io->remove_file(io->context, paths.temporary);
        return result;
    }
    manager_storage_hash_bytes(bytes, length, expected);
    result = manager_storage_hash_path(io, paths.temporary, actual,
                                       &actual_size);
    if (result != MANAGER_STORAGE_OK || actual_size != (uint64_t)length ||
        memcmp(expected, actual, sizeof(actual)) != 0) {
        (void)io->remove_file(io->context, paths.temporary);
        return result == MANAGER_STORAGE_OK ? MANAGER_STORAGE_HASH_MISMATCH
                                            : result;
    }
    /* 4. Old target aside under the in-flight marker. */
    target_status = manager_storage_stat(io, paths.target, &target_kind, NULL);
    if (target_status < 0 ||
        (target_status == 0 && target_kind != MANAGER_STORAGE_NODE_FILE)) {
        (void)io->remove_file(io->context, paths.temporary);
        return target_status < 0 ? MANAGER_STORAGE_IO_ERROR
                                 : MANAGER_STORAGE_CONFLICT;
    }
    had_old = target_status == 0;
    if (had_old) {
        if (io->rename_path(io->context, paths.target, paths.rollback) < 0) {
            (void)io->remove_file(io->context, paths.temporary);
            return MANAGER_STORAGE_IO_ERROR;
        }
        if (io->sync_device(io->context) < 0) {
            (void)io->rename_path(io->context, paths.rollback, paths.target);
            (void)io->remove_file(io->context, paths.temporary);
            return MANAGER_STORAGE_IO_ERROR;
        }
    }
    /* 5. Promote, re-hash, sync. */
    if (io->rename_path(io->context, paths.temporary, paths.target) < 0) {
        if (had_old)
            (void)io->rename_path(io->context, paths.rollback, paths.target);
        (void)io->remove_file(io->context, paths.temporary);
        (void)io->sync_device(io->context);
        return MANAGER_STORAGE_IO_ERROR;
    }
    result = manager_storage_hash_path(io, paths.target, actual, &actual_size);
    if (result != MANAGER_STORAGE_OK || actual_size != (uint64_t)length ||
        memcmp(expected, actual, sizeof(actual)) != 0) {
        manager_storage_text_rollback_promotion(io, &paths, had_old);
        return result == MANAGER_STORAGE_OK ? MANAGER_STORAGE_HASH_MISMATCH
                                            : result;
    }
    if (io->sync_device(io->context) < 0) {
        manager_storage_text_rollback_promotion(io, &paths, had_old);
        return MANAGER_STORAGE_IO_ERROR;
    }
    /* 6. The previous version becomes the newest archive. */
    if (had_old) {
        if (io->rename_path(io->context, paths.rollback, archive) < 0) {
            manager_storage_text_rollback_promotion(io, &paths, 1);
            return MANAGER_STORAGE_IO_ERROR;
        }
        if (io->sync_device(io->context) < 0)
            warning = 1;
    }
    return warning ? MANAGER_STORAGE_SYNC_WARNING : MANAGER_STORAGE_OK;
}

int manager_storage_archive_and_remove(const struct manager_storage_io *io,
                                       const char *name)
{
    struct manager_storage_text_paths paths;
    char archive[MANAGER_STORAGE_PATH_CAPACITY];
    enum manager_storage_node_kind target_kind = MANAGER_STORAGE_NODE_OTHER;
    int target_status;
    int result;
    int warning = 0;

    if (!manager_storage_io_valid(io))
        return MANAGER_STORAGE_INVALID;
    if (manager_storage_text_paths_init(&paths, name) != MANAGER_STORAGE_OK)
        return MANAGER_STORAGE_INVALID;
    result = manager_storage_recover_text(io, &paths);
    if (result < 0)
        return result;
    if (result == MANAGER_STORAGE_SYNC_WARNING)
        warning = 1;
    target_status = manager_storage_stat(io, paths.target, &target_kind, NULL);
    if (target_status == 1)
        return MANAGER_STORAGE_ABSENT;
    if (target_status < 0)
        return MANAGER_STORAGE_IO_ERROR;
    if (target_kind != MANAGER_STORAGE_NODE_FILE)
        return MANAGER_STORAGE_CONFLICT;
    result = manager_storage_ensure_backup_root(io);
    if (result < 0)
        return result;
    if (result == MANAGER_STORAGE_SYNC_WARNING)
        warning = 1;
    result = manager_storage_find_free_named(io, paths.prefix, paths.extension,
                                             archive, sizeof(archive), NULL);
    if (result != MANAGER_STORAGE_OK)
        return result;
    /* One rename: after a power loss the file is either still in place
     * (settings kept) or archived (defaults) - both consistent. */
    if (io->rename_path(io->context, paths.target, archive) < 0)
        return MANAGER_STORAGE_IO_ERROR;
    if (io->sync_device(io->context) < 0)
        warning = 1;
    return warning ? MANAGER_STORAGE_SYNC_WARNING : MANAGER_STORAGE_OK;
}
#endif

int manager_storage_recover(const struct manager_storage_io *io)
{
    int slot;
    int warning = 0;
#if defined(MANAGER_SETTINGS_UI)
    /* Design 1.3 (detail) / 9: port.ini recovery runs at every manager
     * start and return, so a failing save slot (a directory named like its
     * sidecar, a stat error) no longer skips it; the slots are independent,
     * the first failure is the one reported. */
    int failure = 0;
#endif
    if (!manager_storage_io_valid(io))
        return MANAGER_STORAGE_INVALID;
    for (slot = 1; slot <= 3; ++slot) {
        const int result = manager_storage_recover_slot(io, slot);
#if defined(MANAGER_SETTINGS_UI)
        if (result < 0) {
            if (failure == 0)
                failure = result;
            continue;
        }
#else
        if (result < 0)
            return result;
#endif
        if (result == MANAGER_STORAGE_SYNC_WARNING)
            warning = 1;
    }
#if defined(MANAGER_SETTINGS_UI)
    {
        const int result = manager_storage_recover_port(io);
        if (result < 0 && failure == 0)
            failure = result;
        if (result == MANAGER_STORAGE_SYNC_WARNING)
            warning = 1;
    }
    if (failure < 0)
        return failure;
#endif
    return warning ? MANAGER_STORAGE_SYNC_WARNING : MANAGER_STORAGE_OK;
}

#if defined(ISAAC_VITA_MANAGER_SAVE_GAMESTATE)
int manager_storage_backup_slot(const struct manager_storage_io *io, int slot,
                                struct manager_save_backup *created)
{
    char source[MANAGER_STORAGE_PATH_CAPACITY];
    char gamestate[MANAGER_STORAGE_PATH_CAPACITY];
    char temporary[MANAGER_STORAGE_PATH_CAPACITY];
    char gamestate_temporary[MANAGER_STORAGE_PATH_CAPACITY];
    char destination[MANAGER_STORAGE_PATH_CAPACITY];
    char gamestate_destination[MANAGER_STORAGE_PATH_CAPACITY];
    unsigned char digest[32];
    uint64_t size;
    uint64_t gamestate_size = 0u;
    unsigned int index;
    enum manager_storage_node_kind kind;
    int has_gamestate = 0;
    int status;
    int result;
    int warning = 0;

    if (!manager_storage_io_valid(io) || created == NULL || slot < 1 || slot > 3)
        return MANAGER_STORAGE_INVALID;
    result = manager_storage_recover_slot(io, slot);
    if (result < 0)
        return result;
    if (result == MANAGER_STORAGE_SYNC_WARNING)
        warning = 1;
    result = manager_storage_ensure_backup_root(io);
    if (result < 0)
        return result;
    if (result == MANAGER_STORAGE_SYNC_WARNING)
        warning = 1;
    if (manager_storage_slot_path(source, sizeof(source), slot) !=
            MANAGER_STORAGE_OK ||
        manager_storage_gamestate_path(gamestate, sizeof(gamestate), slot) !=
            MANAGER_STORAGE_OK ||
        manager_storage_backup_temp_path(temporary, sizeof(temporary), slot) !=
            MANAGER_STORAGE_OK ||
        manager_storage_backup_gamestate_temp_path(
            gamestate_temporary, sizeof(gamestate_temporary), slot) !=
            MANAGER_STORAGE_OK)
        return MANAGER_STORAGE_INVALID;
    result = manager_storage_stat(io, source, &kind, &size);
    if (result != 0 || kind != MANAGER_STORAGE_NODE_FILE)
        return MANAGER_STORAGE_INVALID;
    if (size > MANAGER_STORAGE_MAX_SAVE_BYTES)
        return MANAGER_STORAGE_TOO_LARGE;
    /* The run state is optional: a slot without one is backed up alone and
     * the backup records that it carries none. */
    status = manager_storage_stat(io, gamestate, &kind, &gamestate_size);
    if (status < 0)
        return MANAGER_STORAGE_IO_ERROR;
    if (status == 0) {
        if (kind != MANAGER_STORAGE_NODE_FILE)
            return MANAGER_STORAGE_CONFLICT;
        if (gamestate_size > MANAGER_STORAGE_MAX_SAVE_BYTES)
            return MANAGER_STORAGE_TOO_LARGE;
        has_gamestate = 1;
    }
    result = manager_storage_find_free_backup(io, "slot", slot, destination,
                                              sizeof(destination), &index);
    if (result != MANAGER_STORAGE_OK)
        return result;
    if (manager_storage_companion_path(gamestate_destination,
                                       sizeof(gamestate_destination),
                                       destination) != MANAGER_STORAGE_OK)
        return MANAGER_STORAGE_INVALID;
    result = manager_storage_copy_verified(io, source, temporary, digest, &size);
    if (result != MANAGER_STORAGE_OK)
        return result;
    if (has_gamestate) {
        result = manager_storage_copy_verified(io, gamestate,
                                               gamestate_temporary, digest,
                                               &gamestate_size);
        if (result != MANAGER_STORAGE_OK) {
            (void)io->remove_file(io->context, temporary);
            return result;
        }
        /* Companion first: a listable .dat never appears without the run
         * state it was made with. */
        if (io->rename_path(io->context, gamestate_temporary,
                            gamestate_destination) < 0) {
            (void)io->remove_file(io->context, gamestate_temporary);
            (void)io->remove_file(io->context, temporary);
            return MANAGER_STORAGE_IO_ERROR;
        }
    }
    if (io->rename_path(io->context, temporary, destination) < 0) {
        (void)io->remove_file(io->context, temporary);
        if (has_gamestate)
            (void)io->remove_file(io->context, gamestate_destination);
        return MANAGER_STORAGE_IO_ERROR;
    }
    if (io->sync_device(io->context) < 0)
        warning = 1;
    memset(created, 0, sizeof(*created));
    created->slot = slot;
    created->origin = MANAGER_SAVE_BACKUP_LOCAL;
    created->size = size;
    created->has_gamestate = has_gamestate;
    created->gamestate_size = has_gamestate ? gamestate_size : 0u;
    (void)snprintf(created->label, sizeof(created->label),
                   "LOCAL S%d slot%d-%04u.dat%s", slot, slot, index,
                   has_gamestate ? " WITH RUN" : "");
    (void)snprintf(created->path, sizeof(created->path), "%s", destination);
    return warning ? MANAGER_STORAGE_SYNC_WARNING : MANAGER_STORAGE_OK;
}
#else
int manager_storage_backup_slot(const struct manager_storage_io *io, int slot,
                                struct manager_save_backup *created)
{
    char source[MANAGER_STORAGE_PATH_CAPACITY];
    char temporary[MANAGER_STORAGE_PATH_CAPACITY];
    char destination[MANAGER_STORAGE_PATH_CAPACITY];
    unsigned char digest[32];
    uint64_t size;
    unsigned int index;
    enum manager_storage_node_kind source_kind;
    int result;
    int warning = 0;

    if (!manager_storage_io_valid(io) || created == NULL || slot < 1 || slot > 3)
        return MANAGER_STORAGE_INVALID;
    result = manager_storage_recover_slot(io, slot);
    if (result < 0)
        return result;
    if (result == MANAGER_STORAGE_SYNC_WARNING)
        warning = 1;
    result = manager_storage_ensure_backup_root(io);
    if (result < 0)
        return result;
    if (result == MANAGER_STORAGE_SYNC_WARNING)
        warning = 1;
    if (manager_storage_slot_path(source, sizeof(source), slot) !=
            MANAGER_STORAGE_OK ||
        manager_storage_backup_temp_path(temporary, sizeof(temporary), slot) !=
            MANAGER_STORAGE_OK)
        return MANAGER_STORAGE_INVALID;
    result = manager_storage_stat(io, source, &source_kind, &size);
    if (result != 0 || source_kind != MANAGER_STORAGE_NODE_FILE)
        return MANAGER_STORAGE_INVALID;
    if (size > MANAGER_STORAGE_MAX_SAVE_BYTES)
        return MANAGER_STORAGE_TOO_LARGE;
    result = manager_storage_find_free_backup(io, "slot", slot, destination,
                                              sizeof(destination), &index);
    if (result != MANAGER_STORAGE_OK)
        return result;
    result = manager_storage_copy_verified(io, source, temporary, digest, &size);
    if (result != MANAGER_STORAGE_OK)
        return result;
    if (io->rename_path(io->context, temporary, destination) < 0) {
        (void)io->remove_file(io->context, temporary);
        return MANAGER_STORAGE_IO_ERROR;
    }
    if (io->sync_device(io->context) < 0)
        warning = 1;
    memset(created, 0, sizeof(*created));
    created->slot = slot;
    created->origin = MANAGER_SAVE_BACKUP_LOCAL;
    created->size = size;
    (void)snprintf(created->label, sizeof(created->label),
                   "LOCAL S%d slot%d-%04u.dat", slot, slot, index);
    (void)snprintf(created->path, sizeof(created->path), "%s", destination);
    return warning ? MANAGER_STORAGE_SYNC_WARNING : MANAGER_STORAGE_OK;
}
#endif

#if defined(ISAAC_VITA_MANAGER_SAVE_GAMESTATE)
int manager_storage_restore_backup(const struct manager_storage_io *io,
                                   const struct manager_save_backup *backup)
{
    struct manager_storage_pair_paths paths;
    struct manager_storage_pair_marker marker;
    char companion[MANAGER_STORAGE_PATH_CAPACITY];
    char archive_primary[MANAGER_STORAGE_PATH_CAPACITY];
    char archive_gamestate[MANAGER_STORAGE_PATH_CAPACITY];
    unsigned char expected_persistent[32];
    unsigned char expected_gamestate[32];
    unsigned char actual[32];
    uint64_t expected_persistent_size = 0u;
    uint64_t expected_gamestate_size = 0u;
    uint64_t actual_size = 0u;
    uint64_t source_size = 0u;
    enum manager_storage_node_kind kind;
    unsigned int index = 0u;
    int has_gamestate = 0;
    int had_old_persistent;
    int had_old_gamestate = 0;
    int status;
    int result;
    int warning = 0;

    if (!manager_storage_io_valid(io) ||
        !manager_storage_backup_allowed(backup))
        return MANAGER_STORAGE_INVALID;
    result = manager_storage_recover_slot(io, backup->slot);
    if (result < 0)
        return result;
    if (result == MANAGER_STORAGE_SYNC_WARNING)
        warning = 1;
    result = manager_storage_stat(io, backup->path, &kind, &source_size);
    if (result != 0 || kind != MANAGER_STORAGE_NODE_FILE)
        return MANAGER_STORAGE_INVALID;
    if (source_size > MANAGER_STORAGE_MAX_SAVE_BYTES)
        return MANAGER_STORAGE_TOO_LARGE;
    /* Only manager-made backups carry a run state; the disk decides, not the
     * listing flag. Dated and sync backups leave gamestateN.dat untouched. */
    if (backup->origin == MANAGER_SAVE_BACKUP_LOCAL) {
        if (manager_storage_companion_path(companion, sizeof(companion),
                                           backup->path) != MANAGER_STORAGE_OK)
            return MANAGER_STORAGE_INVALID;
        status = manager_storage_stat(io, companion, &kind, &source_size);
        if (status < 0)
            return MANAGER_STORAGE_IO_ERROR;
        if (status == 0) {
            if (kind != MANAGER_STORAGE_NODE_FILE)
                return MANAGER_STORAGE_CONFLICT;
            if (source_size > MANAGER_STORAGE_MAX_SAVE_BYTES)
                return MANAGER_STORAGE_TOO_LARGE;
            has_gamestate = 1;
        }
    }
    result = manager_storage_ensure_backup_root(io);
    if (result < 0)
        return result;
    if (result == MANAGER_STORAGE_SYNC_WARNING)
        warning = 1;
    if (manager_storage_pair_paths_init(&paths, backup->slot) !=
        MANAGER_STORAGE_OK)
        return MANAGER_STORAGE_INVALID;
    result = manager_storage_find_free_backup(io, "slot", backup->slot,
                                              archive_primary,
                                              sizeof(archive_primary), &index);
    if (result != MANAGER_STORAGE_OK)
        return result;
    if (manager_storage_companion_path(archive_gamestate,
                                       sizeof(archive_gamestate),
                                       archive_primary) != MANAGER_STORAGE_OK)
        return MANAGER_STORAGE_INVALID;
    status = manager_storage_stat(io, paths.persistent, &kind, NULL);
    if (status < 0 || (status == 0 && kind != MANAGER_STORAGE_NODE_FILE))
        return status < 0 ? MANAGER_STORAGE_IO_ERROR : MANAGER_STORAGE_CONFLICT;
    had_old_persistent = status == 0;
    if (has_gamestate) {
        status = manager_storage_stat(io, paths.gamestate, &kind, NULL);
        if (status < 0 || (status == 0 && kind != MANAGER_STORAGE_NODE_FILE))
            return status < 0 ? MANAGER_STORAGE_IO_ERROR
                              : MANAGER_STORAGE_CONFLICT;
        had_old_gamestate = status == 0;
    }
    result = manager_storage_copy_verified(io, backup->path,
                                           paths.persistent_temp,
                                           expected_persistent,
                                           &expected_persistent_size);
    if (result != MANAGER_STORAGE_OK)
        return result;
    if (has_gamestate) {
        result = manager_storage_copy_verified(io, companion,
                                               paths.gamestate_temp,
                                               expected_gamestate,
                                               &expected_gamestate_size);
        if (result != MANAGER_STORAGE_OK) {
            (void)io->remove_file(io->context, paths.persistent_temp);
            return result;
        }
    }
    marker.persistent = had_old_persistent ? 'P' : 'p';
    marker.gamestate = has_gamestate ? (had_old_gamestate ? 'G' : 'g') : '-';
    marker.has_index = 1;
    marker.index = index;
    result = manager_storage_pair_marker_write(io, paths.marker, &marker);
    if (result != MANAGER_STORAGE_OK) {
        (void)io->remove_file(io->context, paths.persistent_temp);
        if (has_gamestate)
            (void)io->remove_file(io->context, paths.gamestate_temp);
        return result;
    }
    /* From here on every failure unwinds through the marker: old data wins. */
    if (had_old_persistent &&
        io->rename_path(io->context, paths.persistent,
                        paths.persistent_rollback) < 0)
        return manager_storage_pair_fail(io, backup->slot, &paths, &marker,
                                         MANAGER_STORAGE_IO_ERROR);
    if (had_old_gamestate &&
        io->rename_path(io->context, paths.gamestate,
                        paths.gamestate_rollback) < 0)
        return manager_storage_pair_fail(io, backup->slot, &paths, &marker,
                                         MANAGER_STORAGE_IO_ERROR);
    if (io->sync_device(io->context) < 0)
        return manager_storage_pair_fail(io, backup->slot, &paths, &marker,
                                         MANAGER_STORAGE_IO_ERROR);
    if (has_gamestate &&
        io->rename_path(io->context, paths.gamestate_temp, paths.gamestate) < 0)
        return manager_storage_pair_fail(io, backup->slot, &paths, &marker,
                                         MANAGER_STORAGE_IO_ERROR);
    if (io->rename_path(io->context, paths.persistent_temp, paths.persistent) <
        0)
        return manager_storage_pair_fail(io, backup->slot, &paths, &marker,
                                         MANAGER_STORAGE_IO_ERROR);
    result = manager_storage_hash_path(io, paths.persistent, actual,
                                       &actual_size);
    if (result != MANAGER_STORAGE_OK || actual_size != expected_persistent_size ||
        memcmp(expected_persistent, actual, sizeof(actual)) != 0)
        return manager_storage_pair_fail(io, backup->slot, &paths, &marker,
                                         result == MANAGER_STORAGE_OK
                                             ? MANAGER_STORAGE_HASH_MISMATCH
                                             : result);
    if (has_gamestate) {
        result = manager_storage_hash_path(io, paths.gamestate, actual,
                                           &actual_size);
        if (result != MANAGER_STORAGE_OK ||
            actual_size != expected_gamestate_size ||
            memcmp(expected_gamestate, actual, sizeof(actual)) != 0)
            return manager_storage_pair_fail(io, backup->slot, &paths, &marker,
                                             result == MANAGER_STORAGE_OK
                                                 ? MANAGER_STORAGE_HASH_MISMATCH
                                                 : result);
    }
    if (io->sync_device(io->context) < 0)
        return manager_storage_pair_fail(io, backup->slot, &paths, &marker,
                                         MANAGER_STORAGE_IO_ERROR);
    /* Archive the pre-restore pair under the reserved index, run state first
     * so a listable .dat never appears without its companion. */
    if (had_old_gamestate &&
        io->rename_path(io->context, paths.gamestate_rollback,
                        archive_gamestate) < 0)
        return manager_storage_pair_fail(io, backup->slot, &paths, &marker,
                                         MANAGER_STORAGE_IO_ERROR);
    if (had_old_persistent &&
        io->rename_path(io->context, paths.persistent_rollback,
                        archive_primary) < 0)
        return manager_storage_pair_fail(io, backup->slot, &paths, &marker,
                                         MANAGER_STORAGE_IO_ERROR);
    if (io->sync_device(io->context) < 0)
        return manager_storage_pair_fail(io, backup->slot, &paths, &marker,
                                         MANAGER_STORAGE_IO_ERROR);
    /* The commit: without the marker nothing is in flight. */
    if (io->remove_file(io->context, paths.marker) < 0)
        return manager_storage_pair_fail(io, backup->slot, &paths, &marker,
                                         MANAGER_STORAGE_IO_ERROR);
    if (io->sync_device(io->context) < 0)
        warning = 1;
    return warning ? MANAGER_STORAGE_SYNC_WARNING : MANAGER_STORAGE_OK;
}
#else
static void manager_storage_rollback_promotion(
    const struct manager_storage_io *io, const char *target,
    const char *temporary, const char *rollback, int had_old)
{
    enum manager_storage_node_kind kind;
    int target_status = manager_storage_stat(io, target, &kind, NULL);
    if (target_status == 0 && kind == MANAGER_STORAGE_NODE_FILE) {
        if (io->rename_path(io->context, target, temporary) < 0)
            return;
    }
    if (had_old) {
        if (io->rename_path(io->context, rollback, target) < 0) {
            (void)io->rename_path(io->context, temporary, target);
            return;
        }
    }
    (void)io->remove_file(io->context, temporary);
    (void)io->sync_device(io->context);
}

int manager_storage_restore_backup(const struct manager_storage_io *io,
                                   const struct manager_save_backup *backup)
{
    char target[MANAGER_STORAGE_PATH_CAPACITY];
    char temporary[MANAGER_STORAGE_PATH_CAPACITY];
    char rollback[MANAGER_STORAGE_PATH_CAPACITY];
    char archive[MANAGER_STORAGE_PATH_CAPACITY];
    unsigned char expected[32];
    unsigned char actual[32];
    uint64_t expected_size;
    uint64_t actual_size;
    enum manager_storage_node_kind source_kind;
    enum manager_storage_node_kind target_kind;
    uint64_t source_size;
    int target_status;
    int had_old;
    int result;
    int warning = 0;

    if (!manager_storage_io_valid(io) ||
        !manager_storage_backup_allowed(backup))
        return MANAGER_STORAGE_INVALID;
    result = manager_storage_recover_slot(io, backup->slot);
    if (result < 0)
        return result;
    if (result == MANAGER_STORAGE_SYNC_WARNING)
        warning = 1;
    result = manager_storage_stat(io, backup->path, &source_kind, &source_size);
    if (result != 0 || source_kind != MANAGER_STORAGE_NODE_FILE)
        return MANAGER_STORAGE_INVALID;
    if (source_size > MANAGER_STORAGE_MAX_SAVE_BYTES)
        return MANAGER_STORAGE_TOO_LARGE;
    result = manager_storage_ensure_backup_root(io);
    if (result < 0)
        return result;
    if (result == MANAGER_STORAGE_SYNC_WARNING)
        warning = 1;
    if (manager_storage_slot_path(target, sizeof(target), backup->slot) !=
            MANAGER_STORAGE_OK ||
        manager_storage_restore_temp_path(temporary, sizeof(temporary),
                                           backup->slot) != MANAGER_STORAGE_OK ||
        manager_storage_rollback_path(rollback, sizeof(rollback), backup->slot) !=
            MANAGER_STORAGE_OK)
        return MANAGER_STORAGE_INVALID;
    result = manager_storage_find_free_backup(
        io, "slot", backup->slot, archive, sizeof(archive), NULL);
    if (result != MANAGER_STORAGE_OK)
        return result;
    result = manager_storage_copy_verified(io, backup->path, temporary, expected,
                                           &expected_size);
    if (result != MANAGER_STORAGE_OK)
        return result;
    target_status = manager_storage_stat(io, target, &target_kind, NULL);
    if (target_status < 0 ||
        (target_status == 0 && target_kind != MANAGER_STORAGE_NODE_FILE)) {
        (void)io->remove_file(io->context, temporary);
        return target_status < 0 ? MANAGER_STORAGE_IO_ERROR
                                 : MANAGER_STORAGE_CONFLICT;
    }
    had_old = target_status == 0;
    if (had_old) {
        if (io->rename_path(io->context, target, rollback) < 0) {
            (void)io->remove_file(io->context, temporary);
            return MANAGER_STORAGE_IO_ERROR;
        }
        if (io->sync_device(io->context) < 0) {
            (void)io->rename_path(io->context, rollback, target);
            (void)io->remove_file(io->context, temporary);
            return MANAGER_STORAGE_IO_ERROR;
        }
    }
    if (io->rename_path(io->context, temporary, target) < 0) {
        if (had_old)
            (void)io->rename_path(io->context, rollback, target);
        (void)io->remove_file(io->context, temporary);
        (void)io->sync_device(io->context);
        return MANAGER_STORAGE_IO_ERROR;
    }
    result = manager_storage_hash_path(io, target, actual, &actual_size);
    if (result != MANAGER_STORAGE_OK || actual_size != expected_size ||
        memcmp(expected, actual, sizeof(actual)) != 0) {
        manager_storage_rollback_promotion(io, target, temporary, rollback,
                                           had_old);
        return result == MANAGER_STORAGE_OK ? MANAGER_STORAGE_HASH_MISMATCH
                                            : result;
    }
    if (io->sync_device(io->context) < 0) {
        manager_storage_rollback_promotion(io, target, temporary, rollback,
                                           had_old);
        return MANAGER_STORAGE_IO_ERROR;
    }
    if (had_old) {
        if (io->rename_path(io->context, rollback, archive) < 0) {
            manager_storage_rollback_promotion(io, target, temporary, rollback,
                                               1);
            return MANAGER_STORAGE_IO_ERROR;
        }
        if (io->sync_device(io->context) < 0)
            warning = 1;
    }
    return warning ? MANAGER_STORAGE_SYNC_WARNING : MANAGER_STORAGE_OK;
}
#endif
