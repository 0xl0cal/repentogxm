/*
 * Reduced and altered FTP server adaptation for the Isaac manager.
 *
 * Network/passive-transfer/lifecycle prior art:
 *   xerpi/libftpvita @ 77a1d39c1d6f11a55fe65a6ed9fe707bff5ede4b
 *   Copyright (c) 2015-2016 Sergi Granell (xerpi)
 *   Copyright (c) 2019 Sergi Granell
 *   MIT licence: third_party/libftpvita/LICENSE
 *   Exact hashes and project changes: third_party/libftpvita/PROVENANCE.md
 *
 * Unlike upstream libftpvita, this adaptation has no device list and no path
 * reaches sceIo* before manager_ftp_path_resolve has lexically mapped it below
 * the one fixed root ux0:/data/isaacr001. This is intentionally not advertised
 * as a filesystem sandbox: pre-existing native links or mount aliases below
 * that root are outside the manager's supported input contract. Active FTP
 * mode and custom commands are intentionally absent.
 */

#include "manager_ftp_server.h"

#include "manager_ftp_lifecycle.h"
#include "manager_ftp_path.h"
#include "manager_ftp_protocol.h"

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MANAGER_FTP_PORT 1337u
#define MANAGER_FTP_NET_MEMORY_SIZE (64u * 1024u)
#define MANAGER_FTP_FILE_BUFFER_SIZE (64u * 1024u)
#define MANAGER_FTP_CONTROL_BUFFER_SIZE 2048u
#define MANAGER_FTP_NETCTL_ALREADY_INITIALIZED ((int)0x80412102u)
#define MANAGER_FTP_ABORT_BOTH                                              \
    (SCE_NET_SOCKET_ABORT_FLAG_RCV_PRESERVATION |                          \
     SCE_NET_SOCKET_ABORT_FLAG_SND_PRESERVATION)

struct manager_ftp_server_state {
    int started;
    SceUID state_mutex;
    SceUID worker;
    struct manager_ftp_lifecycle lifecycle;
    int module_loaded;
    int net_owned;
    int netctl_owned;
    void *net_memory;
    SceNetInAddr address;
    char ip[16];
};

struct manager_ftp_session {
    SceNetSockaddrIn peer;
    char cwd[MANAGER_FTP_PATH_CAPACITY];
    char rename_path[MANAGER_FTP_PATH_CAPACITY];
    int rename_ready;
};

static struct manager_ftp_server_state manager_ftp_server = {
    .state_mutex = -1,
    .worker = -1,
};

static int manager_ftp_state_lock(void)
{
    if (manager_ftp_server.state_mutex < 0)
        return -1;
    return sceKernelLockMutex(manager_ftp_server.state_mutex, 1, NULL) < 0
               ? -1
               : 0;
}

static void manager_ftp_state_unlock(void)
{
    (void)sceKernelUnlockMutex(manager_ftp_server.state_mutex, 1);
}

static int manager_ftp_should_stop(void)
{
    int result;

    if (manager_ftp_state_lock() < 0)
        return 1;
    result = manager_ftp_lifecycle_should_stop(&manager_ftp_server.lifecycle);
    manager_ftp_state_unlock();
    return result;
}

static void manager_ftp_set_worker_running(int running)
{
    if (manager_ftp_state_lock() < 0)
        return;
    manager_ftp_lifecycle_set_worker_running(&manager_ftp_server.lifecycle,
                                             running);
    manager_ftp_state_unlock();
}

static int manager_ftp_publish_socket(enum manager_ftp_socket_slot slot,
                                      int socket)
{
    int result = -1;

    if (manager_ftp_state_lock() >= 0) {
        result = manager_ftp_lifecycle_publish(&manager_ftp_server.lifecycle,
                                               slot, socket);
        manager_ftp_state_unlock();
    }
    if (result < 0 && socket >= 0)
        (void)sceNetSocketClose(socket);
    return result;
}

static int manager_ftp_load_socket(enum manager_ftp_socket_slot slot,
                                   int require_running)
{
    int socket = -1;

    if (manager_ftp_state_lock() < 0)
        return -1;
    if (require_running) {
        socket = manager_ftp_lifecycle_load_if_running(
            &manager_ftp_server.lifecycle, slot);
    } else {
        socket = manager_ftp_lifecycle_load(&manager_ftp_server.lifecycle,
                                            slot);
    }
    manager_ftp_state_unlock();
    return socket;
}

static void manager_ftp_close_socket(enum manager_ftp_socket_slot slot)
{
    int socket;

    if (manager_ftp_state_lock() < 0)
        return;
    socket = manager_ftp_lifecycle_take(&manager_ftp_server.lifecycle, slot);
    manager_ftp_state_unlock();
    if (socket >= 0)
        (void)sceNetSocketClose(socket);
}

static void manager_ftp_shutdown_and_abort_socket(int socket)
{
    if (socket >= 0) {
        /* shutdown is persistent if stop wins just before a blocking call;
         * abort also wakes a call which is already sleeping. */
        (void)sceNetShutdown(socket, SCE_NET_SHUT_RDWR);
        (void)sceNetSocketAbort(socket, MANAGER_FTP_ABORT_BOTH);
    }
}

static int manager_ftp_send_all(int socket, const void *data, size_t size)
{
    const unsigned char *bytes = (const unsigned char *)data;
    size_t sent = 0u;

    while (sent < size) {
        const int result = sceNetSend(socket, bytes + sent,
                                      (unsigned int)(size - sent), 0);
        if (result <= 0)
            return -1;
        sent += (size_t)result;
    }
    return 0;
}

static int manager_ftp_reply(const char *format, ...)
{
    char message[512];
    va_list arguments;
    int socket;
    int length;

    va_start(arguments, format);
    length = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    socket = manager_ftp_load_socket(MANAGER_FTP_SOCKET_CONTROL, 1);
    if (length < 0 || (size_t)length >= sizeof(message) || socket < 0)
        return -1;
    return manager_ftp_send_all(socket, message, (size_t)length);
}

static void manager_ftp_close_data(void)
{
    manager_ftp_close_socket(MANAGER_FTP_SOCKET_DATA);
    manager_ftp_close_socket(MANAGER_FTP_SOCKET_PASSIVE);
}

static int manager_ftp_open_passive(struct manager_ftp_session *session)
{
    SceNetSockaddrIn address;
    unsigned int address_size;
    int reuse = 1;
    int socket;

    (void)session;
    manager_ftp_close_data();
    socket = sceNetSocket("isaac_manager_ftp_pasv", SCE_NET_AF_INET,
                          SCE_NET_SOCK_STREAM, 0);
    if (socket < 0)
        return manager_ftp_reply("425 Cannot open passive socket.\r\n");
    /* Publication and stop_begin are serialized by state_mutex. If stop won,
     * publish rejects and closes this still-local descriptor. If publish won,
     * stop's snapshot contains it and aborts the blocking edge before join. */
    if (manager_ftp_publish_socket(MANAGER_FTP_SOCKET_PASSIVE, socket) < 0)
        return -1;
    (void)sceNetSetsockopt(socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR,
                           &reuse, sizeof(reuse));

    memset(&address, 0, sizeof(address));
    address.sin_family = SCE_NET_AF_INET;
    address.sin_addr.s_addr = sceNetHtonl(SCE_NET_INADDR_ANY);
    address.sin_port = sceNetHtons(0u);
    if (sceNetBind(socket, (const SceNetSockaddr *)&address,
                   sizeof(address)) < 0 ||
        sceNetListen(socket, 1) < 0) {
        manager_ftp_close_data();
        return manager_ftp_reply("425 Cannot listen in passive mode.\r\n");
    }

    address_size = sizeof(address);
    if (sceNetGetsockname(socket, (SceNetSockaddr *)&address,
                          &address_size) < 0) {
        manager_ftp_close_data();
        return manager_ftp_reply("425 Cannot read passive endpoint.\r\n");
    }
    return manager_ftp_reply(
        "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u).\r\n",
        (unsigned int)((manager_ftp_server.address.s_addr >> 0) & 0xffu),
        (unsigned int)((manager_ftp_server.address.s_addr >> 8) & 0xffu),
        (unsigned int)((manager_ftp_server.address.s_addr >> 16) & 0xffu),
        (unsigned int)((manager_ftp_server.address.s_addr >> 24) & 0xffu),
        (unsigned int)((address.sin_port >> 0) & 0xffu),
        (unsigned int)((address.sin_port >> 8) & 0xffu));
}

static int manager_ftp_accept_data(struct manager_ftp_session *session)
{
    SceNetSockaddrIn peer;
    unsigned int peer_size = sizeof(peer);
    int passive_socket;
    int socket;

    passive_socket = manager_ftp_load_socket(MANAGER_FTP_SOCKET_PASSIVE, 1);
    if (passive_socket < 0) {
        (void)manager_ftp_reply("425 Use PASV first.\r\n");
        return -1;
    }
    memset(&peer, 0, sizeof(peer));
    socket = sceNetAccept(passive_socket,
                          (SceNetSockaddr *)&peer, &peer_size);
    manager_ftp_close_socket(MANAGER_FTP_SOCKET_PASSIVE);
    if (socket < 0) {
        (void)manager_ftp_reply("425 Cannot open data connection.\r\n");
        return -1;
    }
    if (manager_ftp_publish_socket(MANAGER_FTP_SOCKET_DATA, socket) < 0)
        return -1;
    if (peer.sin_addr.s_addr != session->peer.sin_addr.s_addr) {
        manager_ftp_close_socket(MANAGER_FTP_SOCKET_DATA);
        (void)manager_ftp_reply("425 Data peer must match control peer.\r\n");
        return -1;
    }
    return 0;
}

static int manager_ftp_safe_listing_name(const char *name)
{
    const unsigned char *cursor = (const unsigned char *)name;
    if (name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;
    while (*cursor != '\0') {
        if (*cursor < 0x20u || *cursor == 0x7fu || *cursor == '/' ||
            *cursor == '\\')
            return 0;
        ++cursor;
    }
    return 1;
}

static int manager_ftp_list_directory(struct manager_ftp_session *session,
                                      enum manager_ftp_command command,
                                      const char *vita_path)
{
    SceIoDirent entry;
    SceUID directory;
    char line[768];
    int data_socket;
    int read_result = 0;

    directory = sceIoDopen(vita_path);
    if (directory < 0) {
        (void)manager_ftp_reply("550 Invalid directory.\r\n");
        return -1;
    }
    if (manager_ftp_accept_data(session) < 0) {
        (void)sceIoDclose(directory);
        return -1;
    }
    if (manager_ftp_reply("150 Opening data connection.\r\n") < 0) {
        (void)sceIoDclose(directory);
        manager_ftp_close_data();
        return -1;
    }
    data_socket = manager_ftp_load_socket(MANAGER_FTP_SOCKET_DATA, 1);
    if (data_socket < 0) {
        (void)sceIoDclose(directory);
        manager_ftp_close_data();
        return -1;
    }

    for (;;) {
        int length;
        memset(&entry, 0, sizeof(entry));
        read_result = sceIoDread(directory, &entry);
        if (read_result <= 0)
            break;
        if (!manager_ftp_safe_listing_name(entry.d_name))
            continue;
        if (command == MANAGER_FTP_COMMAND_MLSD) {
            if (SCE_S_ISDIR(entry.d_stat.st_mode)) {
                length = snprintf(line, sizeof(line), "type=dir; %s\r\n",
                                  entry.d_name);
            } else {
                length = snprintf(line, sizeof(line),
                                  "type=file;size=%llu; %s\r\n",
                                  (unsigned long long)entry.d_stat.st_size,
                                  entry.d_name);
            }
        } else if (command == MANAGER_FTP_COMMAND_NLST) {
            length = snprintf(line, sizeof(line), "%s\r\n", entry.d_name);
        } else {
            length = snprintf(
                line, sizeof(line), "%c%s 1 vita vita %llu Jan 01 2020 %s\r\n",
                SCE_S_ISDIR(entry.d_stat.st_mode) ? 'd' : '-',
                SCE_S_ISDIR(entry.d_stat.st_mode) ? "rwxr-xr-x" : "rw-r--r--",
                (unsigned long long)entry.d_stat.st_size, entry.d_name);
        }
        if (length < 0 || (size_t)length >= sizeof(line) ||
            manager_ftp_send_all(data_socket, line,
                                 (size_t)length) < 0) {
            read_result = -1;
            break;
        }
    }
    (void)sceIoDclose(directory);
    manager_ftp_close_data();
    if (read_result < 0) {
        (void)manager_ftp_reply("426 Data transfer aborted.\r\n");
        return -1;
    }
    return manager_ftp_reply("226 Transfer complete.\r\n");
}

static int manager_ftp_retrieve(struct manager_ftp_session *session,
                                const char *vita_path)
{
    unsigned char *buffer;
    SceUID file;
    int data_socket;
    int read_result = 0;

    file = sceIoOpen(vita_path, SCE_O_RDONLY, 0);
    if (file < 0) {
        (void)manager_ftp_reply("550 File unavailable.\r\n");
        return -1;
    }
    buffer = (unsigned char *)malloc(MANAGER_FTP_FILE_BUFFER_SIZE);
    if (buffer == NULL) {
        (void)sceIoClose(file);
        (void)manager_ftp_reply("451 Out of memory.\r\n");
        return -1;
    }
    if (manager_ftp_accept_data(session) < 0) {
        free(buffer);
        (void)sceIoClose(file);
        return -1;
    }
    if (manager_ftp_reply("150 Opening binary data connection.\r\n") < 0) {
        manager_ftp_close_data();
        free(buffer);
        (void)sceIoClose(file);
        return -1;
    }
    data_socket = manager_ftp_load_socket(MANAGER_FTP_SOCKET_DATA, 1);
    if (data_socket < 0) {
        manager_ftp_close_data();
        free(buffer);
        (void)sceIoClose(file);
        return -1;
    }

    while ((read_result = sceIoRead(file, buffer,
                                    MANAGER_FTP_FILE_BUFFER_SIZE)) > 0) {
        if (manager_ftp_send_all(data_socket, buffer,
                                 (size_t)read_result) < 0) {
            read_result = -1;
            break;
        }
    }
    (void)sceIoClose(file);
    free(buffer);
    manager_ftp_close_data();
    if (read_result < 0) {
        (void)manager_ftp_reply("426 Data transfer aborted.\r\n");
        return -1;
    }
    return manager_ftp_reply("226 Transfer complete.\r\n");
}

static int manager_ftp_write_all(SceUID file, const unsigned char *data,
                                 size_t size)
{
    size_t written = 0u;
    while (written < size) {
        const int result = sceIoWrite(file, data + written,
                                      (unsigned int)(size - written));
        if (result <= 0)
            return -1;
        written += (size_t)result;
    }
    return 0;
}

static int manager_ftp_store(struct manager_ftp_session *session,
                             const char *vita_path)
{
    unsigned char *buffer;
    SceUID file;
    int data_socket;
    int receive_result = 0;

    if (manager_ftp_accept_data(session) < 0)
        return -1;
    file = sceIoOpen(vita_path, SCE_O_CREAT | SCE_O_WRONLY | SCE_O_TRUNC, 0777);
    if (file < 0) {
        manager_ftp_close_data();
        (void)manager_ftp_reply("550 File unavailable.\r\n");
        return -1;
    }
    buffer = (unsigned char *)malloc(MANAGER_FTP_FILE_BUFFER_SIZE);
    if (buffer == NULL) {
        (void)sceIoClose(file);
        manager_ftp_close_data();
        (void)manager_ftp_reply("451 Out of memory.\r\n");
        return -1;
    }
    if (manager_ftp_reply("150 Opening binary data connection.\r\n") < 0) {
        free(buffer);
        (void)sceIoClose(file);
        manager_ftp_close_data();
        return -1;
    }
    data_socket = manager_ftp_load_socket(MANAGER_FTP_SOCKET_DATA, 1);
    if (data_socket < 0) {
        free(buffer);
        (void)sceIoClose(file);
        manager_ftp_close_data();
        return -1;
    }

    while ((receive_result = sceNetRecv(data_socket, buffer,
                                        MANAGER_FTP_FILE_BUFFER_SIZE, 0)) > 0) {
        if (manager_ftp_write_all(file, buffer, (size_t)receive_result) < 0) {
            receive_result = -1;
            break;
        }
    }
    free(buffer);
    (void)sceIoClose(file);
    manager_ftp_close_data();
    if (receive_result < 0) {
        (void)sceIoRemove(vita_path);
        (void)manager_ftp_reply("426 Data transfer aborted.\r\n");
        return -1;
    }
    return manager_ftp_reply("226 Transfer complete.\r\n");
}

static int manager_ftp_resolve_command_path(
    struct manager_ftp_session *session,
    const struct manager_ftp_parsed_command *command,
    char *virtual_path, char *vita_path)
{
    const enum manager_ftp_path_requirement requirement =
        manager_ftp_command_path_requirement(command->command);

    if (requirement == MANAGER_FTP_PATH_NONE)
        return 0;
    if (requirement == MANAGER_FTP_PATH_REQUIRED && !command->has_argument) {
        (void)manager_ftp_reply("501 Path required.\r\n");
        return -1;
    }
    if (command->command == MANAGER_FTP_COMMAND_CWD &&
        command->has_argument && strcmp(command->argument, "..") == 0) {
        if (manager_ftp_path_parent(session->cwd,
                                    virtual_path, MANAGER_FTP_PATH_CAPACITY,
                                    vita_path, MANAGER_FTP_PATH_CAPACITY) ==
            MANAGER_FTP_PATH_OK)
            return 0;
    } else if (manager_ftp_path_resolve(
                   session->cwd,
                   command->has_argument ? command->argument : session->cwd,
                   virtual_path, MANAGER_FTP_PATH_CAPACITY,
                   vita_path, MANAGER_FTP_PATH_CAPACITY) ==
               MANAGER_FTP_PATH_OK) {
        return 0;
    }
    (void)manager_ftp_reply("550 Path is outside the Isaac data root.\r\n");
    return -1;
}

static int manager_ftp_change_directory(struct manager_ftp_session *session,
                                        const char *virtual_path,
                                        const char *vita_path)
{
    const SceUID directory = sceIoDopen(vita_path);
    if (directory < 0)
        return manager_ftp_reply("550 Invalid directory.\r\n");
    (void)sceIoDclose(directory);
    memcpy(session->cwd, virtual_path, strlen(virtual_path) + 1u);
    return manager_ftp_reply("250 Directory changed.\r\n");
}

static int manager_ftp_handle_command(
    struct manager_ftp_session *session,
    const struct manager_ftp_parsed_command *command)
{
    char virtual_path[MANAGER_FTP_PATH_CAPACITY];
    char vita_path[MANAGER_FTP_PATH_CAPACITY];
    SceIoStat stat;

    virtual_path[0] = '\0';
    vita_path[0] = '\0';
    if (manager_ftp_resolve_command_path(session, command,
                                         virtual_path, vita_path) < 0)
        return 0;

    switch (command->command) {
    case MANAGER_FTP_COMMAND_NOOP:
        (void)manager_ftp_reply("200 OK.\r\n");
        break;
    case MANAGER_FTP_COMMAND_USER:
        (void)manager_ftp_reply("331 Password required.\r\n");
        break;
    case MANAGER_FTP_COMMAND_PASS:
        (void)manager_ftp_reply("230 Logged in to Isaac data root.\r\n");
        break;
    case MANAGER_FTP_COMMAND_QUIT:
        (void)manager_ftp_reply("221 Goodbye.\r\n");
        return 1;
    case MANAGER_FTP_COMMAND_SYST:
        (void)manager_ftp_reply("215 UNIX Type: L8.\r\n");
        break;
    case MANAGER_FTP_COMMAND_FEAT:
        (void)manager_ftp_reply(
            "211-Features\r\n MLSD\r\n SIZE\r\n UTF8\r\n TVFS\r\n211 End\r\n");
        break;
    case MANAGER_FTP_COMMAND_OPTS:
        (void)manager_ftp_reply("200 UTF8 enabled.\r\n");
        break;
    case MANAGER_FTP_COMMAND_TYPE:
        (void)manager_ftp_reply("200 Binary mode.\r\n");
        break;
    case MANAGER_FTP_COMMAND_PASV:
        (void)manager_ftp_open_passive(session);
        break;
    case MANAGER_FTP_COMMAND_PWD:
        (void)manager_ftp_reply("257 \"%s\" is current directory.\r\n",
                                session->cwd);
        break;
    case MANAGER_FTP_COMMAND_CWD:
        (void)manager_ftp_change_directory(session, virtual_path, vita_path);
        break;
    case MANAGER_FTP_COMMAND_CDUP:
        if (manager_ftp_path_parent(session->cwd,
                                    virtual_path, sizeof(virtual_path),
                                    vita_path, sizeof(vita_path)) ==
            MANAGER_FTP_PATH_OK) {
            (void)manager_ftp_change_directory(session, virtual_path, vita_path);
        } else {
            (void)manager_ftp_reply("550 Invalid directory.\r\n");
        }
        break;
    case MANAGER_FTP_COMMAND_LIST:
    case MANAGER_FTP_COMMAND_NLST:
    case MANAGER_FTP_COMMAND_MLSD:
        (void)manager_ftp_list_directory(session, command->command, vita_path);
        break;
    case MANAGER_FTP_COMMAND_RETR:
        if (manager_ftp_path_is_root(virtual_path))
            (void)manager_ftp_reply("550 Root is not a file.\r\n");
        else
            (void)manager_ftp_retrieve(session, vita_path);
        break;
    case MANAGER_FTP_COMMAND_STOR:
        if (manager_ftp_path_is_root(virtual_path))
            (void)manager_ftp_reply("550 Root is immutable.\r\n");
        else
            (void)manager_ftp_store(session, vita_path);
        break;
    case MANAGER_FTP_COMMAND_DELE:
        if (manager_ftp_path_is_root(virtual_path) || sceIoRemove(vita_path) < 0)
            (void)manager_ftp_reply("550 Delete failed.\r\n");
        else
            (void)manager_ftp_reply("250 File deleted.\r\n");
        break;
    case MANAGER_FTP_COMMAND_RMD:
        if (manager_ftp_path_is_root(virtual_path) || sceIoRmdir(vita_path) < 0)
            (void)manager_ftp_reply("550 Remove directory failed.\r\n");
        else
            (void)manager_ftp_reply("250 Directory removed.\r\n");
        break;
    case MANAGER_FTP_COMMAND_MKD:
        if (manager_ftp_path_is_root(virtual_path) ||
            sceIoMkdir(vita_path, 0777) < 0) {
            (void)manager_ftp_reply("550 Create directory failed.\r\n");
        } else {
            (void)manager_ftp_reply("257 \"%s\" created.\r\n", virtual_path);
        }
        break;
    case MANAGER_FTP_COMMAND_RNFR:
        session->rename_ready = 0;
        if (manager_ftp_path_is_root(virtual_path) ||
            sceIoGetstat(vita_path, &stat) < 0) {
            (void)manager_ftp_reply("550 Rename source unavailable.\r\n");
        } else {
            memcpy(session->rename_path, vita_path, strlen(vita_path) + 1u);
            session->rename_ready = 1;
            (void)manager_ftp_reply("350 Send destination.\r\n");
        }
        break;
    case MANAGER_FTP_COMMAND_RNTO:
        if (!session->rename_ready) {
            (void)manager_ftp_reply("503 RNFR required first.\r\n");
        } else if (manager_ftp_path_is_root(virtual_path) ||
                   sceIoRename(session->rename_path, vita_path) < 0) {
            session->rename_ready = 0;
            (void)manager_ftp_reply("550 Rename failed.\r\n");
        } else {
            session->rename_ready = 0;
            (void)manager_ftp_reply("250 Rename complete.\r\n");
        }
        break;
    case MANAGER_FTP_COMMAND_SIZE:
        memset(&stat, 0, sizeof(stat));
        if (manager_ftp_path_is_root(virtual_path) ||
            sceIoGetstat(vita_path, &stat) < 0 ||
            SCE_S_ISDIR(stat.st_mode)) {
            (void)manager_ftp_reply("550 File unavailable.\r\n");
        } else {
            (void)manager_ftp_reply("213 %llu\r\n",
                                    (unsigned long long)stat.st_size);
        }
        break;
    case MANAGER_FTP_COMMAND_UNKNOWN:
    default:
        (void)manager_ftp_reply("502 Command not implemented.\r\n");
        break;
    }
    return 0;
}

static int manager_ftp_process_control(struct manager_ftp_session *session)
{
    char buffer[MANAGER_FTP_CONTROL_BUFFER_SIZE];
    int control_socket;
    size_t used = 0u;

    control_socket = manager_ftp_load_socket(MANAGER_FTP_SOCKET_CONTROL, 1);
    if (control_socket < 0 ||
        manager_ftp_reply(
            "220 Isaac Manager FTP; paths map to the Isaac data root.\r\n") < 0)
        return -1;
    while (!manager_ftp_should_stop()) {
        char *newline;
        int received;

        if (used == sizeof(buffer)) {
            used = 0u;
            (void)manager_ftp_reply("500 Command line too long.\r\n");
        }
        received = sceNetRecv(control_socket, buffer + used,
                              (unsigned int)(sizeof(buffer) - used), 0);
        if (received <= 0)
            break;
        used += (size_t)received;
        while ((newline = (char *)memchr(buffer, '\n', used)) != NULL) {
            const size_t line_length = (size_t)(newline - buffer) + 1u;
            struct manager_ftp_parsed_command command;
            int close_requested = 0;

            if (manager_ftp_parse_command(buffer, line_length, &command) < 0) {
                (void)manager_ftp_reply("500 Malformed command.\r\n");
            } else {
                close_requested = manager_ftp_handle_command(session, &command);
            }
            memmove(buffer, buffer + line_length, used - line_length);
            used -= line_length;
            if (close_requested || manager_ftp_should_stop())
                return 0;
        }
    }
    return 0;
}

static int manager_ftp_worker(SceSize arguments_size, void *arguments)
{
    (void)arguments_size;
    (void)arguments;

    while (!manager_ftp_should_stop()) {
        struct manager_ftp_session session;
        SceNetSockaddrIn peer;
        unsigned int peer_size = sizeof(peer);
        int listen_socket;
        int socket;

        listen_socket = manager_ftp_load_socket(MANAGER_FTP_SOCKET_LISTEN, 1);
        if (listen_socket < 0)
            break;
        memset(&peer, 0, sizeof(peer));
        socket = sceNetAccept(listen_socket,
                              (SceNetSockaddr *)&peer, &peer_size);
        if (socket < 0)
            break;
        if (manager_ftp_publish_socket(MANAGER_FTP_SOCKET_CONTROL, socket) < 0)
            break;

        memset(&session, 0, sizeof(session));
        session.peer = peer;
        memcpy(session.cwd, "/", 2u);
        (void)manager_ftp_process_control(&session);
        manager_ftp_close_data();
        manager_ftp_close_socket(MANAGER_FTP_SOCKET_CONTROL);
    }

    manager_ftp_set_worker_running(0);
    return 0;
}

static int manager_ftp_ensure_root(void)
{
    SceUID directory = sceIoDopen(MANAGER_FTP_VITA_ROOT);
    if (directory >= 0) {
        (void)sceIoDclose(directory);
        return 0;
    }
    if (sceIoMkdir(MANAGER_FTP_VITA_ROOT, 0777) < 0)
        return -1;
    directory = sceIoDopen(MANAGER_FTP_VITA_ROOT);
    if (directory < 0)
        return -1;
    (void)sceIoDclose(directory);
    return 0;
}

static void manager_ftp_network_fini(void)
{
    if (manager_ftp_server.netctl_owned)
        (void)sceNetCtlTerm();
    if (manager_ftp_server.net_owned)
        (void)sceNetTerm();
    free(manager_ftp_server.net_memory);
    manager_ftp_server.net_memory = NULL;
    manager_ftp_server.netctl_owned = 0;
    manager_ftp_server.net_owned = 0;
    if (manager_ftp_server.module_loaded)
        (void)sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
    manager_ftp_server.module_loaded = 0;
}

static int manager_ftp_network_init(void)
{
    SceNetInitParam parameters;
    SceNetCtlInfo info;
    int result;

    result = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    if (result < 0)
        return result;
    manager_ftp_server.module_loaded = 1;

    result = sceNetShowNetstat();
    if ((uint32_t)result == (uint32_t)SCE_NET_ERROR_ENOTINIT) {
        manager_ftp_server.net_memory = malloc(MANAGER_FTP_NET_MEMORY_SIZE);
        if (manager_ftp_server.net_memory == NULL)
            return -1;
        memset(&parameters, 0, sizeof(parameters));
        parameters.memory = manager_ftp_server.net_memory;
        parameters.size = MANAGER_FTP_NET_MEMORY_SIZE;
        result = sceNetInit(&parameters);
        if (result < 0)
            return result;
        manager_ftp_server.net_owned = 1;
    } else if (result < 0) {
        return result;
    }

    result = sceNetCtlInit();
    if (result == 0) {
        manager_ftp_server.netctl_owned = 1;
    } else if (result != MANAGER_FTP_NETCTL_ALREADY_INITIALIZED) {
        return result;
    }

    memset(&info, 0, sizeof(info));
    result = sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info);
    if (result < 0)
        return result;
    if (strlen(info.ip_address) >= sizeof(manager_ftp_server.ip))
        return -1;
    memcpy(manager_ftp_server.ip, info.ip_address, strlen(info.ip_address) + 1u);
    result = sceNetInetPton(SCE_NET_AF_INET, manager_ftp_server.ip,
                            &manager_ftp_server.address);
    return result == 1 ? 0 : (result < 0 ? result : -1);
}

int manager_ftp_server_start(char *ip, size_t ip_size,
                             unsigned short *port)
{
    SceNetSockaddrIn address;
    int listen_socket;
    int reuse = 1;
    int result;

    if (manager_ftp_server.started || ip == NULL || port == NULL || ip_size < 16u)
        return -1;
    manager_ftp_server.state_mutex = -1;
    manager_ftp_server.worker = -1;
    manager_ftp_lifecycle_init(&manager_ftp_server.lifecycle);
    manager_ftp_server.ip[0] = '\0';

    if (manager_ftp_ensure_root() < 0)
        return -1;
    manager_ftp_server.state_mutex = sceKernelCreateMutex(
        "isaac_manager_ftp_state", 0, 0, NULL);
    if (manager_ftp_server.state_mutex < 0)
        return manager_ftp_server.state_mutex;
    result = manager_ftp_network_init();
    if (result < 0)
        goto failure;

    listen_socket = sceNetSocket(
        "isaac_manager_ftp", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (listen_socket < 0) {
        result = listen_socket;
        goto failure;
    }
    if (manager_ftp_publish_socket(MANAGER_FTP_SOCKET_LISTEN,
                                   listen_socket) < 0) {
        result = -1;
        goto failure;
    }
    (void)sceNetSetsockopt(listen_socket, SCE_NET_SOL_SOCKET,
                           SCE_NET_SO_REUSEADDR, &reuse, sizeof(reuse));
    memset(&address, 0, sizeof(address));
    address.sin_family = SCE_NET_AF_INET;
    address.sin_addr.s_addr = sceNetHtonl(SCE_NET_INADDR_ANY);
    address.sin_port = sceNetHtons(MANAGER_FTP_PORT);
    result = sceNetBind(listen_socket,
                        (const SceNetSockaddr *)&address, sizeof(address));
    if (result < 0)
        goto failure;
    result = sceNetListen(listen_socket, 2);
    if (result < 0)
        goto failure;

    manager_ftp_server.worker = sceKernelCreateThread(
        "isaac_manager_ftp_worker", manager_ftp_worker,
        0x10000100, 0x10000, 0, 0, NULL);
    if (manager_ftp_server.worker < 0) {
        result = manager_ftp_server.worker;
        goto failure;
    }
    manager_ftp_set_worker_running(1);
    result = sceKernelStartThread(manager_ftp_server.worker, 0, NULL);
    if (result < 0) {
        manager_ftp_set_worker_running(0);
        (void)sceKernelDeleteThread(manager_ftp_server.worker);
        manager_ftp_server.worker = -1;
        goto failure;
    }

    manager_ftp_server.started = 1;
    memcpy(ip, manager_ftp_server.ip, strlen(manager_ftp_server.ip) + 1u);
    *port = MANAGER_FTP_PORT;
    return 0;

failure:
    manager_ftp_close_socket(MANAGER_FTP_SOCKET_LISTEN);
    manager_ftp_network_fini();
    if (manager_ftp_server.state_mutex >= 0)
        (void)sceKernelDeleteMutex(manager_ftp_server.state_mutex);
    manager_ftp_server.state_mutex = -1;
    manager_ftp_lifecycle_init(&manager_ftp_server.lifecycle);
    return result < 0 ? result : -1;
}

void manager_ftp_server_stop(void)
{
    struct manager_ftp_stop_snapshot snapshot;
    int exit_status;

    if (!manager_ftp_server.started)
        return;
    if (manager_ftp_state_lock() < 0)
        return;

    /* This mutex is the stop/publication handshake. Every descriptor is
     * either already in this snapshot, or its later publication is rejected
     * and the publishing thread closes it. Connected control/data descriptors
     * remain worker-owned: shutdown covers a call not entered yet, abort wakes
     * one already sleeping, and the worker performs the final close. */
    manager_ftp_lifecycle_begin_stop(&manager_ftp_server.lifecycle, &snapshot);
    manager_ftp_shutdown_and_abort_socket(
        snapshot.sockets[MANAGER_FTP_SOCKET_CONTROL]);
    manager_ftp_shutdown_and_abort_socket(
        snapshot.sockets[MANAGER_FTP_SOCKET_DATA]);
    manager_ftp_state_unlock();

    if (snapshot.sockets[MANAGER_FTP_SOCKET_LISTEN] >= 0)
        (void)sceNetSocketClose(
            snapshot.sockets[MANAGER_FTP_SOCKET_LISTEN]);
    if (snapshot.sockets[MANAGER_FTP_SOCKET_PASSIVE] >= 0)
        (void)sceNetSocketClose(
            snapshot.sockets[MANAGER_FTP_SOCKET_PASSIVE]);
    if (manager_ftp_server.worker >= 0) {
        (void)sceKernelWaitThreadEnd(manager_ftp_server.worker,
                                     &exit_status, NULL);
        (void)sceKernelDeleteThread(manager_ftp_server.worker);
    }
    manager_ftp_server.worker = -1;
    manager_ftp_close_data();
    manager_ftp_close_socket(MANAGER_FTP_SOCKET_CONTROL);
    manager_ftp_network_fini();
    manager_ftp_server.started = 0;
    manager_ftp_server.ip[0] = '\0';
    (void)sceKernelDeleteMutex(manager_ftp_server.state_mutex);
    manager_ftp_server.state_mutex = -1;
    manager_ftp_lifecycle_init(&manager_ftp_server.lifecycle);
}

int manager_ftp_server_is_listening(void)
{
    int listening;

    if (!manager_ftp_server.started || manager_ftp_state_lock() < 0)
        return 0;
    listening = manager_ftp_lifecycle_is_listening(
        &manager_ftp_server.lifecycle);
    manager_ftp_state_unlock();
    return listening;
}
