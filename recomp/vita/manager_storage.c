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
        result = manager_storage_add_backup(entries, capacity, used, skipped,
                                            slot, origin, entry.size, label,
                                            path);
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

int manager_storage_recover(const struct manager_storage_io *io)
{
    int slot;
    int warning = 0;
    if (!manager_storage_io_valid(io))
        return MANAGER_STORAGE_INVALID;
    for (slot = 1; slot <= 3; ++slot) {
        const int result = manager_storage_recover_slot(io, slot);
        if (result < 0)
            return result;
        if (result == MANAGER_STORAGE_SYNC_WARNING)
            warning = 1;
    }
    return warning ? MANAGER_STORAGE_SYNC_WARNING : MANAGER_STORAGE_OK;
}

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
