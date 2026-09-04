#ifndef ISAAC_MANAGER_FTP_SERVER_H
#define ISAAC_MANAGER_FTP_SERVER_H

#include <stddef.h>

/* Start one passive-only FTP server on port 1337. */
int manager_ftp_server_start(char *ip, size_t ip_size,
                             unsigned short *port);

/* Idempotent. Joins the worker before tearing down SceNet/SceNetCtl. */
void manager_ftp_server_stop(void);
int manager_ftp_server_is_listening(void);

#endif
