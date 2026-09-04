#include "manager_sha256.h"
#include "manager_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAKE_MAX_NODES 80
#define FAKE_MAX_HANDLES 24
#define FAKE_DATA_CAPACITY 4096u

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

static void prove_sha256(void)
{
    static const unsigned char expected[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    };
    struct manager_sha256_context context;
    unsigned char actual[32];
    manager_sha256_init(&context);
    manager_sha256_update(&context, "a", 1u);
    manager_sha256_update(&context, "bc", 2u);
    manager_sha256_final(&context, actual);
    require(memcmp(actual, expected, sizeof(actual)) == 0,
            "SHA-256 abc vector");
}

static void test_mods(void)
{
    struct fake_fs fs;
    struct manager_storage_io io;
    struct manager_mod_entry mods[8];
    size_t count;
    size_t rejected;
    const char *alpha = MANAGER_STORAGE_MOD_ROOT "/Alpha";
    const char *beta = MANAGER_STORAGE_MOD_ROOT "/Beta";
    const char *beta_marker = MANAGER_STORAGE_MOD_ROOT "/Beta/disable.it";
    const char *beta_stash =
        MANAGER_STORAGE_MOD_ROOT "/Beta/.isaac-manager-disable-marker";

    fake_init(&fs);
    io = fake_io(&fs);
    fake_add_dir(&fs, alpha);
    fake_add_dir(&fs, beta);
    fake_add_file(&fs, beta_marker, "owned marker");
    fake_add_file(&fs, MANAGER_STORAGE_MOD_ROOT "/not-a-mod", "x");
    (void)fake_add_node(&fs, MANAGER_STORAGE_MOD_ROOT "/link",
                        MANAGER_STORAGE_NODE_OTHER, NULL, 0u);

    require(manager_storage_list_mods(&io, mods, 8u, &count, &rejected) == 0,
            "list mods");
    require(count == 2u && rejected == 2u, "mod list boundaries");
    require(strcmp(mods[0].name, "Alpha") == 0 && !mods[0].disabled,
            "enabled mod listed");
    require(strcmp(mods[1].name, "Beta") == 0 && mods[1].disabled,
            "disabled mod listed");

    require(manager_storage_set_mod_enabled(&io, "Alpha", 0) == 0,
            "disable mod");
    require_file(&fs, MANAGER_STORAGE_MOD_ROOT "/Alpha/disable.it", "",
                 "disable.it created");
    require(manager_storage_set_mod_enabled(&io, "Beta", 1) == 0,
            "enable mod");
    require(fake_find(&fs, beta_marker) < 0, "disable.it removed semantically");
    require_file(&fs, beta_stash, "owned marker", "marker contents retained");
    require(manager_storage_set_mod_enabled(&io, "Beta", 0) == 0,
            "disable mod again");
    require_file(&fs, beta_marker, "owned marker", "marker restored exactly");
    require(fake_find(&fs, beta_stash) < 0, "marker stash consumed");

    fake_fail(&fs, FAKE_OP_SYNC_DEVICE, 1);
    require(manager_storage_set_mod_enabled(&io, "Beta", 1) < 0,
            "enable sync failure reported");
    fake_no_fail(&fs);
    require_file(&fs, beta_marker, "owned marker",
                 "enable sync failure rolled back");
    require(manager_storage_set_mod_enabled(&io, "../Beta", 0) ==
                MANAGER_STORAGE_INVALID,
            "mod traversal rejected");

    fake_fail(&fs, FAKE_OP_STAT, 1);
    require(manager_storage_list_mods(&io, mods, 8u, &count, &rejected) ==
                MANAGER_STORAGE_IO_ERROR,
            "mod root stat error is not absence");
    fake_fail(&fs, FAKE_OP_STAT, 3);
    require(manager_storage_list_mods(&io, mods, 8u, &count, &rejected) ==
                MANAGER_STORAGE_IO_ERROR,
            "mod marker stat error is not absence");
    require(count == 0u, "failed mod scan exposes no partial list");
    fake_fail(&fs, FAKE_OP_OPEN_DIR, 1);
    require(manager_storage_list_mods(&io, mods, 8u, &count, &rejected) ==
                MANAGER_STORAGE_IO_ERROR,
            "mod root open error is not absence");
}

static void test_save_listing(void)
{
    struct fake_fs fs;
    struct manager_storage_io io;
    struct manager_save_slot slots[3];
    struct manager_save_backup backups[8];
    size_t count;
    size_t rejected;

    fake_init(&fs);
    io = fake_io(&fs);
    fake_add_file(&fs, MANAGER_STORAGE_SAVE_ROOT "/persistentgamedata1.dat",
                  "slot one");
    fake_add_file(&fs,
                  MANAGER_STORAGE_SAVE_ROOT
                  "/20260821.persistentgamedata1.dat",
                  "dated");
    fake_add_dir(&fs, MANAGER_STORAGE_BACKUP_ROOT);
    fake_add_file(&fs, MANAGER_STORAGE_BACKUP_ROOT "/slot2-0007.dat", "local");
    fake_add_dir(&fs, MANAGER_STORAGE_SYNC_BACKUP_ROOT);
    fake_add_dir(&fs, MANAGER_STORAGE_SYNC_BACKUP_ROOT "/session-A");
    fake_add_dir(&fs, MANAGER_STORAGE_SYNC_BACKUP_ROOT
                      "/session-A/Documents");
    fake_add_dir(&fs, MANAGER_STORAGE_SYNC_BACKUP_ROOT
                      "/session-A/Documents/My Games");
    fake_add_dir(&fs, MANAGER_STORAGE_SYNC_BACKUP_ROOT
                      "/session-A/Documents/My Games/Binding of Isaac Repentance");
    fake_add_file(&fs,
                  MANAGER_STORAGE_SYNC_BACKUP_ROOT
                  "/session-A/Documents/My Games/Binding of Isaac Repentance/"
                  "persistentgamedata3.dat",
                  "sync");

    require(manager_storage_get_save_slots(&io, slots) == 0, "list slots");
    require(slots[0].present && slots[0].size == 8u, "slot one present");
    require(!slots[1].present && !slots[2].present, "missing slots visible");
    require(manager_storage_list_save_backups(&io, backups, 8u, &count,
                                              &rejected) == 0,
            "list save backups");
    require(count == 3u && rejected == 0u, "all backup origins listed");
    require(backups[0].origin == MANAGER_SAVE_BACKUP_SYNC &&
                backups[0].slot == 3,
            "sync backup sorted and typed");
    require(backups[1].origin == MANAGER_SAVE_BACKUP_LOCAL &&
                backups[1].slot == 2,
            "local backup typed");
    require(backups[2].origin == MANAGER_SAVE_BACKUP_DATED &&
                backups[2].slot == 1,
            "dated backup typed");

    fake_fail(&fs, FAKE_OP_STAT, 1);
    require(manager_storage_list_save_backups(&io, backups, 8u, &count,
                                              &rejected) ==
                MANAGER_STORAGE_IO_ERROR,
            "save root stat error is not absence");
    fake_fail(&fs, FAKE_OP_OPEN_DIR, 1);
    require(manager_storage_list_save_backups(&io, backups, 8u, &count,
                                              &rejected) ==
                MANAGER_STORAGE_IO_ERROR,
            "save root open error is not absence");
    fake_fail(&fs, FAKE_OP_STAT, 4);
    require(manager_storage_list_save_backups(&io, backups, 8u, &count,
                                              &rejected) ==
                MANAGER_STORAGE_IO_ERROR,
            "sync backup stat error is not absence");
    require(count == 0u, "failed save scan exposes no partial list");
}

static void setup_restore(struct fake_fs *fs, struct manager_save_backup *backup)
{
    fake_init(fs);
    fake_add_dir(fs, MANAGER_STORAGE_BACKUP_ROOT);
    fake_add_file(fs, MANAGER_STORAGE_SAVE_ROOT "/persistentgamedata1.dat",
                  "OLD SLOT");
    fake_add_file(fs,
                  MANAGER_STORAGE_SAVE_ROOT
                  "/20260821.persistentgamedata1.dat",
                  "NEW SLOT");
    memset(backup, 0, sizeof(*backup));
    backup->slot = 1;
    backup->origin = MANAGER_SAVE_BACKUP_DATED;
    backup->size = 8u;
    (void)snprintf(backup->label, sizeof(backup->label), "DATED S1");
    (void)snprintf(backup->path, sizeof(backup->path), "%s",
                   MANAGER_STORAGE_SAVE_ROOT
                   "/20260821.persistentgamedata1.dat");
}

static void test_backup_and_restore(void)
{
    struct fake_fs fs;
    struct manager_storage_io io;
    struct manager_save_backup source;
    struct manager_save_backup created;
    int result;

    setup_restore(&fs, &source);
    io = fake_io(&fs);
    result = manager_storage_backup_slot(&io, 1, &created);
    require(result == MANAGER_STORAGE_OK, "backup active slot");
    require(created.origin == MANAGER_SAVE_BACKUP_LOCAL && created.slot == 1,
            "created backup metadata");
    require_file(&fs, MANAGER_STORAGE_BACKUP_ROOT "/slot1-0000.dat",
                 "OLD SLOT", "active slot backup bytes");

    result = manager_storage_restore_backup(&io, &source);
    require(result == MANAGER_STORAGE_OK, "restore dated backup");
    require_file(&fs, MANAGER_STORAGE_SAVE_ROOT "/persistentgamedata1.dat",
                 "NEW SLOT", "restored target bytes");
    require_file(&fs, MANAGER_STORAGE_BACKUP_ROOT "/slot1-0001.dat",
                 "OLD SLOT", "pre-restore slot retained");
    require_file(&fs,
                 MANAGER_STORAGE_SAVE_ROOT
                 "/20260821.persistentgamedata1.dat",
                 "NEW SLOT", "restore source retained");
    require(fake_find(&fs, MANAGER_STORAGE_SAVE_ROOT
                           "/.manager-restore-slot1.tmp") < 0,
            "restore temp removed");
    require(fake_find(&fs, MANAGER_STORAGE_SAVE_ROOT
                           "/.manager-rollback-slot1.dat") < 0,
            "rollback marker removed");
}

static void test_restore_faults(void)
{
    static const struct {
        enum fake_operation operation;
        int occurrence;
        const char *name;
    } failures[] = {
        {FAKE_OP_WRITE, 1, "write"},
        {FAKE_OP_SYNC_FILE, 1, "file sync"},
        {FAKE_OP_RENAME, 1, "old-to-rollback rename"},
        {FAKE_OP_RENAME, 2, "promotion rename"},
        {FAKE_OP_RENAME, 3, "archive rename"},
        {FAKE_OP_SYNC_DEVICE, 2, "rollback sync"},
        {FAKE_OP_SYNC_DEVICE, 3, "promotion sync"},
    };
    size_t index;
    for (index = 0u; index < sizeof(failures) / sizeof(failures[0]); ++index) {
        struct fake_fs fs;
        struct manager_storage_io io;
        struct manager_save_backup source;
        setup_restore(&fs, &source);
        io = fake_io(&fs);
        fake_fail(&fs, failures[index].operation, failures[index].occurrence);
        require(manager_storage_restore_backup(&io, &source) < 0,
                failures[index].name);
        fake_no_fail(&fs);
        require_file(&fs,
                     MANAGER_STORAGE_SAVE_ROOT "/persistentgamedata1.dat",
                     "OLD SLOT", failures[index].name);
        require_file(&fs,
                     MANAGER_STORAGE_SAVE_ROOT
                     "/20260821.persistentgamedata1.dat",
                     "NEW SLOT", failures[index].name);
    }
    {
        struct fake_fs fs;
        struct manager_storage_io io;
        struct manager_save_backup source;
        setup_restore(&fs, &source);
        io = fake_io(&fs);
        fs.corrupt_restore_temp = 1;
        require(manager_storage_restore_backup(&io, &source) ==
                    MANAGER_STORAGE_HASH_MISMATCH,
                "readback corruption detected");
        require_file(&fs,
                     MANAGER_STORAGE_SAVE_ROOT "/persistentgamedata1.dat",
                     "OLD SLOT", "readback corruption preserves target");
    }
    {
        struct fake_fs fs;
        struct manager_storage_io io;
        struct manager_save_backup source;
        setup_restore(&fs, &source);
        io = fake_io(&fs);
        fake_fail(&fs, FAKE_OP_SYNC_DEVICE, 4);
        require(manager_storage_restore_backup(&io, &source) ==
                    MANAGER_STORAGE_SYNC_WARNING,
                "final archive sync warning is explicit");
        require_file(&fs,
                     MANAGER_STORAGE_SAVE_ROOT "/persistentgamedata1.dat",
                     "NEW SLOT", "sync warning leaves verified new target");
        require_file(&fs, MANAGER_STORAGE_BACKUP_ROOT "/slot1-0000.dat",
                     "OLD SLOT", "sync warning retains old target");
    }
}

static void test_recovery_and_rejection(void)
{
    struct fake_fs fs;
    struct manager_storage_io io;
    struct manager_save_backup source;

    fake_init(&fs);
    fake_add_dir(&fs, MANAGER_STORAGE_BACKUP_ROOT);
    fake_add_file(&fs, MANAGER_STORAGE_SAVE_ROOT "/persistentgamedata1.dat",
                  "UNVERIFIED NEW");
    fake_add_file(&fs,
                  MANAGER_STORAGE_SAVE_ROOT "/.manager-rollback-slot1.dat",
                  "KNOWN OLD");
    fake_add_file(&fs,
                  MANAGER_STORAGE_SAVE_ROOT "/.manager-restore-slot1.tmp",
                  "STAGED");
    io = fake_io(&fs);
    require(manager_storage_recover(&io) == MANAGER_STORAGE_OK,
            "recover interrupted restore");
    require_file(&fs, MANAGER_STORAGE_SAVE_ROOT "/persistentgamedata1.dat",
                 "KNOWN OLD", "recovery restores known old target");
    require_file(&fs,
                 MANAGER_STORAGE_BACKUP_ROOT "/recovered-slot1-0000.dat",
                 "UNVERIFIED NEW", "recovery quarantines promoted target");
    require(fake_find(&fs, MANAGER_STORAGE_SAVE_ROOT
                           "/.manager-restore-slot1.tmp") < 0,
            "recovery removes stale staging file");

    setup_restore(&fs, &source);
    io = fake_io(&fs);
    (void)snprintf(source.path, sizeof(source.path), "%s",
                   MANAGER_STORAGE_ROOT "/../outside.dat");
    require(manager_storage_restore_backup(&io, &source) ==
                MANAGER_STORAGE_INVALID,
            "arbitrary restore source rejected");
    require_file(&fs, MANAGER_STORAGE_SAVE_ROOT "/persistentgamedata1.dat",
                 "OLD SLOT", "rejected source preserves target");
}

int main(void)
{
    prove_sha256();
    test_mods();
    test_save_listing();
    test_backup_and_restore();
    test_restore_faults();
    test_recovery_and_rejection();
    printf("manager storage host tests: PASS (%u checks)\n", checks);
    return 0;
}
