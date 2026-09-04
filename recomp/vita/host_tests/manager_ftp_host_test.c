#include "manager_ftp_lifecycle.h"
#include "manager_ftp_path.h"
#include "manager_ftp_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int checks;

static void require(int condition, const char *message)
{
    ++checks;
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static void expect_path(const char *cwd, const char *input,
                        const char *expected_virtual,
                        const char *expected_vita)
{
    char virtual_path[MANAGER_FTP_PATH_CAPACITY];
    char vita_path[MANAGER_FTP_PATH_CAPACITY];
    int result = manager_ftp_path_resolve(cwd, input,
                                          virtual_path, sizeof(virtual_path),
                                          vita_path, sizeof(vita_path));
    require(result == MANAGER_FTP_PATH_OK, input);
    require(strcmp(virtual_path, expected_virtual) == 0, "virtual path");
    require(strcmp(vita_path, expected_vita) == 0, "Vita path");
}

static void reject_path(const char *cwd, const char *input)
{
    char virtual_path[MANAGER_FTP_PATH_CAPACITY];
    char vita_path[MANAGER_FTP_PATH_CAPACITY];
    require(manager_ftp_path_resolve(cwd, input,
                                     virtual_path, sizeof(virtual_path),
                                     vita_path, sizeof(vita_path)) < 0,
            input);
}

static void test_paths(void)
{
    char virtual_path[MANAGER_FTP_PATH_CAPACITY];
    char vita_path[MANAGER_FTP_PATH_CAPACITY];
    char long_path[MANAGER_FTP_PATH_CAPACITY + 32u];

    expect_path("/", "/", "/", MANAGER_FTP_VITA_ROOT);
    expect_path("/", MANAGER_FTP_LEGACY_ROOT, "/", MANAGER_FTP_VITA_ROOT);
    expect_path("/mods", MANAGER_FTP_LEGACY_ROOT "/", "/",
                MANAGER_FTP_VITA_ROOT);
    expect_path("/", MANAGER_FTP_VITA_ROOT "/mods/42/main.lua",
                "/mods/42/main.lua",
                MANAGER_FTP_VITA_ROOT "/mods/42/main.lua");
    expect_path("/", MANAGER_FTP_LEGACY_ROOT "/mods/42/main.lua",
                "/mods/42/main.lua",
                MANAGER_FTP_VITA_ROOT "/mods/42/main.lua");
    expect_path("/mods", "42//main.lua/", "/mods/42/main.lua",
                MANAGER_FTP_VITA_ROOT "/mods/42/main.lua");
    expect_path("/mods", "/Documents/My Games/save.dat",
                "/Documents/My Games/save.dat",
                MANAGER_FTP_VITA_ROOT "/Documents/My Games/save.dat");

    reject_path("/", "/ur0:/shell/db/app.db");
    reject_path("/", "/app0:/eboot.bin");
    reject_path("/", "/ux0:/data/other/file");
    reject_path("/", "/ux0:/data/isaacr001-other/file");
    reject_path("/", MANAGER_FTP_LEGACY_ROOT "/../app/ISAACR001/eboot.bin");
    reject_path("/mods", "../eboot.bin");
    reject_path("/mods", "./main.lua");
    reject_path("/", "folder\\file");
    reject_path("/", "bad\nname");
    reject_path("relative", "file");

    memset(long_path, 'a', sizeof(long_path));
    long_path[0] = '/';
    long_path[sizeof(long_path) - 1u] = '\0';
    reject_path("/", long_path);

    require(manager_ftp_path_parent("/mods/42", virtual_path,
                                    sizeof(virtual_path), vita_path,
                                    sizeof(vita_path)) == MANAGER_FTP_PATH_OK,
            "parent child");
    require(strcmp(virtual_path, "/mods") == 0, "parent virtual");
    require(strcmp(vita_path, MANAGER_FTP_VITA_ROOT "/mods") == 0,
            "parent Vita");
    require(manager_ftp_path_parent("/", virtual_path, sizeof(virtual_path),
                                    vita_path, sizeof(vita_path)) ==
                MANAGER_FTP_PATH_OK,
            "parent root");
    require(manager_ftp_path_is_root(virtual_path), "root remains mapped");
    require(strcmp(vita_path, MANAGER_FTP_VITA_ROOT) == 0,
            "root Vita remains mapped");
}

static void expect_command(const char *line, enum manager_ftp_command command,
                           int has_argument, const char *argument,
                           enum manager_ftp_path_requirement requirement)
{
    struct manager_ftp_parsed_command parsed;
    require(manager_ftp_parse_command(line, strlen(line), &parsed) == 0, line);
    require(parsed.command == command, "command enum");
    require(parsed.has_argument == has_argument, "argument presence");
    require(strcmp(parsed.argument, argument) == 0, "argument value");
    require(manager_ftp_command_path_requirement(parsed.command) == requirement,
            "path requirement");
}

static void test_protocol(void)
{
    struct manager_ftp_parsed_command parsed;
    static const enum manager_ftp_command required[] = {
        MANAGER_FTP_COMMAND_CWD,  MANAGER_FTP_COMMAND_RETR,
        MANAGER_FTP_COMMAND_STOR, MANAGER_FTP_COMMAND_DELE,
        MANAGER_FTP_COMMAND_RMD,  MANAGER_FTP_COMMAND_MKD,
        MANAGER_FTP_COMMAND_RNFR, MANAGER_FTP_COMMAND_RNTO,
        MANAGER_FTP_COMMAND_SIZE,
    };
    static const enum manager_ftp_command optional[] = {
        MANAGER_FTP_COMMAND_LIST,
        MANAGER_FTP_COMMAND_NLST,
        MANAGER_FTP_COMMAND_MLSD,
    };
    size_t index;

    expect_command("USER anonymous\r\n", MANAGER_FTP_COMMAND_USER, 1,
                   "anonymous", MANAGER_FTP_PATH_NONE);
    expect_command("type i\n", MANAGER_FTP_COMMAND_TYPE, 1, "i",
                   MANAGER_FTP_PATH_NONE);
    expect_command("MLSD\r\n", MANAGER_FTP_COMMAND_MLSD, 0, "",
                   MANAGER_FTP_PATH_OPTIONAL);
    expect_command("STOR /ux0:/data/isaacr001/mods/42/main.lua\r\n",
                   MANAGER_FTP_COMMAND_STOR, 1,
                   "/ux0:/data/isaacr001/mods/42/main.lua",
                   MANAGER_FTP_PATH_REQUIRED);
    expect_command("RNTO /mods/42/main.lua\r\n", MANAGER_FTP_COMMAND_RNTO, 1,
                   "/mods/42/main.lua", MANAGER_FTP_PATH_REQUIRED);
    expect_command("FROB value\r\n", MANAGER_FTP_COMMAND_UNKNOWN, 1, "value",
                   MANAGER_FTP_PATH_NONE);
    require(manager_ftp_parse_command("", 0u, &parsed) < 0, "empty command");
    require(manager_ftp_parse_command("TOOLONGER x\r\n", 13u, &parsed) < 0,
            "overlong verb");
    require(manager_ftp_parse_command("STOR bad\nname\r\n", 15u, &parsed) < 0,
            "control in argument");
    for (index = 0u; index < sizeof(required) / sizeof(required[0]); ++index) {
        require(manager_ftp_command_path_requirement(required[index]) ==
                    MANAGER_FTP_PATH_REQUIRED,
                "all mutating/read commands require a canonical path");
    }
    for (index = 0u; index < sizeof(optional) / sizeof(optional[0]); ++index) {
        require(manager_ftp_command_path_requirement(optional[index]) ==
                    MANAGER_FTP_PATH_OPTIONAL,
                "all listing commands canonicalize an optional path");
    }
}

static void require_snapshot_empty(
    const struct manager_ftp_stop_snapshot *snapshot,
    const char *message)
{
    int slot;

    for (slot = 0; slot < MANAGER_FTP_SOCKET_COUNT; ++slot)
        require(snapshot->sockets[slot] < 0, message);
}

static void test_hostile_stop_publication_orders(void)
{
    struct manager_ftp_lifecycle lifecycle;
    struct manager_ftp_stop_snapshot snapshot;
    int slot;

    /* A mutex makes these the only two possible linear orders. Exercise both
     * for every descriptor that can otherwise block worker shutdown. */
    for (slot = 0; slot < MANAGER_FTP_SOCKET_COUNT; ++slot) {
        const int descriptor = 100 + slot;
        int owned_descriptor;

        manager_ftp_lifecycle_init(&lifecycle);
        manager_ftp_lifecycle_set_worker_running(&lifecycle, 1);
        require(manager_ftp_lifecycle_publish(
                    &lifecycle, (enum manager_ftp_socket_slot)slot,
                    descriptor) == 0,
                "publish wins before hostile stop");
        manager_ftp_lifecycle_begin_stop(&lifecycle, &snapshot);
        require(manager_ftp_lifecycle_should_stop(&lifecycle),
                "stop is visible after published descriptor snapshot");
        require(snapshot.sockets[slot] == descriptor,
                "stop snapshots every previously published descriptor");
        require(manager_ftp_lifecycle_load_if_running(
                    &lifecycle, (enum manager_ftp_socket_slot)slot) < 0,
                "no blocking operation starts after stop");
        require(manager_ftp_lifecycle_publish(
                    &lifecycle, (enum manager_ftp_socket_slot)slot,
                    descriptor + 1000) < 0,
                "publication after stop is rejected");

        owned_descriptor = manager_ftp_lifecycle_take(
            &lifecycle, (enum manager_ftp_socket_slot)slot);
        if (slot == MANAGER_FTP_SOCKET_LISTEN ||
            slot == MANAGER_FTP_SOCKET_PASSIVE) {
            require(owned_descriptor < 0,
                    "stop transfers accept socket ownership to stopping thread");
        } else {
            require(owned_descriptor == descriptor,
                    "worker retains client descriptor close ownership");
            require(manager_ftp_lifecycle_take(
                        &lifecycle, (enum manager_ftp_socket_slot)slot) < 0,
                    "client descriptor can be taken only once");
        }

        manager_ftp_lifecycle_init(&lifecycle);
        manager_ftp_lifecycle_set_worker_running(&lifecycle, 1);
        manager_ftp_lifecycle_begin_stop(&lifecycle, &snapshot);
        require_snapshot_empty(&snapshot,
                               "hostile stop before publish sees no descriptor");
        require(manager_ftp_lifecycle_publish(
                    &lifecycle, (enum manager_ftp_socket_slot)slot,
                    descriptor) < 0,
                "hostile publish after stop loses atomically");
        require(manager_ftp_lifecycle_load(
                    &lifecycle, (enum manager_ftp_socket_slot)slot) < 0,
                "rejected descriptor is never made globally visible");
    }
}

static void test_lifecycle_status_and_duplicate_publication(void)
{
    struct manager_ftp_lifecycle lifecycle;
    struct manager_ftp_stop_snapshot snapshot;

    manager_ftp_lifecycle_init(&lifecycle);
    require(!manager_ftp_lifecycle_is_listening(&lifecycle),
            "fresh lifecycle is stopped");
    manager_ftp_lifecycle_set_worker_running(&lifecycle, 1);
    require(manager_ftp_lifecycle_publish(&lifecycle,
                                          MANAGER_FTP_SOCKET_LISTEN, 41) == 0,
            "publish listener");
    require(manager_ftp_lifecycle_is_listening(&lifecycle),
            "listener and worker report ready");
    require(manager_ftp_lifecycle_publish(&lifecycle,
                                          MANAGER_FTP_SOCKET_LISTEN, 42) < 0,
            "duplicate publication is rejected");
    require(manager_ftp_lifecycle_take(&lifecycle,
                                       MANAGER_FTP_SOCKET_LISTEN) == 41,
            "duplicate publication cannot overwrite descriptor ownership");
    require(!manager_ftp_lifecycle_is_listening(&lifecycle),
            "taken listener reports stopped");

    manager_ftp_lifecycle_begin_stop(&lifecycle, &snapshot);
    require_snapshot_empty(&snapshot, "stop after listener take is empty");
    manager_ftp_lifecycle_set_worker_running(&lifecycle, 0);
    require(!manager_ftp_lifecycle_is_listening(&lifecycle),
            "completed worker reports stopped");
}

int main(void)
{
    test_paths();
    test_protocol();
    test_hostile_stop_publication_orders();
    test_lifecycle_status_and_duplicate_publication();
    printf("manager FTP host tests: PASS; checks=%u\n", checks);
    return 0;
}
