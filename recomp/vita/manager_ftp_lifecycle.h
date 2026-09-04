#ifndef ISAAC_MANAGER_FTP_LIFECYCLE_H
#define ISAAC_MANAGER_FTP_LIFECYCLE_H

/*
 * Small, platform-neutral state machine for the FTP worker lifecycle.
 *
 * The caller must serialize every access to this structure.  The Vita server
 * does that with one kernel mutex, so socket publication and stop_begin form a
 * single linear order: either stop sees a published descriptor and aborts it,
 * or publication sees the stop flag and rejects the descriptor.
 */

enum manager_ftp_socket_slot {
    MANAGER_FTP_SOCKET_LISTEN = 0,
    MANAGER_FTP_SOCKET_CONTROL,
    MANAGER_FTP_SOCKET_PASSIVE,
    MANAGER_FTP_SOCKET_DATA,
    MANAGER_FTP_SOCKET_COUNT
};

struct manager_ftp_lifecycle {
    int stop_requested;
    int worker_running;
    int sockets[MANAGER_FTP_SOCKET_COUNT];
};

struct manager_ftp_stop_snapshot {
    int sockets[MANAGER_FTP_SOCKET_COUNT];
};

void manager_ftp_lifecycle_init(struct manager_ftp_lifecycle *state);
int manager_ftp_lifecycle_should_stop(const struct manager_ftp_lifecycle *state);
void manager_ftp_lifecycle_set_worker_running(struct manager_ftp_lifecycle *state,
                                              int running);
int manager_ftp_lifecycle_is_listening(const struct manager_ftp_lifecycle *state);
int manager_ftp_lifecycle_publish(struct manager_ftp_lifecycle *state,
                                  enum manager_ftp_socket_slot slot,
                                  int socket);
int manager_ftp_lifecycle_load(const struct manager_ftp_lifecycle *state,
                               enum manager_ftp_socket_slot slot);
int manager_ftp_lifecycle_load_if_running(const struct manager_ftp_lifecycle *state,
                                          enum manager_ftp_socket_slot slot);
int manager_ftp_lifecycle_take(struct manager_ftp_lifecycle *state,
                               enum manager_ftp_socket_slot slot);
void manager_ftp_lifecycle_begin_stop(struct manager_ftp_lifecycle *state,
                                      struct manager_ftp_stop_snapshot *snapshot);

#endif
