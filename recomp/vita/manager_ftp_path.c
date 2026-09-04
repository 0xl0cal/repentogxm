#include "manager_ftp_path.h"

#include <string.h>

static int manager_ftp_append(char *output, size_t output_size,
                              size_t *length, const char *data,
                              size_t data_length)
{
    if (*length >= output_size || data_length > output_size - *length - 1u)
        return MANAGER_FTP_PATH_TOO_LONG;
    memcpy(output + *length, data, data_length);
    *length += data_length;
    output[*length] = '\0';
    return MANAGER_FTP_PATH_OK;
}

static int manager_ftp_bad_character(unsigned char value)
{
    return value < 0x20u || value == 0x7fu || value == '\\' || value == ':' ||
           value == '"';
}

static int manager_ftp_append_segments(char *output, size_t output_size,
                                       size_t *length, const char *input)
{
    const char *cursor = input;

    while (*cursor != '\0') {
        const char *segment;
        size_t segment_length;
        size_t index;
        int result;

        while (*cursor == '/')
            ++cursor;
        if (*cursor == '\0')
            break;
        segment = cursor;
        while (*cursor != '\0' && *cursor != '/')
            ++cursor;
        segment_length = (size_t)(cursor - segment);

        if ((segment_length == 1u && segment[0] == '.') ||
            (segment_length == 2u && segment[0] == '.' && segment[1] == '.'))
            return MANAGER_FTP_PATH_TRAVERSAL;
        for (index = 0u; index < segment_length; ++index) {
            if (manager_ftp_bad_character((unsigned char)segment[index]))
                return MANAGER_FTP_PATH_INVALID;
        }

        if (*length > 1u) {
            result = manager_ftp_append(output, output_size, length, "/", 1u);
            if (result != MANAGER_FTP_PATH_OK)
                return result;
        }
        result = manager_ftp_append(output, output_size, length,
                                    segment, segment_length);
        if (result != MANAGER_FTP_PATH_OK)
            return result;
    }
    return MANAGER_FTP_PATH_OK;
}

static int manager_ftp_strip_legacy_root(const char *input,
                                         const char **relative)
{
    const size_t vita_length = sizeof(MANAGER_FTP_VITA_ROOT) - 1u;
    const size_t ftp_length = sizeof(MANAGER_FTP_LEGACY_ROOT) - 1u;

    if (strncmp(input, MANAGER_FTP_LEGACY_ROOT, ftp_length) == 0 &&
        (input[ftp_length] == '\0' || input[ftp_length] == '/')) {
        *relative = input + ftp_length;
        return 1;
    }
    if (strncmp(input, MANAGER_FTP_VITA_ROOT, vita_length) == 0 &&
        (input[vita_length] == '\0' || input[vita_length] == '/')) {
        *relative = input + vita_length;
        return 1;
    }
    return 0;
}

int manager_ftp_path_resolve(const char *cwd, const char *input,
                             char *virtual_path, size_t virtual_size,
                             char *vita_path, size_t vita_size)
{
    const char *relative;
    size_t virtual_length = 0u;
    size_t vita_length = 0u;
    int result;

    if (cwd == NULL || input == NULL || virtual_path == NULL ||
        vita_path == NULL || virtual_size < 2u || vita_size < 2u ||
        cwd[0] != '/')
        return MANAGER_FTP_PATH_INVALID;

    result = manager_ftp_append(virtual_path, virtual_size, &virtual_length,
                                "/", 1u);
    if (result != MANAGER_FTP_PATH_OK)
        return result;

    if (manager_ftp_strip_legacy_root(input, &relative)) {
        /* The exact legacy root is an absolute alias, never a prefix match. */
    } else if (input[0] == '/') {
        relative = input + 1;
    } else {
        result = manager_ftp_append_segments(virtual_path, virtual_size,
                                             &virtual_length, cwd + 1);
        if (result != MANAGER_FTP_PATH_OK)
            return result;
        relative = input;
    }

    result = manager_ftp_append_segments(virtual_path, virtual_size,
                                         &virtual_length, relative);
    if (result != MANAGER_FTP_PATH_OK)
        return result;

    result = manager_ftp_append(vita_path, vita_size, &vita_length,
                                MANAGER_FTP_VITA_ROOT,
                                sizeof(MANAGER_FTP_VITA_ROOT) - 1u);
    if (result != MANAGER_FTP_PATH_OK)
        return result;
    if (virtual_length > 1u) {
        result = manager_ftp_append(vita_path, vita_size, &vita_length,
                                    virtual_path, virtual_length);
        if (result != MANAGER_FTP_PATH_OK)
            return result;
    }
    return MANAGER_FTP_PATH_OK;
}

int manager_ftp_path_parent(const char *cwd,
                            char *virtual_path, size_t virtual_size,
                            char *vita_path, size_t vita_size)
{
    char canonical[MANAGER_FTP_PATH_CAPACITY];
    char unused_vita[MANAGER_FTP_PATH_CAPACITY];
    char *slash;
    int result;

    result = manager_ftp_path_resolve("/", cwd,
                                      canonical, sizeof(canonical),
                                      unused_vita, sizeof(unused_vita));
    if (result != MANAGER_FTP_PATH_OK)
        return result;
    if (canonical[1] != '\0') {
        slash = strrchr(canonical, '/');
        if (slash == canonical)
            canonical[1] = '\0';
        else
            *slash = '\0';
    }
    return manager_ftp_path_resolve("/", canonical,
                                    virtual_path, virtual_size,
                                    vita_path, vita_size);
}

int manager_ftp_path_is_root(const char *virtual_path)
{
    return virtual_path != NULL && virtual_path[0] == '/' &&
           virtual_path[1] == '\0';
}
