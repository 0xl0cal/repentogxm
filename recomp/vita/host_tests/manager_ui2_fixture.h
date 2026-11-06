/*
 * Manager UI2 host-test fixture: a populated ux0:data/isaacr001 on the fake
 * FS (fake_fs.h, included first), a fake FTP host, and an io wrapper that
 * reports the INSTALL.md sizes for the two resource archives (and, for the
 * render test, realistic save sizes) without storing that many bytes.
 *
 * The mod names and versions are the made-up rows of the design mock-up
 * of the Russian MODS page; nothing here comes from the game.
 */
#ifndef ISAAC_MANAGER_UI2_FIXTURE_H
#define ISAAC_MANAGER_UI2_FIXTURE_H

#include "manager_ui.h"

#include <stdio.h>
#include <string.h>

#define UI2_LOG_PATH MANAGER_STORAGE_ROOT "/first-arm-fault.log"
#define UI2_BUILD_LINE                                                         \
    "[0.123 thr 0x40010003] build 2026-09-27-k2a; guest base=0x00400000; "    \
    "softfp ARMv7\n"
#define UI2_RECEIPT_LINE                                                       \
    "[0.201 thr 0x40010003] ISAAC VITA SETTINGS: "                            \
    "path=ux0:/data/isaacr001/port.ini status=ok bytes=22 lines=2 "          \
    "rejected=0 unknown=0 dup=0 fps_lock=60 "                                  \
    "raster=960x544(ignored:not-compiled) remap=default "                     \
    "chords=rb/back restart_hold=1 stick_swap=0\n"

struct ui2_mod_fixture {
    const char *dir;
    const char *name; /* NULL: no metadata.xml */
    const char *version;
    int disabled;
};

static const struct ui2_mod_fixture ui2_mods[] = {
    {"836319872", "External Item Descriptions", "4.92", 0},
    {"2681875787", "Mod Config Menu Pure", "1.0", 0},
    {"2491006386", "Music Mod Callback", "2.4", 1},
    {"1200000001", "Описания предметов (русский)", NULL, 0},
    {"2010022451", "Planetarium chance", NULL, 0},
    {"1348031964", "Stage API", "2.0", 1},
    {"2376005362", "Encyclopedia", NULL, 0},
    {"2851063440", "Fiend Folio", NULL, 0},
    /* 108 characters: cut to 90 by manager_mods_meta, then by the row. */
    {"3001000001", "A very long mod name that cannot fit in the row at all "
                   "and is cut with an ellipsis before it reaches the end",
     "0.9.1", 0},
    {"vita-perf", NULL, NULL, 0},
};
#define UI2_MOD_COUNT (sizeof(ui2_mods) / sizeof(ui2_mods[0]))

static void ui2_populate(struct fake_fs *fs, int with_log)
{
    size_t index;
    char path[MANAGER_STORAGE_PATH_CAPACITY];
    char xml[512];
    fake_init(fs);
    for (index = 0u; index < UI2_MOD_COUNT; ++index) {
        const struct ui2_mod_fixture *mod = &ui2_mods[index];
        (void)snprintf(path, sizeof(path), "%s/%s", MANAGER_STORAGE_MOD_ROOT,
                       mod->dir);
        fake_add_dir(fs, path);
        if (mod->name != NULL) {
            (void)snprintf(path, sizeof(path), "%s/%s/metadata.xml",
                           MANAGER_STORAGE_MOD_ROOT, mod->dir);
            if (mod->version != NULL)
                (void)snprintf(xml, sizeof(xml),
                               "<?xml version=\"1.0\"?>\n<metadata>\n"
                               "  <name>%s</name>\n  <id>%s</id>\n"
                               "  <version>%s</version>\n</metadata>\n",
                               mod->name, mod->dir, mod->version);
            else
                (void)snprintf(xml, sizeof(xml),
                               "<metadata><name>%s</name></metadata>",
                               mod->name);
            fake_add_file(fs, path, xml);
        }
        if (mod->disabled) {
            (void)snprintf(path, sizeof(path), "%s/%s/disable.it",
                           MANAGER_STORAGE_MOD_ROOT, mod->dir);
            fake_add_file(fs, path, "");
        }
    }
    fake_add_file(fs, MANAGER_STORAGE_SAVE_ROOT "/persistentgamedata1.dat",
                  "slot one");
    fake_add_file(fs, MANAGER_STORAGE_SAVE_ROOT "/persistentgamedata2.dat",
                  "slot two");
    fake_add_file(fs,
                  MANAGER_STORAGE_SAVE_ROOT "/20260821.persistentgamedata1.dat",
                  "dated one");
    fake_add_dir(fs, MANAGER_STORAGE_BACKUP_ROOT);
    fake_add_file(fs, MANAGER_STORAGE_BACKUP_ROOT "/slot1-0003.dat", "old one");
    fake_add_file(fs, MANAGER_STORAGE_BACKUP_ROOT "/slot1-0004.dat", "older");
    fake_add_file(fs, MANAGER_STORAGE_BACKUP_ROOT "/slot2-0007.dat", "two");
    fake_add_file(fs, MANAGER_STORAGE_PORT_INI_PATH, "schema=1\nfps_lock=60\n");
    if (with_log)
        fake_add_file(fs, UI2_LOG_PATH, UI2_BUILD_LINE UI2_RECEIPT_LINE);
    fake_add_dir(fs, MANAGER_STORAGE_ROOT "/resources");
    fake_add_dir(fs, MANAGER_STORAGE_ROOT "/resources/packed");
    fake_add_file(fs, MANAGER_STORAGE_ROOT "/resources/packed/animations.a",
                  "a");
    fake_add_file(fs, MANAGER_STORAGE_ROOT "/resources/packed/repentance.a",
                  "r");
}

/* ---- io wrapper: reported sizes -------------------------------------- */

struct ui2_size_override {
    const char *suffix;
    uint64_t size;
};

static const struct ui2_size_override ui2_resource_sizes[] = {
    {"/resources/packed/animations.a", MANAGER_UI_RESOURCE_ANIMATIONS_BYTES},
    {"/resources/packed/repentance.a", MANAGER_UI_RESOURCE_REPENTANCE_BYTES},
};
/* Render test only (no writes there): realistic save sizes on screen. */
static const struct ui2_size_override ui2_save_sizes[] = {
    {"/persistentgamedata1.dat", 1258291u},
    {"/persistentgamedata2.dat", 1101004u},
    {"/20260821.persistentgamedata1.dat", 1239000u},
    {"/slot1-0003.dat", 1247000u},
    {"/slot1-0004.dat", 1180000u},
    {"/slot2-0007.dat", 1099000u},
};

static int ui2_inflate_saves;

static uint64_t ui2_override(const char *path, uint64_t size)
{
    const size_t length = strlen(path);
    size_t index;
    for (index = 0u; index < sizeof(ui2_resource_sizes) /
                                 sizeof(ui2_resource_sizes[0]);
         ++index) {
        const size_t suffix = strlen(ui2_resource_sizes[index].suffix);
        if (length >= suffix &&
            strcmp(path + length - suffix, ui2_resource_sizes[index].suffix) == 0)
            return ui2_resource_sizes[index].size;
    }
    if (!ui2_inflate_saves)
        return size;
    for (index = 0u; index < sizeof(ui2_save_sizes) / sizeof(ui2_save_sizes[0]);
         ++index) {
        const size_t suffix = strlen(ui2_save_sizes[index].suffix);
        if (length >= suffix &&
            strcmp(path + length - suffix, ui2_save_sizes[index].suffix) == 0)
            return ui2_save_sizes[index].size;
    }
    return size;
}

static int ui2_stat(void *context, const char *path,
                    enum manager_storage_node_kind *kind, uint64_t *size)
{
    const int result = fake_stat(context, path, kind, size);
    if (result == 0)
        *size = ui2_override(path, *size);
    return result;
}

static int ui2_read_dir(void *context, int handle,
                        struct manager_storage_dir_entry *entry)
{
    const int result = fake_read_dir(context, handle, entry);
    if (result > 0) {
        char path[MANAGER_STORAGE_PATH_CAPACITY + MANAGER_STORAGE_NAME_CAPACITY];
        (void)snprintf(path, sizeof(path), "/%s", entry->name);
        entry->size = ui2_override(path, entry->size);
    }
    return result;
}

static struct manager_storage_io ui2_io(struct fake_fs *fs, int inflate_saves)
{
    struct manager_storage_io io = fake_io(fs);
    ui2_inflate_saves = inflate_saves;
    io.stat_path = ui2_stat;
    io.read_directory = ui2_read_dir;
    return io;
}

/* ---- fake FTP host ------------------------------------------------------ */

struct ui2_ftp {
    int start_result;
    int listening;
    unsigned int starts;
    unsigned int stops;
};

static int ui2_ftp_start(void *context, char *ip, size_t capacity,
                         unsigned short *port)
{
    struct ui2_ftp *ftp = context;
    ++ftp->starts;
    if (ftp->start_result < 0)
        return ftp->start_result;
    (void)snprintf(ip, capacity, "%s", "192.168.1.20");
    *port = 1337u;
    ftp->listening = 1;
    return 0;
}

static void ui2_ftp_stop(void *context)
{
    struct ui2_ftp *ftp = context;
    ++ftp->stops;
    ftp->listening = 0;
}

static int ui2_ftp_listening(void *context)
{
    const struct ui2_ftp *ftp = context;
    return ftp->listening;
}

static struct manager_ui_host ui2_host(struct ui2_ftp *ftp)
{
    struct manager_ui_host host;
    host.context = ftp;
    host.ftp_start = ui2_ftp_start;
    host.ftp_stop = ui2_ftp_stop;
    host.ftp_listening = ui2_ftp_listening;
    return host;
}

#endif
