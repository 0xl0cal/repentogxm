#include "manager_sha256.h"
#include "manager_storage.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_fs.h"

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
#if !defined(ISAAC_VITA_MANAGER_SAVE_GAMESTATE)
    /* Occurrence numbers belong to the single-file protocol; the pair build
     * runs the exhaustive matrix in test_pair_restore_faults instead. */
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
#endif
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
#if defined(ISAAC_VITA_MANAGER_SAVE_GAMESTATE)
        /* recover, asides, verified promotion, archive, then the post-commit
         * sync: the only device sync whose failure is a warning. */
        fake_fail(&fs, FAKE_OP_SYNC_DEVICE, 5);
#else
        fake_fail(&fs, FAKE_OP_SYNC_DEVICE, 4);
#endif
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

#if defined(ISAAC_VITA_MANAGER_SAVE_GAMESTATE)
#define PAIR_P1 MANAGER_STORAGE_SAVE_ROOT "/persistentgamedata1.dat"
#define PAIR_G1 MANAGER_STORAGE_SAVE_ROOT "/gamestate1.dat"
#define PAIR_P2 MANAGER_STORAGE_SAVE_ROOT "/persistentgamedata2.dat"
#define PAIR_G2 MANAGER_STORAGE_SAVE_ROOT "/gamestate2.dat"
#define PAIR_G3 MANAGER_STORAGE_SAVE_ROOT "/gamestate3.dat"
#define PAIR_BACKUP_P MANAGER_STORAGE_BACKUP_ROOT "/slot1-0000.dat"
#define PAIR_BACKUP_G MANAGER_STORAGE_BACKUP_ROOT "/slot1-0000.gamestate.dat"
#define PAIR_ARCHIVE_P MANAGER_STORAGE_BACKUP_ROOT "/slot1-0001.dat"
#define PAIR_ARCHIVE_G MANAGER_STORAGE_BACKUP_ROOT "/slot1-0001.gamestate.dat"
#define PAIR_MARKER MANAGER_STORAGE_SAVE_ROOT "/.manager-pair-slot1.inflight"
#define PAIR_ROLLBACK_P                                                        \
    MANAGER_STORAGE_SAVE_ROOT "/.manager-pair-rollback-slot1.dat"
#define PAIR_ROLLBACK_G                                                        \
    MANAGER_STORAGE_SAVE_ROOT "/.manager-pair-rollback-slot1.gamestate.dat"
#define PAIR_TEMP_P MANAGER_STORAGE_SAVE_ROOT "/.manager-restore-slot1.tmp"
#define PAIR_TEMP_G                                                            \
    MANAGER_STORAGE_SAVE_ROOT "/.manager-restore-slot1.gamestate.tmp"
#define PAIR_QUARANTINE_P MANAGER_STORAGE_BACKUP_ROOT "/recovered-slot1-0000.dat"
#define PAIR_QUARANTINE_G                                                      \
    MANAGER_STORAGE_BACKUP_ROOT "/recovered-slot1-0000.gamestate.dat"
#define PAIR_DATED MANAGER_STORAGE_SAVE_ROOT "/20260821.persistentgamedata1.dat"

static int fake_count_containing(const struct fake_fs *fs, const char *needle)
{
    size_t index;
    int count = 0;
    for (index = 0u; index < fs->node_count; ++index) {
        if (strstr(fs->nodes[index].path, needle) != NULL)
            ++count;
    }
    return count;
}

static void require_absent(struct fake_fs *fs, const char *path,
                           const char *message)
{
    require(fake_find(fs, path) < 0, message);
}

static void require_no_inflight(struct fake_fs *fs, const char *message)
{
    require(fake_count_containing(fs, ".manager-restore-slot") == 0, message);
    require(fake_count_containing(fs, ".manager-pair-") == 0, message);
    require(fake_count_containing(fs, ".manager-backup-slot") == 0, message);
    require(fake_count_containing(fs, ".manager-rollback-slot") == 0, message);
}

/* NULL means the file must be absent. */
static void require_live(struct fake_fs *fs, const char *persistent,
                         const char *gamestate, const char *message)
{
    if (persistent != NULL)
        require_file(fs, PAIR_P1, persistent, message);
    else
        require_absent(fs, PAIR_P1, message);
    if (gamestate != NULL)
        require_file(fs, PAIR_G1, gamestate, message);
    else
        require_absent(fs, PAIR_G1, message);
}

/* manager_draw_saves shows 47 characters through manager_display_text, which
 * keeps A-Z 0-9 space . - _ (lowercase is folded) and turns the rest into
 * '?'. */
static void require_display_label(const char *label, const char *message)
{
    const unsigned char *cursor = (const unsigned char *)label;
    require(strlen(label) <= 47u, message);
    while (*cursor != '\0') {
        require(isalnum(*cursor) || *cursor == ' ' || *cursor == '.' ||
                    *cursor == '-' || *cursor == '_',
                message);
        ++cursor;
    }
}

static void local_backup(struct manager_save_backup *backup, const char *path,
                         int has_gamestate)
{
    memset(backup, 0, sizeof(*backup));
    backup->slot = 1;
    backup->origin = MANAGER_SAVE_BACKUP_LOCAL;
    backup->size = 5u;
    backup->has_gamestate = has_gamestate;
    (void)snprintf(backup->label, sizeof(backup->label), "LOCAL S1");
    (void)snprintf(backup->path, sizeof(backup->path), "%s", path);
}

static void setup_pair(struct fake_fs *fs, const char *live_persistent,
                       const char *live_gamestate, int backup_has_gamestate)
{
    fake_init(fs);
    fake_add_dir(fs, MANAGER_STORAGE_BACKUP_ROOT);
    if (live_persistent != NULL)
        fake_add_file(fs, PAIR_P1, live_persistent);
    if (live_gamestate != NULL)
        fake_add_file(fs, PAIR_G1, live_gamestate);
    fake_add_file(fs, PAIR_BACKUP_P, "NEW P");
    if (backup_has_gamestate)
        fake_add_file(fs, PAIR_BACKUP_G, "NEW G");
}

static void test_pair_slots_and_backup(void)
{
    struct fake_fs fs;
    struct manager_storage_io io;
    struct manager_save_slot slots[3];
    struct manager_save_backup backups[8];
    struct manager_save_backup created;
    size_t count;
    size_t rejected;
    size_t index;

    fake_init(&fs);
    io = fake_io(&fs);
    fake_add_dir(&fs, MANAGER_STORAGE_BACKUP_ROOT);
    fake_add_file(&fs, PAIR_P1, "PERSIST ONE");
    fake_add_file(&fs, PAIR_G1, "RUN ONE");
    fake_add_file(&fs, PAIR_P2, "PERSIST TWO");

    require(manager_storage_get_save_slots(&io, slots) == MANAGER_STORAGE_OK,
            "list slots with run state");
    require(slots[0].present && slots[0].gamestate_present &&
                slots[0].gamestate_size == 7u,
            "slot one carries run state");
    require(slots[1].present && !slots[1].gamestate_present &&
                slots[1].gamestate_size == 0u,
            "slot two has no run state");
    require(!slots[2].present && !slots[2].gamestate_present,
            "missing slot has no run state");

    require(manager_storage_backup_slot(&io, 1, &created) == MANAGER_STORAGE_OK,
            "backup slot with run state");
    require(created.has_gamestate && created.gamestate_size == 7u &&
                created.size == 11u,
            "created pair metadata");
    require(strcmp(created.label, "LOCAL S1 slot1-0000.dat WITH RUN") == 0,
            "created pair label");
    require(strcmp(created.path, PAIR_BACKUP_P) == 0, "created pair path");
    require_display_label(created.label, "pair label fits the row");
    require_file(&fs, PAIR_BACKUP_P, "PERSIST ONE", "pair backup persistent");
    require_file(&fs, PAIR_BACKUP_G, "RUN ONE", "pair backup run state");

    require(manager_storage_backup_slot(&io, 2, &created) == MANAGER_STORAGE_OK,
            "backup slot without run state");
    require(!created.has_gamestate && created.gamestate_size == 0u,
            "created single metadata");
    require(strcmp(created.label, "LOCAL S2 slot2-0000.dat") == 0,
            "created single label");
    require_file(&fs, MANAGER_STORAGE_BACKUP_ROOT "/slot2-0000.dat",
                 "PERSIST TWO", "single backup persistent");
    require_absent(&fs, MANAGER_STORAGE_BACKUP_ROOT "/slot2-0000.gamestate.dat",
                   "single backup has no companion");

    /* An orphaned companion burns its index instead of being adopted. */
    fake_add_file(&fs, PAIR_ARCHIVE_G, "ORPHAN");
    require(manager_storage_backup_slot(&io, 1, &created) == MANAGER_STORAGE_OK,
            "backup past an orphaned companion");
    require(strcmp(created.path,
                   MANAGER_STORAGE_BACKUP_ROOT "/slot1-0002.dat") == 0,
            "orphaned companion index skipped");
    require_file(&fs, MANAGER_STORAGE_BACKUP_ROOT "/slot1-0002.gamestate.dat",
                 "RUN ONE", "third backup companion");
    require_file(&fs, PAIR_ARCHIVE_G, "ORPHAN", "orphan untouched");

    require(manager_storage_list_save_backups(&io, backups, 8u, &count,
                                              &rejected) == MANAGER_STORAGE_OK,
            "list pair backups");
    require(count == 3u && rejected == 0u, "orphan is not a backup");
    require(strcmp(backups[0].path,
                   MANAGER_STORAGE_BACKUP_ROOT "/slot2-0000.dat") == 0 &&
                !backups[0].has_gamestate &&
                strstr(backups[0].label, "WITH RUN") == NULL,
            "single backup listed without flag");
    require(strcmp(backups[1].path,
                   MANAGER_STORAGE_BACKUP_ROOT "/slot1-0002.dat") == 0 &&
                backups[1].has_gamestate && backups[1].gamestate_size == 7u,
            "newest pair listed first among slot one");
    require(strcmp(backups[2].path, PAIR_BACKUP_P) == 0 &&
                backups[2].has_gamestate &&
                strcmp(backups[2].label,
                       "LOCAL S1 slot1-0000.dat WITH RUN") == 0,
            "pair listed with flag");
    for (index = 0u; index < count; ++index)
        require_display_label(backups[index].label, "listed label fits");
    require_file(&fs, PAIR_P1, "PERSIST ONE", "backup leaves live persistent");
    require_file(&fs, PAIR_G1, "RUN ONE", "backup leaves live run state");
    require_no_inflight(&fs, "backup leaves no temporaries");

    fake_add_file(&fs, PAIR_G3, "RUN THREE");
    require(manager_storage_backup_slot(&io, 3, &created) ==
                MANAGER_STORAGE_INVALID,
            "run state without persistent file is not a slot");
    fake_add_dir(&fs, PAIR_G2);
    require(manager_storage_backup_slot(&io, 2, &created) ==
                MANAGER_STORAGE_CONFLICT,
            "directory named like run state is a conflict");
    require(manager_storage_get_save_slots(&io, slots) ==
                MANAGER_STORAGE_CONFLICT,
            "slot listing reports the conflict");
}

static void test_pair_backup_faults(void)
{
    enum fake_operation operation;
    for (operation = FAKE_OP_STAT; operation < FAKE_OP_COUNT; ++operation) {
        int occurrence;
        for (occurrence = 1;; ++occurrence) {
            struct fake_fs fs;
            struct manager_storage_io io;
            struct manager_save_backup created;
            int result;
            int total;
            size_t index;
            fake_init(&fs);
            fake_add_dir(&fs, MANAGER_STORAGE_BACKUP_ROOT);
            fake_add_file(&fs, PAIR_P1, "PERSIST ONE");
            fake_add_file(&fs, PAIR_G1, "RUN ONE");
            io = fake_io(&fs);
            memset(&created, 0, sizeof(created));
            fake_fail(&fs, (enum fake_operation)operation, occurrence);
            result = manager_storage_backup_slot(&io, 1, &created);
            total = fs.operation_counts[operation];
            fake_no_fail(&fs);
            if (total < occurrence) {
                require(result >= 0, "clean backup succeeds");
                break;
            }
            require_file(&fs, PAIR_P1, "PERSIST ONE", "backup fault live P");
            require_file(&fs, PAIR_G1, "RUN ONE", "backup fault live G");
            require(manager_storage_recover(&io) == MANAGER_STORAGE_OK,
                    "recover after backup fault");
            require_no_inflight(&fs, "backup fault leaves no temporaries");
            /* Every listable .dat made with a run state has its companion. */
            for (index = 0u; index < fs.node_count; ++index) {
                const char *name = strrchr(fs.nodes[index].path, '/');
                char companion[MANAGER_STORAGE_PATH_CAPACITY];
                if (name == NULL || strncmp(name, "/slot1-", 7u) != 0 ||
                    strstr(name, ".gamestate.") != NULL)
                    continue;
                require_file(&fs, fs.nodes[index].path, "PERSIST ONE",
                             "published backup bytes");
                (void)snprintf(companion, sizeof(companion),
                               "%.*s.gamestate.dat",
                               (int)(strlen(fs.nodes[index].path) - 4u),
                               fs.nodes[index].path);
                require_file(&fs, companion, "RUN ONE",
                             "published backup has its companion");
            }
            if (result >= 0) {
                require(created.has_gamestate, "successful pair flagged");
                require_file(&fs, created.path, "PERSIST ONE",
                             "successful pair persistent");
            }
        }
        /* A clean backup never removes anything or creates the root. */
        require(occurrence > 1 || operation == FAKE_OP_MKDIR ||
                    operation == FAKE_OP_REMOVE ||
                    operation >= FAKE_OP_OPEN_DIR,
                "every file operation of the backup path is exercised");
    }
}

static void test_pair_restore(void)
{
    struct fake_fs fs;
    struct manager_storage_io io;
    struct manager_save_backup backup;
    struct manager_save_backup backups[4];
    size_t count;
    size_t rejected;

    /* Pair into a slot that has both files; the old pair becomes a backup. */
    setup_pair(&fs, "OLD P", "OLD G", 1);
    io = fake_io(&fs);
    local_backup(&backup, PAIR_BACKUP_P, 1);
    require(manager_storage_restore_backup(&io, &backup) == MANAGER_STORAGE_OK,
            "restore pair");
    require_live(&fs, "NEW P", "NEW G", "pair restored");
    require_file(&fs, PAIR_ARCHIVE_P, "OLD P", "old persistent archived");
    require_file(&fs, PAIR_ARCHIVE_G, "OLD G", "old run state archived");
    require_file(&fs, PAIR_BACKUP_P, "NEW P", "pair source retained");
    require_file(&fs, PAIR_BACKUP_G, "NEW G", "pair companion retained");
    require_no_inflight(&fs, "pair restore leaves no temporaries");
    local_backup(&backup, PAIR_ARCHIVE_P, 1);
    require(manager_storage_restore_backup(&io, &backup) == MANAGER_STORAGE_OK,
            "restore archived pair");
    require_live(&fs, "OLD P", "OLD G", "archived pair restored");
    require_file(&fs, MANAGER_STORAGE_BACKUP_ROOT "/slot1-0002.dat", "NEW P",
                 "second archive persistent");
    require_file(&fs, MANAGER_STORAGE_BACKUP_ROOT "/slot1-0002.gamestate.dat",
                 "NEW G", "second archive run state");

    /* A backup without run state leaves the live run state alone, whatever
     * the listing flag said. */
    setup_pair(&fs, "OLD P", "OLD G", 0);
    io = fake_io(&fs);
    local_backup(&backup, PAIR_BACKUP_P, 1);
    require(manager_storage_restore_backup(&io, &backup) == MANAGER_STORAGE_OK,
            "restore legacy local backup");
    require_live(&fs, "NEW P", "OLD G", "run state kept");
    require_file(&fs, PAIR_ARCHIVE_P, "OLD P", "legacy restore archives P");
    require_absent(&fs, PAIR_ARCHIVE_G, "legacy restore archives no G");
    require_no_inflight(&fs, "legacy restore leaves no temporaries");

    /* Pair into a slot that had no run state. */
    setup_pair(&fs, "OLD P", NULL, 1);
    io = fake_io(&fs);
    local_backup(&backup, PAIR_BACKUP_P, 1);
    require(manager_storage_restore_backup(&io, &backup) == MANAGER_STORAGE_OK,
            "restore pair into slot without run state");
    require_live(&fs, "NEW P", "NEW G", "run state created");
    require_file(&fs, PAIR_ARCHIVE_P, "OLD P", "old persistent archived alone");
    require_absent(&fs, PAIR_ARCHIVE_G, "no run state to archive");

    /* Pair into an empty slot: nothing to archive. */
    setup_pair(&fs, NULL, NULL, 1);
    io = fake_io(&fs);
    local_backup(&backup, PAIR_BACKUP_P, 1);
    require(manager_storage_restore_backup(&io, &backup) == MANAGER_STORAGE_OK,
            "restore pair into empty slot");
    require_live(&fs, "NEW P", "NEW G", "empty slot filled");
    require(fake_count_containing(&fs, "slot1-0001") == 0,
            "empty slot leaves no archive");
    require_no_inflight(&fs, "empty slot restore leaves no temporaries");

    /* The game's own dated backup restores exactly as before. */
    setup_restore(&fs, &backup);
    fake_add_file(&fs, PAIR_G1, "OLD G");
    io = fake_io(&fs);
    require(manager_storage_restore_backup(&io, &backup) == MANAGER_STORAGE_OK,
            "restore dated backup with run state present");
    require_live(&fs, "NEW SLOT", "OLD G", "dated restore keeps run state");
    require_file(&fs, PAIR_BACKUP_P, "OLD SLOT", "dated restore archives P");
    require_absent(&fs, PAIR_BACKUP_G, "dated restore archives no G");
    require_no_inflight(&fs, "dated restore leaves no temporaries");

    /* Listing to restore round trip. */
    setup_pair(&fs, "OLD P", "OLD G", 1);
    io = fake_io(&fs);
    require(manager_storage_list_save_backups(&io, backups, 4u, &count,
                                              &rejected) == MANAGER_STORAGE_OK &&
                count == 1u && backups[0].has_gamestate,
            "listed pair");
    require(manager_storage_restore_backup(&io, &backups[0]) ==
                MANAGER_STORAGE_OK,
            "restore listed pair");
    require_live(&fs, "NEW P", "NEW G", "listed pair restored");

    /* A companion that is not a file refuses before anything moves. */
    setup_pair(&fs, "OLD P", "OLD G", 0);
    fake_add_dir(&fs, PAIR_BACKUP_G);
    io = fake_io(&fs);
    local_backup(&backup, PAIR_BACKUP_P, 0);
    require(manager_storage_restore_backup(&io, &backup) ==
                MANAGER_STORAGE_CONFLICT,
            "companion directory is a conflict");
    require_live(&fs, "OLD P", "OLD G", "conflict leaves the slot");
}

struct pair_scenario {
    const char *name;
    const char *live_persistent;
    const char *live_gamestate;
    int backup_has_gamestate;
    int dated;
};

static void run_restore_fault_matrix(const struct pair_scenario *scenario)
{
    enum fake_operation operation;
    for (operation = FAKE_OP_STAT; operation < FAKE_OP_COUNT; ++operation) {
        int occurrence;
        for (occurrence = 1;; ++occurrence) {
            struct fake_fs fs;
            struct manager_storage_io io;
            struct manager_save_backup backup;
            const char *expected_persistent;
            const char *expected_gamestate;
            int result;
            int total;
            if (scenario->dated) {
                setup_restore(&fs, &backup);
                if (scenario->live_gamestate != NULL)
                    fake_add_file(&fs, PAIR_G1, scenario->live_gamestate);
            } else {
                setup_pair(&fs, scenario->live_persistent,
                           scenario->live_gamestate,
                           scenario->backup_has_gamestate);
                local_backup(&backup, PAIR_BACKUP_P,
                             scenario->backup_has_gamestate);
            }
            io = fake_io(&fs);
            fake_fail(&fs, (enum fake_operation)operation, occurrence);
            result = manager_storage_restore_backup(&io, &backup);
            total = fs.operation_counts[operation];
            fake_no_fail(&fs);
            if (total < occurrence) {
                require(result >= 0, "clean restore succeeds");
                break;
            }
            if (scenario->dated) {
                require_file(&fs, PAIR_DATED, "NEW SLOT", scenario->name);
            } else {
                require_file(&fs, PAIR_BACKUP_P, "NEW P", scenario->name);
                if (scenario->backup_has_gamestate)
                    require_file(&fs, PAIR_BACKUP_G, "NEW G", scenario->name);
            }
            /* The next manager start repairs whatever the fault left. */
            require(manager_storage_recover(&io) == MANAGER_STORAGE_OK,
                    scenario->name);
            if (result >= 0) {
                expected_persistent = scenario->dated ? "NEW SLOT" : "NEW P";
                expected_gamestate = scenario->backup_has_gamestate
                                         ? "NEW G"
                                         : scenario->live_gamestate;
            } else {
                expected_persistent = scenario->live_persistent;
                expected_gamestate = scenario->live_gamestate;
                require(fake_count_containing(&fs, "slot1-0001") == 0,
                        scenario->name);
            }
            require_live(&fs, expected_persistent, expected_gamestate,
                         scenario->name);
            require_no_inflight(&fs, scenario->name);
            require(manager_storage_recover(&io) == MANAGER_STORAGE_OK,
                    scenario->name);
            require_live(&fs, expected_persistent, expected_gamestate,
                         scenario->name);
        }
        require(occurrence > 1 || operation == FAKE_OP_MKDIR ||
                    operation >= FAKE_OP_OPEN_DIR,
                "every file operation of the restore path is exercised");
    }
}

static void test_pair_restore_faults(void)
{
    static const struct pair_scenario scenarios[] = {
        {"pair into full slot", "OLD P", "OLD G", 1, 0},
        {"pair into slot without run state", "OLD P", NULL, 1, 0},
        {"pair into empty slot", NULL, NULL, 1, 0},
        {"pair into run state only", NULL, "OLD G", 1, 0},
        {"single into full slot", "OLD P", "OLD G", 0, 0},
        {"dated into full slot", "OLD SLOT", "OLD G", 0, 1},
    };
    size_t index;
    for (index = 0u; index < sizeof(scenarios) / sizeof(scenarios[0]); ++index)
        run_restore_fault_matrix(&scenarios[index]);
}

static void pair_recovery_base(struct fake_fs *fs, const char *marker)
{
    fake_init(fs);
    fake_add_dir(fs, MANAGER_STORAGE_BACKUP_ROOT);
    if (marker != NULL)
        fake_add_file(fs, PAIR_MARKER, marker);
}

static void require_recovered(struct fake_fs *fs, struct manager_storage_io *io,
                              const char *persistent, const char *gamestate,
                              const char *message)
{
    require(manager_storage_recover(io) == MANAGER_STORAGE_OK, message);
    require_live(fs, persistent, gamestate, message);
    require_no_inflight(fs, message);
    require(manager_storage_recover(io) == MANAGER_STORAGE_OK, message);
    require_live(fs, persistent, gamestate, message);
}

static void test_pair_recovery(void)
{
    struct fake_fs fs;
    struct manager_storage_io io;

    /* Crash after both asides: rollbacks come back, staging is dropped. */
    pair_recovery_base(&fs, "PG0000");
    fake_add_file(&fs, PAIR_ROLLBACK_P, "OLD P");
    fake_add_file(&fs, PAIR_ROLLBACK_G, "OLD G");
    fake_add_file(&fs, PAIR_TEMP_P, "STAGED");
    fake_add_file(&fs, PAIR_TEMP_G, "STAGED");
    io = fake_io(&fs);
    require_recovered(&fs, &io, "OLD P", "OLD G", "recover after asides");
    require(fake_count_containing(&fs, "recovered-") == 0,
            "nothing to quarantine after asides");

    /* Crash after promotion: the unverified pair is quarantined. */
    pair_recovery_base(&fs, "PG0000");
    fake_add_file(&fs, PAIR_ROLLBACK_P, "OLD P");
    fake_add_file(&fs, PAIR_ROLLBACK_G, "OLD G");
    fake_add_file(&fs, PAIR_P1, "NEW P");
    fake_add_file(&fs, PAIR_G1, "NEW G");
    io = fake_io(&fs);
    require_recovered(&fs, &io, "OLD P", "OLD G", "recover after promotion");
    require_file(&fs, PAIR_QUARANTINE_P, "NEW P", "promoted P quarantined");
    require_file(&fs, PAIR_QUARANTINE_G, "NEW G", "promoted G quarantined");

    /* Crash between the two archive renames: the run state is already under
     * its archive name and comes back from there. */
    pair_recovery_base(&fs, "PG0000");
    fake_add_file(&fs, PAIR_ROLLBACK_P, "OLD P");
    fake_add_file(&fs, PAIR_BACKUP_G, "OLD G");
    fake_add_file(&fs, PAIR_P1, "NEW P");
    fake_add_file(&fs, PAIR_G1, "NEW G");
    io = fake_io(&fs);
    require_recovered(&fs, &io, "OLD P", "OLD G", "recover between archives");
    require_absent(&fs, PAIR_BACKUP_G, "archived run state moved back");
    require_file(&fs, PAIR_QUARANTINE_P, "NEW P", "P quarantined");
    require_file(&fs, PAIR_QUARANTINE_G, "NEW G", "G quarantined");

    /* Crash after both archives but before the commit: both come back. */
    pair_recovery_base(&fs, "PG0000");
    fake_add_file(&fs, PAIR_BACKUP_P, "OLD P");
    fake_add_file(&fs, PAIR_BACKUP_G, "OLD G");
    fake_add_file(&fs, PAIR_P1, "NEW P");
    fake_add_file(&fs, PAIR_G1, "NEW G");
    io = fake_io(&fs);
    require_recovered(&fs, &io, "OLD P", "OLD G", "recover before commit");
    require_absent(&fs, PAIR_BACKUP_P, "archived P moved back");
    require_absent(&fs, PAIR_BACKUP_G, "archived G moved back");

    /* Empty slot: whatever is live was promoted and is set aside. */
    pair_recovery_base(&fs, "pg0000");
    fake_add_file(&fs, PAIR_P1, "NEW P");
    fake_add_file(&fs, PAIR_G1, "NEW G");
    io = fake_io(&fs);
    require_recovered(&fs, &io, NULL, NULL, "recover empty slot");
    require_file(&fs, PAIR_QUARANTINE_P, "NEW P", "empty slot P quarantined");
    require_file(&fs, PAIR_QUARANTINE_G, "NEW G", "empty slot G quarantined");

    /* Run state not part of the restore: never touched. */
    pair_recovery_base(&fs, "P-0000");
    fake_add_file(&fs, PAIR_ROLLBACK_P, "OLD P");
    fake_add_file(&fs, PAIR_P1, "NEW P");
    fake_add_file(&fs, PAIR_G1, "OLD G");
    io = fake_io(&fs);
    require_recovered(&fs, &io, "OLD P", "OLD G", "recover single restore");
    require_file(&fs, PAIR_QUARANTINE_P, "NEW P", "single P quarantined");
    require_absent(&fs, PAIR_QUARANTINE_G, "untouched G not quarantined");

    /* Slot had no run state before: the promoted one is set aside. */
    pair_recovery_base(&fs, "Pg0000");
    fake_add_file(&fs, PAIR_ROLLBACK_P, "OLD P");
    fake_add_file(&fs, PAIR_P1, "NEW P");
    fake_add_file(&fs, PAIR_G1, "NEW G");
    io = fake_io(&fs);
    require_recovered(&fs, &io, "OLD P", NULL, "recover new run state");
    require_file(&fs, PAIR_QUARANTINE_G, "NEW G", "new G quarantined");

    /* Crash before the marker was synced: nothing has moved yet. */
    pair_recovery_base(&fs, "");
    fake_add_file(&fs, PAIR_P1, "OLD P");
    fake_add_file(&fs, PAIR_G1, "OLD G");
    fake_add_file(&fs, PAIR_TEMP_P, "STAGED");
    fake_add_file(&fs, PAIR_TEMP_G, "STAGED");
    io = fake_io(&fs);
    require_recovered(&fs, &io, "OLD P", "OLD G", "recover empty marker");
    require(fake_count_containing(&fs, "recovered-") == 0,
            "empty marker quarantines nothing");

    /* Marker synced, crash before the first aside. */
    pair_recovery_base(&fs, "PG0000");
    fake_add_file(&fs, PAIR_P1, "OLD P");
    fake_add_file(&fs, PAIR_G1, "OLD G");
    fake_add_file(&fs, PAIR_TEMP_P, "STAGED");
    fake_add_file(&fs, PAIR_TEMP_G, "STAGED");
    io = fake_io(&fs);
    require_recovered(&fs, &io, "OLD P", "OLD G", "recover before asides");
    require(fake_count_containing(&fs, "recovered-") == 0,
            "pre-aside state quarantines nothing");

    /* Malformed marker with a rollback: the rollback still wins. */
    pair_recovery_base(&fs, "XY12");
    fake_add_file(&fs, PAIR_ROLLBACK_G, "OLD G");
    fake_add_file(&fs, PAIR_P1, "OLD P");
    fake_add_file(&fs, PAIR_G1, "NEW G");
    io = fake_io(&fs);
    require_recovered(&fs, &io, "OLD P", "OLD G", "recover malformed marker");
    require_file(&fs, PAIR_QUARANTINE_G, "NEW G", "malformed marker G aside");

    /* Stray rollback without a marker: treated like the legacy rollback. */
    pair_recovery_base(&fs, NULL);
    fake_add_file(&fs, PAIR_ROLLBACK_P, "OLD P");
    fake_add_file(&fs, PAIR_P1, "NEW P");
    io = fake_io(&fs);
    require_recovered(&fs, &io, "OLD P", NULL, "recover stray rollback");
    require_file(&fs, PAIR_QUARANTINE_P, "NEW P", "stray rollback quarantine");

    /* Stale backup staging of the run state is removed. */
    pair_recovery_base(&fs, NULL);
    fake_add_file(&fs, MANAGER_STORAGE_BACKUP_ROOT
                  "/.manager-backup-slot1.gamestate.tmp",
                  "STAGED");
    fake_add_file(&fs, PAIR_P1, "OLD P");
    io = fake_io(&fs);
    require_recovered(&fs, &io, "OLD P", NULL, "recover backup staging");

    /* The legacy single-file rollback is still honoured next to a run state. */
    pair_recovery_base(&fs, NULL);
    fake_add_file(&fs, MANAGER_STORAGE_SAVE_ROOT "/.manager-rollback-slot1.dat",
                  "KNOWN OLD");
    fake_add_file(&fs, PAIR_P1, "UNVERIFIED NEW");
    fake_add_file(&fs, PAIR_G1, "OLD G");
    io = fake_io(&fs);
    require_recovered(&fs, &io, "KNOWN OLD", "OLD G", "legacy rollback");
    require_file(&fs, PAIR_QUARANTINE_P, "UNVERIFIED NEW",
                 "legacy quarantine name unchanged");
}
#endif

int main(void)
{
    prove_sha256();
    test_mods();
    test_save_listing();
    test_backup_and_restore();
    test_restore_faults();
    test_recovery_and_rejection();
#if defined(ISAAC_VITA_MANAGER_SAVE_GAMESTATE)
    test_pair_slots_and_backup();
    test_pair_backup_faults();
    test_pair_restore();
    test_pair_restore_faults();
    test_pair_recovery();
    printf("manager storage host tests (slot pair): PASS (%u checks)\n",
           checks);
#else
    printf("manager storage host tests: PASS (%u checks)\n", checks);
#endif
    return 0;
}
