/*
 * In-memory manager_storage_io for the host tests (manager_storage_host_test
 * and manager_settings_host_test).  Moved verbatim out of
 * manager_storage_host_test.c: flat node table, per-operation fault
 * injection (fail the Nth occurrence of one vtable operation), and the
 * refusal to rename onto an existing destination that the storage protocols
 * rely on.  A test may define FAKE_DATA_CAPACITY before including this header
 * when it needs nodes larger than 4 KiB (the settings test reads a 16 KiB
 * log head); the storage test keeps the historical size.
 */
#ifndef ISAAC_MANAGER_FAKE_FS_H
#define ISAAC_MANAGER_FAKE_FS_H

#include "manager_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAKE_MAX_NODES 80
#define FAKE_MAX_HANDLES 24
#ifndef FAKE_DATA_CAPACITY
#define FAKE_DATA_CAPACITY 4096u
#endif

enum fake_operation {
    FAKE_OP_STAT = 0,
    FAKE_OP_OPEN_READ,
    FAKE_OP_OPEN_WRITE,
    FAKE_OP_READ,
    FAKE_OP_WRITE,
    FAKE_OP_SYNC_FILE,
    FAKE_OP_CLOSE,
    FAKE_OP_REMOVE,
    FAKE_OP_RENAME,
    FAKE_OP_MKDIR,
    FAKE_OP_SYNC_DEVICE,
    FAKE_OP_OPEN_DIR,
    FAKE_OP_READ_DIR,
    FAKE_OP_CLOSE_DIR,
    FAKE_OP_COUNT,
};

struct fake_node {
    char path[MANAGER_STORAGE_PATH_CAPACITY];
    enum manager_storage_node_kind kind;
    unsigned char data[FAKE_DATA_CAPACITY];
    size_t size;
};

struct fake_handle {
    int used;
    int node;
    size_t offset;
    int directory;
    size_t directory_cursor;
};

struct fake_fs {
    struct fake_node nodes[FAKE_MAX_NODES];
    size_t node_count;
    struct fake_handle handles[FAKE_MAX_HANDLES];
    int operation_counts[FAKE_OP_COUNT];
    enum fake_operation fail_operation;
    int fail_at;
    int corrupt_restore_temp;
    /* Optional hooks, inactive while NULL (the storage test never sets
     * them): stat reports every path with this prefix as an existing file
     * (models a full 0000-9999 archive range without 10000 nodes); the
     * first byte read at offset 0 from a node whose path contains this
     * substring is flipped (readback-verification faults). */
    const char *pretend_exists_prefix;
    const char *corrupt_read_path_substring;
};

static unsigned int checks;

static void require(int condition, const char *message)
{
    ++checks;
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static int fake_should_fail(struct fake_fs *fs, enum fake_operation operation)
{
    ++fs->operation_counts[operation];
    return fs->fail_operation == operation &&
           fs->operation_counts[operation] == fs->fail_at;
}

static int fake_find(const struct fake_fs *fs, const char *path)
{
    size_t index;
    for (index = 0u; index < fs->node_count; ++index) {
        if (strcmp(fs->nodes[index].path, path) == 0)
            return (int)index;
    }
    return -1;
}

static struct fake_node *fake_add_node(struct fake_fs *fs, const char *path,
                                       enum manager_storage_node_kind kind,
                                       const void *data, size_t size)
{
    struct fake_node *node;
    require(fs->node_count < FAKE_MAX_NODES, "fake node capacity");
    require(strlen(path) < MANAGER_STORAGE_PATH_CAPACITY, "fake path capacity");
    require(size <= FAKE_DATA_CAPACITY, "fake data capacity");
    require(fake_find(fs, path) < 0, "fake node must be unique");
    node = &fs->nodes[fs->node_count++];
    memset(node, 0, sizeof(*node));
    (void)snprintf(node->path, sizeof(node->path), "%s", path);
    node->kind = kind;
    if (size != 0u)
        memcpy(node->data, data, size);
    node->size = size;
    return node;
}

static void fake_add_dir(struct fake_fs *fs, const char *path)
{
    (void)fake_add_node(fs, path, MANAGER_STORAGE_NODE_DIRECTORY, NULL, 0u);
}

static void fake_add_file(struct fake_fs *fs, const char *path,
                          const char *contents)
{
    (void)fake_add_node(fs, path, MANAGER_STORAGE_NODE_FILE, contents,
                        strlen(contents));
}

static int fake_allocate_handle(struct fake_fs *fs, int node, int directory)
{
    int index;
    for (index = 0; index < FAKE_MAX_HANDLES; ++index) {
        if (!fs->handles[index].used) {
            memset(&fs->handles[index], 0, sizeof(fs->handles[index]));
            fs->handles[index].used = 1;
            fs->handles[index].node = node;
            fs->handles[index].directory = directory;
            return index + 1;
        }
    }
    return -1;
}

static struct fake_handle *fake_handle(struct fake_fs *fs, int handle)
{
    if (handle <= 0 || handle > FAKE_MAX_HANDLES ||
        !fs->handles[handle - 1].used)
        return NULL;
    return &fs->handles[handle - 1];
}

static int fake_stat(void *context, const char *path,
                     enum manager_storage_node_kind *kind, uint64_t *size)
{
    struct fake_fs *fs = context;
    const int index = fake_find(fs, path);
    if (fake_should_fail(fs, FAKE_OP_STAT))
        return -1;
    if (index < 0 && fs->pretend_exists_prefix != NULL &&
        strncmp(path, fs->pretend_exists_prefix,
                strlen(fs->pretend_exists_prefix)) == 0) {
        *kind = MANAGER_STORAGE_NODE_FILE;
        *size = 0u;
        return 0;
    }
    if (index < 0)
        return 1;
    *kind = fs->nodes[index].kind;
    *size = fs->nodes[index].size;
    return 0;
}

static int fake_open_read(void *context, const char *path)
{
    struct fake_fs *fs = context;
    const int index = fake_find(fs, path);
    if (fake_should_fail(fs, FAKE_OP_OPEN_READ) || index < 0 ||
        fs->nodes[index].kind != MANAGER_STORAGE_NODE_FILE)
        return -1;
    return fake_allocate_handle(fs, index, 0);
}

static int fake_open_write(void *context, const char *path)
{
    struct fake_fs *fs = context;
    struct fake_node *node;
    if (fake_should_fail(fs, FAKE_OP_OPEN_WRITE) || fake_find(fs, path) >= 0)
        return -1;
    node = fake_add_node(fs, path, MANAGER_STORAGE_NODE_FILE, NULL, 0u);
    return fake_allocate_handle(fs, (int)(node - fs->nodes), 0);
}

static int fake_read(void *context, int handle, void *data, size_t size)
{
    struct fake_fs *fs = context;
    struct fake_handle *opened = fake_handle(fs, handle);
    struct fake_node *node;
    size_t amount;
    if (fake_should_fail(fs, FAKE_OP_READ) || opened == NULL ||
        opened->directory)
        return -1;
    node = &fs->nodes[opened->node];
    if (opened->offset == node->size)
        return 0;
    amount = node->size - opened->offset;
    if (amount > size)
        amount = size;
    memcpy(data, node->data + opened->offset, amount);
    if (fs->corrupt_restore_temp && opened->offset == 0u && amount != 0u &&
        strstr(node->path, ".manager-restore-slot") != NULL)
        ((unsigned char *)data)[0] ^= 0x80u;
    if (fs->corrupt_read_path_substring != NULL && opened->offset == 0u &&
        amount != 0u &&
        strstr(node->path, fs->corrupt_read_path_substring) != NULL)
        ((unsigned char *)data)[0] ^= 0x80u;
    opened->offset += amount;
    return (int)amount;
}

static int fake_write(void *context, int handle, const void *data, size_t size)
{
    struct fake_fs *fs = context;
    struct fake_handle *opened = fake_handle(fs, handle);
    struct fake_node *node;
    if (fake_should_fail(fs, FAKE_OP_WRITE) || opened == NULL ||
        opened->directory)
        return -1;
    node = &fs->nodes[opened->node];
    if (opened->offset + size > sizeof(node->data))
        return -1;
    memcpy(node->data + opened->offset, data, size);
    opened->offset += size;
    if (node->size < opened->offset)
        node->size = opened->offset;
    return (int)size;
}

static int fake_sync_file(void *context, int handle)
{
    struct fake_fs *fs = context;
    return fake_should_fail(fs, FAKE_OP_SYNC_FILE) ||
                   fake_handle(fs, handle) == NULL
               ? -1
               : 0;
}

static int fake_close(void *context, int handle)
{
    struct fake_fs *fs = context;
    struct fake_handle *opened = fake_handle(fs, handle);
    int failed = fake_should_fail(fs, FAKE_OP_CLOSE);
    if (opened == NULL)
        return -1;
    opened->used = 0;
    return failed ? -1 : 0;
}

static int fake_remove(void *context, const char *path)
{
    struct fake_fs *fs = context;
    const int index = fake_find(fs, path);
    if (fake_should_fail(fs, FAKE_OP_REMOVE) || index < 0 ||
        fs->nodes[index].kind != MANAGER_STORAGE_NODE_FILE)
        return -1;
    fs->nodes[index] = fs->nodes[fs->node_count - 1u];
    --fs->node_count;
    return 0;
}

static int fake_rename(void *context, const char *source,
                       const char *destination)
{
    struct fake_fs *fs = context;
    const int index = fake_find(fs, source);
    if (fake_should_fail(fs, FAKE_OP_RENAME) || index < 0 ||
        fake_find(fs, destination) >= 0 ||
        strlen(destination) >= sizeof(fs->nodes[index].path))
        return -1;
    (void)snprintf(fs->nodes[index].path, sizeof(fs->nodes[index].path), "%s",
                   destination);
    return 0;
}

static int fake_mkdir(void *context, const char *path)
{
    struct fake_fs *fs = context;
    if (fake_should_fail(fs, FAKE_OP_MKDIR) || fake_find(fs, path) >= 0)
        return -1;
    fake_add_dir(fs, path);
    return 0;
}

static int fake_sync_device(void *context)
{
    struct fake_fs *fs = context;
    return fake_should_fail(fs, FAKE_OP_SYNC_DEVICE) ? -1 : 0;
}

static int fake_open_dir(void *context, const char *path)
{
    struct fake_fs *fs = context;
    const int index = fake_find(fs, path);
    if (fake_should_fail(fs, FAKE_OP_OPEN_DIR) || index < 0 ||
        fs->nodes[index].kind != MANAGER_STORAGE_NODE_DIRECTORY)
        return -1;
    return fake_allocate_handle(fs, index, 1);
}

static int fake_direct_child(const char *directory, const char *path,
                             const char **name)
{
    const size_t root_length = strlen(directory);
    const char *tail;
    if (strncmp(directory, path, root_length) != 0 ||
        path[root_length] != '/')
        return 0;
    tail = path + root_length + 1u;
    if (*tail == '\0' || strchr(tail, '/') != NULL)
        return 0;
    *name = tail;
    return 1;
}

static int fake_read_dir(void *context, int handle,
                         struct manager_storage_dir_entry *entry)
{
    struct fake_fs *fs = context;
    struct fake_handle *opened = fake_handle(fs, handle);
    const char *directory;
    if (fake_should_fail(fs, FAKE_OP_READ_DIR) || opened == NULL ||
        !opened->directory)
        return -1;
    directory = fs->nodes[opened->node].path;
    while (opened->directory_cursor < fs->node_count) {
        struct fake_node *node = &fs->nodes[opened->directory_cursor++];
        const char *name;
        if (!fake_direct_child(directory, node->path, &name))
            continue;
        memset(entry, 0, sizeof(*entry));
        (void)snprintf(entry->name, sizeof(entry->name), "%s", name);
        entry->kind = node->kind;
        entry->size = node->size;
        return 1;
    }
    return 0;
}

static int fake_close_dir(void *context, int handle)
{
    struct fake_fs *fs = context;
    struct fake_handle *opened = fake_handle(fs, handle);
    int failed = fake_should_fail(fs, FAKE_OP_CLOSE_DIR);
    if (opened == NULL || !opened->directory)
        return -1;
    opened->used = 0;
    return failed ? -1 : 0;
}

static struct manager_storage_io fake_io(struct fake_fs *fs)
{
    struct manager_storage_io io = {
        fs,          fake_stat,      fake_open_read, fake_open_write,
        fake_read,   fake_write,     fake_sync_file, fake_close,
        fake_remove, fake_rename,    fake_mkdir,     fake_sync_device,
        fake_open_dir, fake_read_dir, fake_close_dir,
    };
    return io;
}

static void fake_init(struct fake_fs *fs)
{
    memset(fs, 0, sizeof(*fs));
    fs->fail_operation = FAKE_OP_COUNT;
    fake_add_dir(fs, MANAGER_STORAGE_ROOT);
    fake_add_dir(fs, MANAGER_STORAGE_MOD_ROOT);
    fake_add_dir(fs, MANAGER_STORAGE_SAVE_ROOT);
}

static void fake_fail(struct fake_fs *fs, enum fake_operation operation,
                      int occurrence)
{
    memset(fs->operation_counts, 0, sizeof(fs->operation_counts));
    fs->fail_operation = operation;
    fs->fail_at = occurrence;
}

static void fake_no_fail(struct fake_fs *fs)
{
    memset(fs->operation_counts, 0, sizeof(fs->operation_counts));
    fs->fail_operation = FAKE_OP_COUNT;
    fs->fail_at = 0;
}

static void require_file(struct fake_fs *fs, const char *path,
                         const char *contents, const char *message)
{
    const int index = fake_find(fs, path);
    const size_t size = strlen(contents);
    require(index >= 0, message);
    require(fs->nodes[index].kind == MANAGER_STORAGE_NODE_FILE, message);
    require(fs->nodes[index].size == size, message);
    require(memcmp(fs->nodes[index].data, contents, size) == 0, message);
}

#endif
