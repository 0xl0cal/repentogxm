#include "manager_ftp_lifecycle.h"

static int manager_ftp_lifecycle_valid_slot(enum manager_ftp_socket_slot slot)
{
    return (unsigned int)slot < (unsigned int)MANAGER_FTP_SOCKET_COUNT;
}

void manager_ftp_lifecycle_init(struct manager_ftp_lifecycle *state)
{
    int i;

    state->stop_requested = 0;
    state->worker_running = 0;
    for (i = 0; i < MANAGER_FTP_SOCKET_COUNT; ++i) {
        state->sockets[i] = -1;
    }
}

int manager_ftp_lifecycle_should_stop(const struct manager_ftp_lifecycle *state)
{
    return state->stop_requested != 0;
}

void manager_ftp_lifecycle_set_worker_running(struct manager_ftp_lifecycle *state,
                                              int running)
{
    state->worker_running = running != 0;
}

int manager_ftp_lifecycle_is_listening(const struct manager_ftp_lifecycle *state)
{
    return !state->stop_requested && state->worker_running &&
           state->sockets[MANAGER_FTP_SOCKET_LISTEN] >= 0;
}

int manager_ftp_lifecycle_publish(struct manager_ftp_lifecycle *state,
                                  enum manager_ftp_socket_slot slot,
                                  int socket)
{
    if (!manager_ftp_lifecycle_valid_slot(slot) || socket < 0 ||
        state->stop_requested || state->sockets[slot] >= 0) {
        return -1;
    }

    state->sockets[slot] = socket;
    return 0;
}

int manager_ftp_lifecycle_load(const struct manager_ftp_lifecycle *state,
                               enum manager_ftp_socket_slot slot)
{
    if (!manager_ftp_lifecycle_valid_slot(slot)) {
        return -1;
    }
    return state->sockets[slot];
}

int manager_ftp_lifecycle_load_if_running(const struct manager_ftp_lifecycle *state,
                                          enum manager_ftp_socket_slot slot)
{
    if (state->stop_requested || !state->worker_running) {
        return -1;
    }
    return manager_ftp_lifecycle_load(state, slot);
}

int manager_ftp_lifecycle_take(struct manager_ftp_lifecycle *state,
                               enum manager_ftp_socket_slot slot)
{
    int socket;

    if (!manager_ftp_lifecycle_valid_slot(slot)) {
        return -1;
    }

    socket = state->sockets[slot];
    state->sockets[slot] = -1;
    return socket;
}

void manager_ftp_lifecycle_begin_stop(struct manager_ftp_lifecycle *state,
                                      struct manager_ftp_stop_snapshot *snapshot)
{
    int i;

    state->stop_requested = 1;
    for (i = 0; i < MANAGER_FTP_SOCKET_COUNT; ++i) {
        snapshot->sockets[i] = state->sockets[i];
    }

    /* The stop caller owns and closes both accept sockets after releasing the
     * lock. Connected descriptors stay worker-owned to avoid fd-reuse races. */
    state->sockets[MANAGER_FTP_SOCKET_LISTEN] = -1;
    state->sockets[MANAGER_FTP_SOCKET_PASSIVE] = -1;
}
