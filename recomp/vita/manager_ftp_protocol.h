#ifndef ISAAC_MANAGER_FTP_PROTOCOL_H
#define ISAAC_MANAGER_FTP_PROTOCOL_H

#include <stddef.h>

#include "manager_ftp_path.h"

enum manager_ftp_command {
    MANAGER_FTP_COMMAND_UNKNOWN = 0,
    MANAGER_FTP_COMMAND_NOOP,
    MANAGER_FTP_COMMAND_USER,
    MANAGER_FTP_COMMAND_PASS,
    MANAGER_FTP_COMMAND_QUIT,
    MANAGER_FTP_COMMAND_SYST,
    MANAGER_FTP_COMMAND_FEAT,
    MANAGER_FTP_COMMAND_OPTS,
    MANAGER_FTP_COMMAND_TYPE,
    MANAGER_FTP_COMMAND_PASV,
    MANAGER_FTP_COMMAND_PWD,
    MANAGER_FTP_COMMAND_CWD,
    MANAGER_FTP_COMMAND_CDUP,
    MANAGER_FTP_COMMAND_LIST,
    MANAGER_FTP_COMMAND_NLST,
    MANAGER_FTP_COMMAND_MLSD,
    MANAGER_FTP_COMMAND_RETR,
    MANAGER_FTP_COMMAND_STOR,
    MANAGER_FTP_COMMAND_DELE,
    MANAGER_FTP_COMMAND_RMD,
    MANAGER_FTP_COMMAND_MKD,
    MANAGER_FTP_COMMAND_RNFR,
    MANAGER_FTP_COMMAND_RNTO,
    MANAGER_FTP_COMMAND_SIZE,
};

enum manager_ftp_path_requirement {
    MANAGER_FTP_PATH_NONE = 0,
    MANAGER_FTP_PATH_OPTIONAL,
    MANAGER_FTP_PATH_REQUIRED,
};

struct manager_ftp_parsed_command {
    enum manager_ftp_command command;
    int has_argument;
    char argument[MANAGER_FTP_PATH_CAPACITY];
};

int manager_ftp_parse_command(const char *line, size_t line_length,
                              struct manager_ftp_parsed_command *parsed);
enum manager_ftp_path_requirement manager_ftp_command_path_requirement(
    enum manager_ftp_command command);

#endif
