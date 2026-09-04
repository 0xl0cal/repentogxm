#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

#include "host_vita_startup.h"
#include "host_vita_import_id.h"
#include "platform.h"
#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
#include "host_vita_async_write.h"
#endif

typedef void (*vita_startup_import_fn)(CPU *__restrict);

typedef struct vita_startup_import_entry {
    const char *name;
    vita_startup_import_fn fn;
} vita_startup_import_entry;

static void vita_startup_set_last_error(CPU *__restrict c, uint32_t error)
{
    c->last_error = error;
}

static uint32_t vita_startup_arg(CPU *__restrict c, uint32_t index)
{
    return ld32(guest_stack_address(
        c, c->esp + 4U + index * 4U, 4U, 0U));
}

static void vita_startup_stdcall_return(CPU *__restrict c,
                                        uint32_t argument_bytes)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, argument_bytes, 0U);
}

static int vita_startup_range(CPU *__restrict c, uint32_t address,
                              uint32_t size, const char *what)
{
    if (size && (!address || address > UINT32_MAX - (size - 1U))) {
        guest_fault(c, address, what);
        return 0;
    }
    return 1;
}

static int vita_startup_copy_ascii(CPU *__restrict c, uint32_t address,
                                   char *out, uint32_t maximum,
                                   const char *null_fault,
                                   const char *long_fault)
{
    uint32_t i;

    if (!address) {
        guest_fault(c, address, null_fault);
        return 0;
    }
    for (i = 0U; i <= maximum; ++i) {
        uint8_t value;
        if (address > UINT32_MAX - i) {
            guest_fault(c, address, long_fault);
            return 0;
        }
        value = ld8(address + i);
        if (!value) {
            out[i] = '\0';
            return 1;
        }
        if (i == maximum) {
            guest_fault(c, address, long_fault);
            return 0;
        }
        out[i] = (char)value;
    }
    return 0;
}

static char vita_startup_ascii_lower(char value)
{
    if (value >= 'A' && value <= 'Z')
        return (char)(value + ('a' - 'A'));
    return value;
}

static int vita_startup_module_equal(const char *actual,
                                     const char *expected)
{
    while (*actual && *expected) {
        if (vita_startup_ascii_lower(*actual) != *expected)
            return 0;
        ++actual;
        ++expected;
    }
    return *actual == '\0' && *expected == '\0';
}

static uint32_t s_vita_startup_foreground_lock_timeout;

static int vita_startup_path_has_parent(const char *path)
{
    const char *p = path;

    while (*p) {
        const char *component;
        while (*p == '/')
            ++p;
        component = p;
        while (*p && *p != '/') {
            if (*p == '\\')
                return 1;
            ++p;
        }
        if (p - component == 2 && component[0] == '.' &&
            component[1] == '.')
            return 1;
    }
    return 0;
}

static int vita_startup_map_ascii(CPU *__restrict c, const char *guest_path,
                                  char *mapped_path, uint32_t capacity,
                                  const char *mapped_root)
{
    const char *suffix = guest_path;
    const char *rooted_suffix = NULL;
    const char *guest_root = ISAAC_VITA_DATA_ROOT;
    const char *native_root = ISAAC_VITA_NATIVE_DATA_ROOT;
    size_t guest_root_length = strlen(guest_root);
    size_t native_root_length = strlen(native_root);
    size_t mapped_root_length = strlen(mapped_root);
    size_t suffix_length;
    size_t total;
    int add_separator;

    if (!mapped_path || !capacity) {
        guest_fault(c, 0U, "Vita path mapper received no native storage");
        return 0;
    }
    if (strncmp(guest_path, guest_root, guest_root_length) == 0 &&
        (guest_path[guest_root_length] == '\0' ||
         guest_path[guest_root_length] == '/')) {
        rooted_suffix = guest_path + guest_root_length;
    } else if (strncmp(guest_path, native_root, native_root_length) == 0 &&
               (guest_path[native_root_length] == '\0' ||
                guest_path[native_root_length] == '/')) {
        rooted_suffix = guest_path + native_root_length;
    }
    if (rooted_suffix) {
        suffix_length = strlen(rooted_suffix);
        total = mapped_root_length + suffix_length;
        if (total >= capacity) {
            guest_fault(c, 0U, "Vita rooted path exceeds measured bound");
            return 0;
        }
        if (vita_startup_path_has_parent(rooted_suffix)) {
            guest_fault(c, 0U, "Vita rooted path escapes the data root");
            return 0;
        }
        memcpy(mapped_path, mapped_root, mapped_root_length);
        memcpy(mapped_path + mapped_root_length,
               rooted_suffix, suffix_length + 1U);
        return 1;
    }

    if (strchr(guest_path, ':')) {
        isaac_vita_log("Vita path outside root: %s", guest_path);
        guest_fault(c, 0U, "Vita path is outside the data root");
        return 0;
    }

    if (guest_path[0] == '.' && guest_path[1] == '/')
        suffix = guest_path + 2;
    if (vita_startup_path_has_parent(suffix)) {
        guest_fault(c, 0U, "Vita relative path escapes the data root");
        return 0;
    }
    suffix_length = strlen(suffix);
    add_separator = suffix_length != 0U;
    total = mapped_root_length + (size_t)add_separator + suffix_length;
    if (total >= capacity) {
        guest_fault(c, 0U, "Vita mapped path exceeds measured bound");
        return 0;
    }
    memcpy(mapped_path, mapped_root, mapped_root_length);
    if (add_separator)
        mapped_path[mapped_root_length++] = '/';
    memcpy(mapped_path + mapped_root_length, suffix, suffix_length + 1U);
    return 1;
}

int isaac_vita_startup_map_path(CPU *__restrict c, uint32_t guest_path,
                                char *native_path, uint32_t capacity)
{
    char path[ISAAC_VITA_STARTUP_PATH_MAX + 1U];
    size_t native_parent_length;

    if (!vita_startup_copy_ascii(
            c, guest_path, path, ISAAC_VITA_STARTUP_PATH_MAX,
            "Vita path mapper received a null guest path",
            "Vita guest path exceeds measured bound"))
        return 0;

    /* The guest's recursive directory creator first probes the exact data
     * parent before the title root.  Admit that one prefix only; siblings
     * below ux0:data remain outside the title sandbox. */
    if (strcmp(path, ISAAC_VITA_GUEST_DATA_PARENT) == 0 ||
        strcmp(path, ISAAC_VITA_GUEST_DATA_PARENT "/") == 0) {
        native_parent_length = strlen(ISAAC_VITA_NATIVE_DATA_PARENT);
        if (native_parent_length >= capacity) {
            guest_fault(c, 0U, "Vita data-parent path exceeds output bound");
            return 0;
        }
        memcpy(native_path, ISAAC_VITA_NATIVE_DATA_PARENT,
               native_parent_length + 1U);
        return 1;
    }
    return vita_startup_map_ascii(
        c, path, native_path, capacity, ISAAC_VITA_NATIVE_DATA_ROOT);
}

static uint32_t vita_startup_missing_path_error(const char *native_path)
{
    char parent[ISAAC_VITA_STARTUP_PATH_MAX + 1U];
    struct stat information;
    char *separator;
    size_t length = strlen(native_path);

    if (length > ISAAC_VITA_STARTUP_PATH_MAX)
        return ISAAC_VITA_STARTUP_ERROR_PATH_NOT_FOUND;
    memcpy(parent, native_path, length + 1U);
    while (length && parent[length - 1U] == '/')
        parent[--length] = '\0';
    separator = strrchr(parent, '/');
    if (!separator)
        return ISAAC_VITA_STARTUP_ERROR_PATH_NOT_FOUND;
    *separator = '\0';
    if (stat(parent, &information) == 0 && S_ISDIR(information.st_mode))
        return ISAAC_VITA_STARTUP_ERROR_FILE_NOT_FOUND;
    return ISAAC_VITA_STARTUP_ERROR_PATH_NOT_FOUND;
}

static void vita_startup_GetEnvironmentVariableA(CPU *__restrict c)
{
    uint32_t name = vita_startup_arg(c, 0U);
    char measured_name[ISAAC_VITA_STARTUP_ENV_NAME_MAX + 1U];

    if (!vita_startup_copy_ascii(
            c, name, measured_name, ISAAC_VITA_STARTUP_ENV_NAME_MAX,
            "GetEnvironmentVariableA received a null name",
            "GetEnvironmentVariableA name exceeds measured bound"))
        return;

    /* Vita has no ambient Windows environment.  An absent variable returns
     * zero and does not touch the caller's output buffer.  Once the bounded
     * name is valid, the result is known without reading or validating the
     * output pointer or capacity. */
    (void)measured_name;
    vita_startup_set_last_error(c,
        ISAAC_VITA_STARTUP_ERROR_ENVVAR_NOT_FOUND);
    c->eax = 0U;
    vita_startup_stdcall_return(c, 12U);
}

static void vita_startup_GetSystemInfo(CPU *__restrict c)
{
    uint32_t output = vita_startup_arg(c, 0U);

    if (!vita_startup_range(c, output,
                            ISAAC_VITA_STARTUP_SYSTEM_INFO_SIZE,
                            "GetSystemInfo received an invalid output range"))
        return;

    st16(output + 0U,  (uint16_t)ISAAC_VITA_STARTUP_PROCESSOR_ARCHITECTURE);
    st16(output + 2U,  0U);
    st32(output + 4U,  ISAAC_VITA_STARTUP_PAGE_SIZE);
    st32(output + 8U,  ISAAC_VITA_STARTUP_MIN_APP_ADDRESS);
    st32(output + 12U, ISAAC_VITA_STARTUP_MAX_APP_ADDRESS);
    st32(output + 16U, ISAAC_VITA_STARTUP_ACTIVE_PROCESSOR_MASK);
    st32(output + 20U, ISAAC_VITA_STARTUP_PROCESSOR_COUNT);
    st32(output + 24U, ISAAC_VITA_STARTUP_PROCESSOR_TYPE);
    st32(output + 28U, ISAAC_VITA_STARTUP_ALLOCATION_GRANULARITY);
    st16(output + 32U, (uint16_t)ISAAC_VITA_STARTUP_PROCESSOR_LEVEL);
    st16(output + 34U, (uint16_t)ISAAC_VITA_STARTUP_PROCESSOR_REVISION);
    vita_startup_stdcall_return(c, 4U);
}

static void vita_startup_LoadLibraryA(CPU *__restrict c)
{
    uint32_t name = vita_startup_arg(c, 0U);
    char module[ISAAC_VITA_STARTUP_LOAD_LIBRARY_NAME_MAX + 1U];

    if (!vita_startup_copy_ascii(
            c, name, module, ISAAC_VITA_STARTUP_LOAD_LIBRARY_NAME_MAX,
            "LoadLibraryA received a null module name",
            "LoadLibraryA module name exceeds measured bound"))
        return;
    if (!vita_startup_module_equal(module, "kernelbase.dll") &&
        !vita_startup_module_equal(module, "ntdll.dll") &&
        !vita_startup_module_equal(module, "kernel32.dll") &&
        !vita_startup_module_equal(
            module, ISAAC_VITA_STARTUP_LOAD_USERENV_MODULE) &&
        !vita_startup_module_equal(
            module, ISAAC_VITA_STARTUP_LOAD_WINMM_MODULE)) {
        guest_fault(c, name, "LoadLibraryA received an unexpected module");
        return;
    }

    /* The first three loads probe optional allocator entry points; USERENV
     * probes the optional profile-directory API; winmm probes optional GLFW
     * joystick support.  NULL selects each existing fallback and avoids
     * inventing a native module/token or procedure. */
    vita_startup_set_last_error(
        c, ISAAC_VITA_STARTUP_ERROR_MOD_NOT_FOUND);
    c->eax = 0U;
    vita_startup_stdcall_return(c, 4U);
}

static void vita_startup_SystemParametersInfoA(CPU *__restrict c)
{
    uint32_t action = vita_startup_arg(c, 0U);
    uint32_t ui_parameter = vita_startup_arg(c, 1U);
    uint32_t parameter = vita_startup_arg(c, 2U);
    uint32_t update_flags = vita_startup_arg(c, 3U);

    if (ui_parameter != 0U) {
        guest_fault(c, ui_parameter,
                    "SystemParametersInfoA received unexpected uiParam");
        return;
    }
    if (action == ISAAC_VITA_STARTUP_SPI_GET_FOREGROUND_LOCK_TIMEOUT) {
        if (update_flags != ISAAC_VITA_STARTUP_SPI_UPDATE_FLAGS_NONE) {
            guest_fault(c, update_flags,
                        "SystemParametersInfoA GET received unexpected flags");
            return;
        }
        if (!vita_startup_range(
                c, parameter, 4U,
                "SystemParametersInfoA GET received invalid output range"))
            return;
        st32(parameter, s_vita_startup_foreground_lock_timeout);
    } else if (action ==
               ISAAC_VITA_STARTUP_SPI_SET_FOREGROUND_LOCK_TIMEOUT) {
        if (update_flags !=
            ISAAC_VITA_STARTUP_SPI_UPDATE_FLAGS_SEND_CHANGE) {
            guest_fault(c, update_flags,
                        "SystemParametersInfoA SET received unexpected flags");
            return;
        }
        s_vita_startup_foreground_lock_timeout = parameter;
    } else {
        guest_fault(c, action,
                    "SystemParametersInfoA received unsupported action");
        return;
    }

    c->eax = 1U;
    vita_startup_stdcall_return(c, 16U);
}

static void vita_startup_FreeLibrary(CPU *__restrict c)
{
    uint32_t module = vita_startup_arg(c, 0U);

    if (module != ISAAC_VITA_STARTUP_FREE_USERENV_MODULE) {
        guest_fault(c, module,
                    "FreeLibrary received an unexpected module token");
        return;
    }

    /* The measured save-path code unconditionally releases the result of the
     * failed USERENV load.  Win32 reports FreeLibrary(NULL) as FALSE. */
    vita_startup_set_last_error(
        c, ISAAC_VITA_STARTUP_ERROR_INVALID_HANDLE);
    c->eax = 0U;
    vita_startup_stdcall_return(c, 4U);
}

static void vita_startup_GetFileAttributesA(CPU *__restrict c)
{
    uint32_t path = vita_startup_arg(c, 0U);
    char native_path[ISAAC_VITA_STARTUP_PATH_MAX + 1U];
    struct stat information;

    if (!isaac_vita_startup_map_path(
            c, path, native_path, sizeof native_path))
        return;
#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
    /* Attribute probes must observe a save whose image is still queued. */
    isaac_vita_async_write_sync_path(native_path);
#endif
    if (stat(native_path, &information) != 0) {
        int native_error = errno;
        if (native_error == ENOENT) {
            vita_startup_set_last_error(
                c, vita_startup_missing_path_error(native_path));
        } else if (native_error == ENOTDIR) {
            vita_startup_set_last_error(c,
                ISAAC_VITA_STARTUP_ERROR_PATH_NOT_FOUND);
        } else if (native_error == EACCES || native_error == EPERM) {
            vita_startup_set_last_error(c,
                ISAAC_VITA_STARTUP_ERROR_ACCESS_DENIED);
        } else {
            isaac_vita_log(
                "GetFileAttributesA native stat failed: errno=%d path=%s",
                native_error, native_path);
            guest_fault(c, path,
                        "GetFileAttributesA received an unexpected native error");
            return;
        }
        c->eax = ISAAC_VITA_STARTUP_INVALID_FILE_ATTRIBUTES;
    } else if (S_ISDIR(information.st_mode)) {
        c->eax = ISAAC_VITA_STARTUP_FILE_ATTRIBUTE_DIRECTORY;
    } else {
        c->eax = ISAAC_VITA_STARTUP_FILE_ATTRIBUTE_ARCHIVE;
    }
    vita_startup_stdcall_return(c, 4U);
}

static void vita_startup_CreateDirectoryA(CPU *__restrict c)
{
    uint32_t path = vita_startup_arg(c, 0U);
    uint32_t security = vita_startup_arg(c, 1U);
    char native_path[ISAAC_VITA_STARTUP_PATH_MAX + 1U];

    if (security) {
        guest_fault(c, security,
                    "CreateDirectoryA received guest security attributes");
        return;
    }
    if (!isaac_vita_startup_map_path(
            c, path, native_path, sizeof native_path))
        return;
    if (mkdir(native_path, 0777) == 0) {
        c->eax = 1U;
    } else {
        int native_error = errno;
        if (native_error == EEXIST) {
            vita_startup_set_last_error(c,
                ISAAC_VITA_STARTUP_ERROR_ALREADY_EXISTS);
        } else if (native_error == ENOENT || native_error == ENOTDIR) {
            vita_startup_set_last_error(c,
                ISAAC_VITA_STARTUP_ERROR_PATH_NOT_FOUND);
        } else if (native_error == EACCES || native_error == EPERM) {
            vita_startup_set_last_error(c,
                ISAAC_VITA_STARTUP_ERROR_ACCESS_DENIED);
        } else {
            isaac_vita_log(
                "CreateDirectoryA native mkdir failed: errno=%d path=%s",
                native_error, native_path);
            guest_fault(c, path,
                        "CreateDirectoryA received an unexpected native error");
            return;
        }
        c->eax = 0U;
    }
    vita_startup_stdcall_return(c, 8U);
}

static void vita_startup_GetLastError(CPU *__restrict c)
{
    c->eax = c->last_error;
    vita_startup_stdcall_return(c, 0U);
}

static void vita_startup_GetCurrentDirectoryA(CPU *__restrict c)
{
    uint32_t capacity = vita_startup_arg(c, 0U);
    uint32_t output = vita_startup_arg(c, 1U);
    uint32_t length = (uint32_t)strlen(ISAAC_VITA_DATA_ROOT);

    if (capacity <= length) {
        c->eax = length + 1U;
        vita_startup_stdcall_return(c, 8U);
        return;
    }
    if (!vita_startup_range(
            c, output, length + 1U,
            "GetCurrentDirectoryA received an invalid output range"))
        return;
    memcpy((void *)(uintptr_t)output, ISAAC_VITA_DATA_ROOT, length + 1U);
    c->eax = length;
    vita_startup_stdcall_return(c, 8U);
}

static void vita_startup_GetFullPathNameW(CPU *__restrict c)
{
    uint32_t input = vita_startup_arg(c, 0U);
    uint32_t capacity = vita_startup_arg(c, 1U);
    uint32_t output = vita_startup_arg(c, 2U);
    uint32_t file_part_out = vita_startup_arg(c, 3U);
    char narrow[ISAAC_VITA_STARTUP_PATH_MAX + 1U];
    char guest_visible_path[ISAAC_VITA_STARTUP_PATH_MAX + 1U];
    uint32_t i;
    uint32_t length;
    uint32_t file_part = 0U;

    if (!input) {
        guest_fault(c, input, "GetFullPathNameW received a null path");
        return;
    }
    for (i = 0U; i <= ISAAC_VITA_STARTUP_PATH_MAX; ++i) {
        uint16_t value;
        if (input > UINT32_MAX - i * 2U) {
            guest_fault(c, input, "GetFullPathNameW path range overflow");
            return;
        }
        value = ld16(input + i * 2U);
        if (!value) {
            narrow[i] = '\0';
            break;
        }
        if (i == ISAAC_VITA_STARTUP_PATH_MAX || value > 0xffU) {
            guest_fault(c, input,
                        "GetFullPathNameW path exceeds C-locale byte/bound contract");
            return;
        }
        narrow[i] = (char)value;
    }
    if (!vita_startup_map_ascii(
            c, narrow, guest_visible_path, sizeof guest_visible_path,
            ISAAC_VITA_DATA_ROOT))
        return;
    length = (uint32_t)strlen(guest_visible_path);
    if (!capacity || capacity <= length) {
        c->eax = length + 1U;
        vita_startup_stdcall_return(c, 16U);
        return;
    }
    if (!vita_startup_range(
            c, output, (length + 1U) * 2U,
            "GetFullPathNameW received an invalid output range"))
        return;
    for (i = 0U; i <= length; ++i) {
        st16(output + i * 2U,
             (uint16_t)(uint8_t)guest_visible_path[i]);
        if (guest_visible_path[i] == '/')
            file_part = output + (i + 1U) * 2U;
    }
    if (file_part_out) {
        if (!vita_startup_range(
                c, file_part_out, 4U,
                "GetFullPathNameW received an invalid file-part output"))
            return;
        st32(file_part_out, file_part ? file_part : output);
    }
    c->eax = length;
    vita_startup_stdcall_return(c, 16U);
}

static void vita_startup_QueryPerformanceFrequency(CPU *__restrict c)
{
    uint32_t output = vita_startup_arg(c, 0U);

    if (!vita_startup_range(
            c, output, 8U,
            "QueryPerformanceFrequency received an invalid output range"))
        return;
    /* isaac_vita_get_process_time(), used by QPC, is measured in usec. */
    st32(output, ISAAC_VITA_STARTUP_QPF_FREQUENCY);
    st32(output + 4U, 0U);
    c->eax = 1U;
    vita_startup_stdcall_return(c, 4U);
}

static const vita_startup_import_entry s_vita_startup_imports[] = {
    { ISAAC_VITA_STARTUP_FREE_LIBRARY_NAME,
      vita_startup_FreeLibrary },
    { ISAAC_VITA_STARTUP_ENV_NAME,
      vita_startup_GetEnvironmentVariableA },
    { ISAAC_VITA_STARTUP_GET_LAST_ERROR_NAME,
      vita_startup_GetLastError },
    { ISAAC_VITA_STARTUP_SYSTEM_INFO_NAME,
      vita_startup_GetSystemInfo },
    { ISAAC_VITA_STARTUP_LOAD_LIBRARY_NAME,
      vita_startup_LoadLibraryA },
    { ISAAC_VITA_STARTUP_FILE_ATTRIBUTES_NAME,
      vita_startup_GetFileAttributesA },
    { ISAAC_VITA_STARTUP_CREATE_DIRECTORY_NAME,
      vita_startup_CreateDirectoryA },
    { ISAAC_VITA_STARTUP_CURRENT_DIRECTORY_NAME,
      vita_startup_GetCurrentDirectoryA },
    { ISAAC_VITA_STARTUP_FULL_PATH_NAME,
      vita_startup_GetFullPathNameW },
    { ISAAC_VITA_STARTUP_QPF_NAME,
      vita_startup_QueryPerformanceFrequency },
    { ISAAC_VITA_STARTUP_SYSTEM_PARAMETERS_NAME,
      vita_startup_SystemParametersInfoA }
};

_Static_assert(sizeof s_vita_startup_imports /
               sizeof s_vita_startup_imports[0] ==
               ISAAC_VITA_STARTUP_IMPORT_COUNT,
               "Vita startup import count drifted from measured batch");

const char *isaac_vita_startup_import_name(uint32_t index)
{
    return index < ISAAC_VITA_STARTUP_IMPORT_COUNT
        ? s_vita_startup_imports[index].name : NULL;
}

int isaac_vita_startup_import_indexed(CPU *__restrict c, uint32_t index,
                                      unsigned *call_count)
{
    if (index >= ISAAC_VITA_STARTUP_IMPORT_COUNT)
        return 0;
    if (call_count)
        ++*call_count;
    s_vita_startup_imports[index].fn(c);
    return 1;
}

static int vita_startup_dispatch(CPU *__restrict c, const char *name,
                                 unsigned *call_count)
{
    uint32_t i;

    for (i = 0U; i < ISAAC_VITA_STARTUP_IMPORT_COUNT; ++i) {
        if (strcmp(s_vita_startup_imports[i].name, name) == 0)
            return isaac_vita_startup_import_indexed(c, i, call_count);
    }
    return 0;
}

int isaac_vita_startup_import(CPU *__restrict c, const char *name)
{
    return vita_startup_dispatch(c, name, NULL);
}

int isaac_vita_startup_import_counted(CPU *__restrict c, const char *name,
                                      unsigned *call_count)
{
    return vita_startup_dispatch(c, name, call_count);
}
