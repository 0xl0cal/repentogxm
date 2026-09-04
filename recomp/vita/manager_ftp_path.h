/*
 * Lexical FTP-to-Vita path policy for the Isaac manager's single mapped root.
 * This is not a filesystem sandbox for pre-existing native symbolic links or
 * mount aliases. This file is project code; the server using it is an
 * adaptation of xerpi/libftpvita pinned in
 * third_party/libftpvita/PROVENANCE.md.
 */

#ifndef ISAAC_MANAGER_FTP_PATH_H
#define ISAAC_MANAGER_FTP_PATH_H

#include <stddef.h>

#define MANAGER_FTP_VITA_ROOT "ux0:/data/isaacr001"
#define MANAGER_FTP_LEGACY_ROOT "/ux0:/data/isaacr001"
#define MANAGER_FTP_PATH_CAPACITY 1024u

enum manager_ftp_path_result {
    MANAGER_FTP_PATH_OK = 0,
    MANAGER_FTP_PATH_INVALID = -1,
    MANAGER_FTP_PATH_TRAVERSAL = -2,
    MANAGER_FTP_PATH_TOO_LONG = -3,
};

/*
 * Resolve input against canonical virtual cwd.  virtual_path is always rooted
 * at the FTP-visible "/"; vita_path is always rooted at
 * ux0:/data/isaacr001.  For compatibility with the existing PC sync CLI, the
 * exact old VitaShell spelling /ux0:/data/isaacr001[/...] is an alias of the
 * virtual root.  No other device-qualified path is accepted.
 */
int manager_ftp_path_resolve(const char *cwd, const char *input,
                             char *virtual_path, size_t virtual_size,
                             char *vita_path, size_t vita_size);

/* Move a canonical virtual path up one level without leaving the mapped root. */
int manager_ftp_path_parent(const char *cwd,
                            char *virtual_path, size_t virtual_size,
                            char *vita_path, size_t vita_size);

int manager_ftp_path_is_root(const char *virtual_path);

#endif
