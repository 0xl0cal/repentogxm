#include "manager_ftp_protocol.h"

#include <ctype.h>
#include <string.h>

struct manager_ftp_command_name {
    const char *name;
    enum manager_ftp_command command;
};

static const struct manager_ftp_command_name manager_ftp_command_names[] = {
    {"NOOP", MANAGER_FTP_COMMAND_NOOP},
    {"USER", MANAGER_FTP_COMMAND_USER},
    {"PASS", MANAGER_FTP_COMMAND_PASS},
    {"QUIT", MANAGER_FTP_COMMAND_QUIT},
    {"SYST", MANAGER_FTP_COMMAND_SYST},
    {"FEAT", MANAGER_FTP_COMMAND_FEAT},
    {"OPTS", MANAGER_FTP_COMMAND_OPTS},
    {"TYPE", MANAGER_FTP_COMMAND_TYPE},
    {"PASV", MANAGER_FTP_COMMAND_PASV},
    {"PWD", MANAGER_FTP_COMMAND_PWD},
    {"CWD", MANAGER_FTP_COMMAND_CWD},
    {"CDUP", MANAGER_FTP_COMMAND_CDUP},
    {"LIST", MANAGER_FTP_COMMAND_LIST},
    {"NLST", MANAGER_FTP_COMMAND_NLST},
    {"MLSD", MANAGER_FTP_COMMAND_MLSD},
    {"RETR", MANAGER_FTP_COMMAND_RETR},
    {"STOR", MANAGER_FTP_COMMAND_STOR},
    {"DELE", MANAGER_FTP_COMMAND_DELE},
    {"RMD", MANAGER_FTP_COMMAND_RMD},
    {"MKD", MANAGER_FTP_COMMAND_MKD},
    {"RNFR", MANAGER_FTP_COMMAND_RNFR},
    {"RNTO", MANAGER_FTP_COMMAND_RNTO},
    {"SIZE", MANAGER_FTP_COMMAND_SIZE},
};

int manager_ftp_parse_command(const char *line, size_t line_length,
                              struct manager_ftp_parsed_command *parsed)
{
    char command[9];
    size_t command_length = 0u;
    size_t cursor = 0u;
    size_t argument_end;
    size_t argument_length;
    size_t index;

    if (line == NULL || parsed == NULL || line_length == 0u)
        return -1;
    memset(parsed, 0, sizeof(*parsed));

    while (line_length > 0u &&
           (line[line_length - 1u] == '\r' || line[line_length - 1u] == '\n'))
        --line_length;
    while (cursor < line_length && line[cursor] != ' ' && line[cursor] != '\t') {
        const unsigned char value = (unsigned char)line[cursor];
        if (!isalpha(value) || command_length == sizeof(command) - 1u)
            return -1;
        command[command_length++] = (char)toupper(value);
        ++cursor;
    }
    if (command_length == 0u)
        return -1;
    command[command_length] = '\0';

    parsed->command = MANAGER_FTP_COMMAND_UNKNOWN;
    for (index = 0u;
         index < sizeof(manager_ftp_command_names) /
                     sizeof(manager_ftp_command_names[0]);
         ++index) {
        if (strcmp(command, manager_ftp_command_names[index].name) == 0) {
            parsed->command = manager_ftp_command_names[index].command;
            break;
        }
    }

    while (cursor < line_length &&
           (line[cursor] == ' ' || line[cursor] == '\t'))
        ++cursor;
    argument_end = line_length;
    while (argument_end > cursor &&
           (line[argument_end - 1u] == ' ' || line[argument_end - 1u] == '\t'))
        --argument_end;
    argument_length = argument_end - cursor;
    if (argument_length >= sizeof(parsed->argument))
        return -1;
    for (index = 0u; index < argument_length; ++index) {
        const unsigned char value = (unsigned char)line[cursor + index];
        if (value < 0x20u || value == 0x7fu)
            return -1;
    }
    if (argument_length > 0u) {
        memcpy(parsed->argument, line + cursor, argument_length);
        parsed->argument[argument_length] = '\0';
        parsed->has_argument = 1;
    }
    return 0;
}

enum manager_ftp_path_requirement manager_ftp_command_path_requirement(
    enum manager_ftp_command command)
{
    switch (command) {
    case MANAGER_FTP_COMMAND_LIST:
    case MANAGER_FTP_COMMAND_NLST:
    case MANAGER_FTP_COMMAND_MLSD:
        return MANAGER_FTP_PATH_OPTIONAL;
    case MANAGER_FTP_COMMAND_CWD:
    case MANAGER_FTP_COMMAND_RETR:
    case MANAGER_FTP_COMMAND_STOR:
    case MANAGER_FTP_COMMAND_DELE:
    case MANAGER_FTP_COMMAND_RMD:
    case MANAGER_FTP_COMMAND_MKD:
    case MANAGER_FTP_COMMAND_RNFR:
    case MANAGER_FTP_COMMAND_RNTO:
    case MANAGER_FTP_COMMAND_SIZE:
        return MANAGER_FTP_PATH_REQUIRED;
    default:
        return MANAGER_FTP_PATH_NONE;
    }
}
