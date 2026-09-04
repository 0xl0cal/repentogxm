#include <stdint.h>
#include <string.h>

#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>

#include "host_vita_find.h"
#include "host_vita_import_id.h"
#include "platform.h"
#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
#include "host_vita_async_write.h"
#endif

#define VITA_FIND_PATH_UNITS_MAX 1024U
#define VITA_FIND_PATH_UTF8_MAX  4096U
#define VITA_FIND_PATTERN_MAX    255U
#define VITA_FIND_TOKEN_MAGIC    0xf17d0000U
#define VITA_FIND_TOKEN_MASK     0xffff0000U
#define VITA_FIND_TOKEN_INDEX_MASK 0x0000000fU
#define VITA_FIND_TOKEN_GENERATION_MASK 0x00000fffU

#define VITA_FIND_ADVANCE_FAULT (-1)
#define VITA_FIND_ADVANCE_STALE (-2)

typedef void (*vita_find_import_fn)(CPU *__restrict);

typedef struct vita_find_import_entry {
    const char *name;
    vita_find_import_fn fn;
} vita_find_import_entry;

typedef struct vita_find_handle {
    SceUID descriptor;
    uint16_t generation;
    uint8_t in_use;
    uint8_t exhausted;
    char pattern[VITA_FIND_PATTERN_MAX + 1U];
} vita_find_handle;

static vita_find_handle s_vita_find_handles[ISAAC_VITA_FIND_HANDLE_CAPACITY];
static uint32_t s_vita_find_registry_lock;

static void vita_find_lock(void)
{
    while (__atomic_exchange_n(&s_vita_find_registry_lock, 1U,
                               __ATOMIC_ACQUIRE) != 0U) {
        /* Contention is not expected on the measured single-threaded boot,
         * but future engine threads must not burn a Vita core or starve the
         * descriptor owner while it is inside a synchronous I/O call. */
        (void)sceKernelDelayThread(1U);
    }
}

static void vita_find_unlock(void)
{
    __atomic_store_n(&s_vita_find_registry_lock, 0U, __ATOMIC_RELEASE);
}

static uint32_t vita_find_arg(CPU *__restrict c, uint32_t index)
{
    return ld32(guest_stack_address(
        c, c->esp + 4U + index * 4U, 4U, 0U));
}

static void vita_find_set_last_error(CPU *__restrict c, uint32_t error)
{
    c->last_error = error;
}

static void vita_find_stdcall_return(CPU *__restrict c,
                                     uint32_t argument_bytes)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, argument_bytes, 0U);
}

static int vita_find_range(CPU *__restrict c, uint32_t address,
                           uint32_t size, const char *what)
{
    if (size && (!address || address > UINT32_MAX - (size - 1U))) {
        guest_fault(c, address, what);
        return 0;
    }
    return 1;
}

static int vita_find_emit_utf8(CPU *__restrict c, uint32_t codepoint,
                               char *output, uint32_t capacity,
                               uint32_t *length)
{
    uint32_t need;

    if (codepoint <= 0x7fU)
        need = 1U;
    else if (codepoint <= 0x7ffU)
        need = 2U;
    else if (codepoint <= 0xffffU)
        need = 3U;
    else
        need = 4U;
    if (*length > capacity - need) {
        guest_fault(c, 0U, "FindFirstFileW path exceeds UTF-8 bound");
        return 0;
    }
    if (need == 1U) {
        output[(*length)++] = (char)codepoint;
    } else if (need == 2U) {
        output[(*length)++] = (char)(0xc0U | (codepoint >> 6));
        output[(*length)++] = (char)(0x80U | (codepoint & 0x3fU));
    } else if (need == 3U) {
        output[(*length)++] = (char)(0xe0U | (codepoint >> 12));
        output[(*length)++] =
            (char)(0x80U | ((codepoint >> 6) & 0x3fU));
        output[(*length)++] = (char)(0x80U | (codepoint & 0x3fU));
    } else {
        output[(*length)++] = (char)(0xf0U | (codepoint >> 18));
        output[(*length)++] =
            (char)(0x80U | ((codepoint >> 12) & 0x3fU));
        output[(*length)++] =
            (char)(0x80U | ((codepoint >> 6) & 0x3fU));
        output[(*length)++] = (char)(0x80U | (codepoint & 0x3fU));
    }
    return 1;
}

static int vita_find_decode_path(CPU *__restrict c, uint32_t address,
                                 char *output, uint32_t capacity)
{
    uint32_t input_index;
    uint32_t output_length = 0U;

    if (!address) {
        guest_fault(c, address, "FindFirstFileW received a null pattern");
        return 0;
    }
    for (input_index = 0U;
         input_index <= VITA_FIND_PATH_UNITS_MAX;
         ++input_index) {
        uint32_t offset = input_index * 2U;
        uint16_t unit;
        uint32_t codepoint;

        if (address > UINT32_MAX - offset - 1U) {
            guest_fault(c, address, "FindFirstFileW pattern range overflow");
            return 0;
        }
        unit = ld16(address + offset);
        if (!unit) {
            output[output_length] = '\0';
            return 1;
        }
        if (input_index == VITA_FIND_PATH_UNITS_MAX) {
            guest_fault(c, address,
                        "FindFirstFileW pattern exceeds UTF-16 bound");
            return 0;
        }
        if (unit >= 0xd800U && unit <= 0xdbffU) {
            uint16_t low;
            if (address > UINT32_MAX - offset - 3U) {
                guest_fault(c, address,
                            "FindFirstFileW pattern range overflow");
                return 0;
            }
            low = ld16(address + offset + 2U);
            if (low < 0xdc00U || low > 0xdfffU) {
                guest_fault(c, address,
                            "FindFirstFileW pattern has invalid UTF-16");
                return 0;
            }
            codepoint = 0x10000U +
                (((uint32_t)unit - 0xd800U) << 10) +
                ((uint32_t)low - 0xdc00U);
            ++input_index;
        } else if (unit >= 0xdc00U && unit <= 0xdfffU) {
            guest_fault(c, address,
                        "FindFirstFileW pattern has invalid UTF-16");
            return 0;
        } else {
            codepoint = unit;
        }
        if (!vita_find_emit_utf8(c, codepoint, output,
                                 capacity - 1U, &output_length))
            return 0;
    }
    return 0;
}

static int vita_find_component_equal(const char *component, size_t length,
                                     const char *literal)
{
    size_t literal_length = strlen(literal);
    return length == literal_length &&
           memcmp(component, literal, length) == 0;
}

static int vita_find_prepare_pattern(CPU *__restrict c, uint32_t guest_pattern,
                                     char *directory, uint32_t directory_size,
                                     char *pattern, uint32_t pattern_size)
{
    char decoded[VITA_FIND_PATH_UTF8_MAX + 1U];
    char normalized[VITA_FIND_PATH_UTF8_MAX + 1U];
    const char *suffix;
    const char *guest_root = ISAAC_VITA_DATA_ROOT;
    const char *native_root = ISAAC_VITA_NATIVE_DATA_ROOT;
    size_t guest_root_length = strlen(guest_root);
    size_t native_root_length = strlen(native_root);
    size_t decoded_length;
    size_t source;
    size_t output = 0U;
    char *last_separator;

    if (!vita_find_decode_path(c, guest_pattern, decoded, sizeof decoded))
        return 0;
    decoded_length = strlen(decoded);
    if (!decoded_length) {
        guest_fault(c, guest_pattern,
                    "FindFirstFileW received an empty pattern");
        return 0;
    }
    for (source = 0U; source <= decoded_length; ++source)
        if (decoded[source] == '\\')
            decoded[source] = '/';

    if (strncmp(decoded, guest_root, guest_root_length) == 0 &&
        (decoded[guest_root_length] == '\0' ||
         decoded[guest_root_length] == '/')) {
        suffix = decoded + guest_root_length;
    } else if (strncmp(decoded, native_root, native_root_length) == 0 &&
               (decoded[native_root_length] == '\0' ||
                decoded[native_root_length] == '/')) {
        suffix = decoded + native_root_length;
    } else {
        if (strchr(decoded, ':')) {
            guest_fault(c, guest_pattern,
                        "FindFirstFileW pattern is outside the data root");
            return 0;
        }
        suffix = decoded;
        while (suffix[0] == '.' && suffix[1] == '/')
            suffix += 2;
    }

    if (native_root_length >= sizeof normalized) {
        guest_fault(c, guest_pattern, "FindFirstFileW root exceeds bound");
        return 0;
    }
    memcpy(normalized, native_root, native_root_length);
    output = native_root_length;

    source = 0U;
    while (suffix[source]) {
        size_t begin;
        size_t length;

        while (suffix[source] == '/')
            ++source;
        if (!suffix[source])
            break;
        begin = source;
        while (suffix[source] && suffix[source] != '/')
            ++source;
        length = source - begin;
        if (vita_find_component_equal(suffix + begin, length, "."))
            continue;
        if (vita_find_component_equal(suffix + begin, length, "..")) {
            guest_fault(c, guest_pattern,
                        "FindFirstFileW pattern escapes the data root");
            return 0;
        }
        if (memchr(suffix + begin, ':', length)) {
            guest_fault(c, guest_pattern,
                        "FindFirstFileW pattern has an invalid component");
            return 0;
        }
        if (output + 1U + length >= sizeof normalized) {
            guest_fault(c, guest_pattern,
                        "FindFirstFileW normalized pattern exceeds bound");
            return 0;
        }
        normalized[output++] = '/';
        memcpy(normalized + output, suffix + begin, length);
        output += length;
    }
    normalized[output] = '\0';
    last_separator = strrchr(normalized, '/');
    if (!last_separator ||
        last_separator < normalized + native_root_length) {
        guest_fault(c, guest_pattern,
                    "FindFirstFileW pattern has no filename component");
        return 0;
    }
    if (!last_separator[1]) {
        guest_fault(c, guest_pattern,
                    "FindFirstFileW pattern has no filename component");
        return 0;
    }
    if (strchr(last_separator + 1, '/') ||
        memchr(normalized + native_root_length, '*',
               (size_t)(last_separator - normalized -
                        native_root_length)) ||
        memchr(normalized + native_root_length, '?',
               (size_t)(last_separator - normalized -
                        native_root_length))) {
        guest_fault(c, guest_pattern,
                    "FindFirstFileW wildcard appears before filename");
        return 0;
    }
    if ((size_t)(last_separator - normalized) >= directory_size ||
        strlen(last_separator + 1) >= pattern_size) {
        guest_fault(c, guest_pattern,
                    "FindFirstFileW directory or wildcard exceeds bound");
        return 0;
    }
    memcpy(directory, normalized,
           (size_t)(last_separator - normalized));
    directory[last_separator - normalized] = '\0';
    strcpy(pattern, last_separator + 1);
    return 1;
}

static unsigned char vita_find_fold(unsigned char value)
{
    if (value >= 'A' && value <= 'Z')
        return (unsigned char)(value + ('a' - 'A'));
    return value;
}

static int vita_find_wildcard_match(const char *pattern, const char *name)
{
    const char *star = NULL;
    const char *retry = NULL;

    /* Win32 treats *.* as the universal directory wildcard. */
    if (strcmp(pattern, "*.*") == 0)
        pattern = "*";
    while (*name) {
        if (*pattern == '?' ||
            (*pattern && *pattern != '*' &&
             vita_find_fold((unsigned char)*pattern) ==
             vita_find_fold((unsigned char)*name))) {
            ++pattern;
            ++name;
        } else if (*pattern == '*') {
            star = pattern++;
            retry = name;
        } else if (star) {
            pattern = star + 1;
            name = ++retry;
        } else {
            return 0;
        }
    }
    while (*pattern == '*')
        ++pattern;
    return *pattern == '\0';
}

static int vita_find_has_wildcard(const char *pattern)
{
    return strchr(pattern, '*') != NULL || strchr(pattern, '?') != NULL;
}

static int vita_find_join_exact_path(CPU *__restrict c,
                                     uint32_t guest_pattern,
                                     const char *directory,
                                     const char *name,
                                     char *path, uint32_t capacity)
{
    size_t directory_length = strlen(directory);
    size_t name_length = strlen(name);

    if (directory_length + 1U + name_length >= capacity) {
        guest_fault(c, guest_pattern,
                    "FindFirstFileW exact path exceeds bound");
        return 0;
    }
    memcpy(path, directory, directory_length);
    path[directory_length] = '/';
    memcpy(path + directory_length + 1U, name, name_length + 1U);
    return 1;
}

static int vita_find_utf8_to_utf16(CPU *__restrict c, const char *input,
                                   uint16_t *output, uint32_t capacity)
{
    uint32_t source = 0U;
    uint32_t target = 0U;

    while (input[source]) {
        uint32_t codepoint;
        uint32_t minimum;
        uint32_t continuation;
        uint8_t first = (uint8_t)input[source++];
        uint32_t i;

        if (first < 0x80U) {
            codepoint = first;
            minimum = 0U;
            continuation = 0U;
        } else if ((first & 0xe0U) == 0xc0U) {
            codepoint = first & 0x1fU;
            minimum = 0x80U;
            continuation = 1U;
        } else if ((first & 0xf0U) == 0xe0U) {
            codepoint = first & 0x0fU;
            minimum = 0x800U;
            continuation = 2U;
        } else if ((first & 0xf8U) == 0xf0U) {
            codepoint = first & 0x07U;
            minimum = 0x10000U;
            continuation = 3U;
        } else {
            guest_fault(c, 0U, "FindFile received an invalid UTF-8 name");
            return 0;
        }
        for (i = 0U; i < continuation; ++i) {
            uint8_t next = (uint8_t)input[source++];
            if (!next || (next & 0xc0U) != 0x80U) {
                guest_fault(c, 0U,
                            "FindFile received an invalid UTF-8 name");
                return 0;
            }
            codepoint = (codepoint << 6) | (next & 0x3fU);
        }
        if ((continuation && codepoint < minimum) ||
            codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            guest_fault(c, 0U, "FindFile received an invalid UTF-8 name");
            return 0;
        }
        if (codepoint <= 0xffffU) {
            if (target + 1U >= capacity) {
                guest_fault(c, 0U,
                            "FindFile name exceeds WIN32_FIND_DATAW");
                return 0;
            }
            output[target++] = (uint16_t)codepoint;
        } else {
            if (target + 2U >= capacity) {
                guest_fault(c, 0U,
                            "FindFile name exceeds WIN32_FIND_DATAW");
                return 0;
            }
            codepoint -= 0x10000U;
            output[target++] = (uint16_t)(0xd800U | (codepoint >> 10));
            output[target++] = (uint16_t)(0xdc00U | (codepoint & 0x3ffU));
        }
    }
    output[target] = 0U;
    return 1;
}

static int vita_find_is_leap(uint32_t year)
{
    return (year % 4U == 0U && year % 100U != 0U) ||
           year % 400U == 0U;
}

static uint64_t vita_find_days_before_year(uint32_t year)
{
    uint64_t previous = year - 1U;
    return previous * 365U + previous / 4U -
           previous / 100U + previous / 400U;
}

static uint64_t vita_find_filetime(const SceDateTime *time)
{
    static const uint8_t month_lengths[12] = {
        31U, 28U, 31U, 30U, 31U, 30U,
        31U, 31U, 30U, 31U, 30U, 31U
    };
    static const uint16_t month_starts[12] = {
        0U, 31U, 59U, 90U, 120U, 151U,
        181U, 212U, 243U, 273U, 304U, 334U
    };
    uint64_t days;
    uint64_t seconds;

    if (time->year < 1601U || time->year > 9999U ||
        time->month < 1U || time->month > 12U ||
        time->day < 1U ||
        time->day > (uint32_t)month_lengths[time->month - 1U] +
                    (uint32_t)(time->month == 2U &&
                               vita_find_is_leap(time->year)) ||
        time->hour > 23U || time->minute > 59U ||
        time->second > 60U || time->microsecond > 999999U)
        return 0U;
    days = vita_find_days_before_year(time->year) -
           vita_find_days_before_year(1601U);
    days += month_starts[time->month - 1U];
    if (time->month > 2U && vita_find_is_leap(time->year))
        ++days;
    days += time->day - 1U;
    seconds = ((days * 24U + time->hour) * 60U + time->minute) * 60U +
              (time->second == 60U ? 59U : time->second);
    return seconds * UINT64_C(10000000) +
           (uint64_t)time->microsecond * 10U;
}

static void vita_find_store_time(uint32_t output, uint32_t offset,
                                 const SceDateTime *time)
{
    uint64_t value = vita_find_filetime(time);
    st32(output + offset, (uint32_t)value);
    st32(output + offset + 4U, (uint32_t)(value >> 32));
}

static int vita_find_write_data(CPU *__restrict c, uint32_t output,
                                const char *name, const SceIoStat *status)
{
    uint16_t utf16[ISAAC_VITA_FIND_DATA_NAME_UNITS];
    uint32_t i;
    uint64_t size = 0U;
    uint32_t attributes;

    if (!vita_find_utf8_to_utf16(c, name, utf16,
                                 ISAAC_VITA_FIND_DATA_NAME_UNITS))
        return 0;
    memset((void *)(uintptr_t)output, 0, ISAAC_VITA_FIND_DATA_SIZE);
    if (status && SCE_S_ISDIR(status->st_mode))
        attributes = ISAAC_VITA_FIND_FILE_ATTRIBUTE_DIRECTORY;
    else
        attributes = ISAAC_VITA_FIND_FILE_ATTRIBUTE_ARCHIVE;
    st32(output + ISAAC_VITA_FIND_DATA_ATTRIBUTES_OFFSET, attributes);
    if (status) {
        vita_find_store_time(output,
                             ISAAC_VITA_FIND_DATA_CREATION_OFFSET,
                             &status->st_ctime);
        vita_find_store_time(output,
                             ISAAC_VITA_FIND_DATA_ACCESS_OFFSET,
                             &status->st_atime);
        vita_find_store_time(output,
                             ISAAC_VITA_FIND_DATA_WRITE_OFFSET,
                             &status->st_mtime);
        if (status->st_size > 0)
            size = (uint64_t)status->st_size;
    }
    st32(output + ISAAC_VITA_FIND_DATA_SIZE_HIGH_OFFSET,
         (uint32_t)(size >> 32));
    st32(output + ISAAC_VITA_FIND_DATA_SIZE_LOW_OFFSET, (uint32_t)size);
    for (i = 0U; i < ISAAC_VITA_FIND_DATA_NAME_UNITS; ++i) {
        st16(output + ISAAC_VITA_FIND_DATA_NAME_OFFSET + i * 2U,
             utf16[i]);
        if (!utf16[i])
            break;
    }
    /* cAlternateFileName remains the zeroed empty string.  The Vita has no
     * DOS 8.3 alias service, and inventing aliases would alter guest control
     * flow at the measured optional second conversion call. */
    return 1;
}

static uint32_t vita_find_token(uint32_t index, uint16_t generation)
{
    return VITA_FIND_TOKEN_MAGIC |
           (((uint32_t)generation &
             VITA_FIND_TOKEN_GENERATION_MASK) << 4) |
           index;
}

/* Registry helpers below require s_vita_find_registry_lock. */
static vita_find_handle *vita_find_lookup(uint32_t token)
{
    uint32_t index;
    uint32_t generation;
    vita_find_handle *handle;

    if ((token & VITA_FIND_TOKEN_MASK) != VITA_FIND_TOKEN_MAGIC)
        return NULL;
    index = token & VITA_FIND_TOKEN_INDEX_MASK;
    generation = (token >> 4) & VITA_FIND_TOKEN_GENERATION_MASK;
    if (index >= ISAAC_VITA_FIND_HANDLE_CAPACITY || !generation)
        return NULL;
    handle = &s_vita_find_handles[index];
    if (!handle->in_use ||
        ((uint32_t)handle->generation &
         VITA_FIND_TOKEN_GENERATION_MASK) != generation)
        return NULL;
    return handle;
}

static vita_find_handle *vita_find_reserve(uint32_t *token)
{
    uint32_t index;

    for (index = 0U; index < ISAAC_VITA_FIND_HANDLE_CAPACITY; ++index) {
        vita_find_handle *handle = &s_vita_find_handles[index];
        if (!handle->in_use) {
            uint16_t generation = (uint16_t)(
                ((uint32_t)handle->generation + 1U) &
                VITA_FIND_TOKEN_GENERATION_MASK);
            if (!generation)
                generation = 1U;
            memset(handle, 0, sizeof *handle);
            handle->descriptor = -1;
            handle->generation = generation;
            handle->in_use = 1U;
            *token = vita_find_token(index, generation);
            return handle;
        }
    }
    return NULL;
}

static void vita_find_release(vita_find_handle *handle)
{
    uint16_t generation = handle->generation;
    memset(handle, 0, sizeof *handle);
    handle->descriptor = -1;
    handle->generation = generation;
}

/* Returns 1 for an emitted entry, 0 only for normal EOF,
 * VITA_FIND_ADVANCE_STALE for a stale guest token, and
 * VITA_FIND_ADVANCE_FAULT after a loud fault.  Native iteration and registry
 * mutation stay under the lock; UTF-8 conversion and every guest_fault happen
 * after unlocking. */
static int vita_find_advance(CPU *__restrict c, uint32_t token,
                             uint32_t output)
{
    char name[sizeof ((SceIoDirent *)0)->d_name];
    SceIoStat status;
    vita_find_handle *handle;

    vita_find_lock();
    handle = vita_find_lookup(token);
    if (!handle) {
        vita_find_unlock();
        return VITA_FIND_ADVANCE_STALE;
    }
    if (handle->exhausted) {
        vita_find_unlock();
        return 0;
    }
    for (;;) {
        SceIoDirent entry;
        int result;

        memset(&entry, 0, sizeof entry);
        result = sceIoDread(handle->descriptor, &entry);
        if (result < 0) {
            SceUID descriptor = handle->descriptor;
            vita_find_unlock();
            isaac_vita_log(
                "Vita FindFile sceIoDread failed: fd=%d result=0x%08x",
                (int)descriptor, (unsigned)result);
            guest_fault(c, (uint32_t)result,
                        "FindFile native directory read failed");
            return VITA_FIND_ADVANCE_FAULT;
        }
        if (result == 0) {
            handle->exhausted = 1U;
            vita_find_unlock();
            return 0;
        }
        if (!memchr(entry.d_name, '\0', sizeof entry.d_name)) {
            vita_find_unlock();
            guest_fault(c, 0U,
                        "FindFile received an unterminated Vita name");
            return VITA_FIND_ADVANCE_FAULT;
        }
        /* Win32 supplies these itself; ignore a backend that also does. */
        if (strcmp(entry.d_name, ".") == 0 ||
            strcmp(entry.d_name, "..") == 0)
            continue;
        if (!vita_find_wildcard_match(handle->pattern, entry.d_name))
            continue;
        strcpy(name, entry.d_name);
        status = entry.d_stat;
        break;
    }
    vita_find_unlock();
    return vita_find_write_data(c, output, name, &status) ?
        1 : VITA_FIND_ADVANCE_FAULT;
}

static int vita_find_exact(CPU *__restrict c, uint32_t guest_pattern,
                           uint32_t output, const char *directory,
                           const char *name)
{
    char path[VITA_FIND_PATH_UTF8_MAX + 1U];
    SceIoStat status;
    int result;
    vita_find_handle *handle;
    uint32_t token;

    if (!vita_find_join_exact_path(c, guest_pattern, directory, name,
                                   path, sizeof path))
        return VITA_FIND_ADVANCE_FAULT;
    memset(&status, 0, sizeof status);
    result = sceIoGetstat(path, &status);
    if ((uint32_t)result == ISAAC_VITA_FIND_NATIVE_ERROR_NOT_DIRECTORY) {
        vita_find_set_last_error(c, ISAAC_VITA_FIND_ERROR_PATH_NOT_FOUND);
        c->eax = ISAAC_VITA_FIND_INVALID_HANDLE_VALUE;
        return 0;
    }
    if ((uint32_t)result == ISAAC_VITA_FIND_NATIVE_ERROR_NOT_FOUND) {
        SceIoStat directory_status;
        int directory_result;

        memset(&directory_status, 0, sizeof directory_status);
        directory_result = sceIoGetstat(directory, &directory_status);
        if (directory_result >= 0 &&
            SCE_S_ISDIR(directory_status.st_mode)) {
            vita_find_set_last_error(
                c, ISAAC_VITA_FIND_ERROR_FILE_NOT_FOUND);
        } else if ((uint32_t)directory_result ==
                       ISAAC_VITA_FIND_NATIVE_ERROR_NOT_FOUND ||
                   (uint32_t)directory_result ==
                       ISAAC_VITA_FIND_NATIVE_ERROR_NOT_DIRECTORY ||
                   directory_result >= 0) {
            vita_find_set_last_error(
                c, ISAAC_VITA_FIND_ERROR_PATH_NOT_FOUND);
        } else {
            isaac_vita_log(
                "Vita FindFile exact parent stat failed: result=0x%08x path=%s",
                (unsigned)directory_result, directory);
            guest_fault(c, (uint32_t)directory_result,
                        "FindFirstFileW exact parent stat failed");
            return VITA_FIND_ADVANCE_FAULT;
        }
        c->eax = ISAAC_VITA_FIND_INVALID_HANDLE_VALUE;
        return 0;
    }
    if (result < 0) {
        isaac_vita_log(
            "Vita FindFile exact stat failed: result=0x%08x path=%s",
            (unsigned)result, path);
        guest_fault(c, (uint32_t)result,
                    "FindFirstFileW exact path stat failed");
        return VITA_FIND_ADVANCE_FAULT;
    }

    vita_find_lock();
    handle = vita_find_reserve(&token);
    if (handle) {
        handle->descriptor = -1;
        handle->exhausted = 1U;
    }
    vita_find_unlock();
    if (!handle) {
        guest_fault(c, 0U, "Vita FindFile handle registry exhausted");
        return VITA_FIND_ADVANCE_FAULT;
    }
    if (!vita_find_write_data(c, output, name, &status)) {
        vita_find_lock();
        handle = vita_find_lookup(token);
        if (handle)
            vita_find_release(handle);
        vita_find_unlock();
        return VITA_FIND_ADVANCE_FAULT;
    }
    c->eax = token;
    return 1;
}

static void vita_find_FindFirstFileW(CPU *__restrict c)
{
    uint32_t guest_pattern = vita_find_arg(c, 0U);
    uint32_t output = vita_find_arg(c, 1U);
    char directory[VITA_FIND_PATH_UTF8_MAX + 1U];
    char pattern[VITA_FIND_PATTERN_MAX + 1U];
    vita_find_handle *handle;
    uint32_t token;
    SceUID descriptor;
    int result;

    if (!vita_find_range(c, output, ISAAC_VITA_FIND_DATA_SIZE,
                         "FindFirstFileW received an invalid output range"))
        return;
    if (!vita_find_prepare_pattern(c, guest_pattern,
                                   directory, sizeof directory,
                                   pattern, sizeof pattern))
        return;
#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
    /* A directory scan must list every save whose image is still queued.
     * The queue is empty in steady state, so this returns immediately. */
    isaac_vita_async_write_drain();
#endif
    if (!vita_find_has_wildcard(pattern)) {
        result = vita_find_exact(c, guest_pattern, output,
                                 directory, pattern);
        if (result != VITA_FIND_ADVANCE_FAULT)
            vita_find_stdcall_return(c, 8U);
        return;
    }
    descriptor = sceIoDopen(directory);
    if ((uint32_t)descriptor == ISAAC_VITA_FIND_NATIVE_ERROR_NOT_FOUND ||
        (uint32_t)descriptor == ISAAC_VITA_FIND_NATIVE_ERROR_NOT_DIRECTORY) {
        vita_find_set_last_error(
            c, ISAAC_VITA_FIND_ERROR_PATH_NOT_FOUND);
        c->eax = ISAAC_VITA_FIND_INVALID_HANDLE_VALUE;
        vita_find_stdcall_return(c, 8U);
        return;
    }
    if (descriptor < 0) {
        isaac_vita_log(
            "Vita FindFile sceIoDopen failed: result=0x%08x path=%s",
            (unsigned)descriptor, directory);
        guest_fault(c, (uint32_t)descriptor,
                    "FindFirstFileW native directory open failed");
        return;
    }
    vita_find_lock();
    handle = vita_find_reserve(&token);
    if (!handle) {
        int close_result;
        vita_find_unlock();
        close_result = sceIoDclose(descriptor);
        if (close_result < 0)
            isaac_vita_log(
                "Vita FindFile registry-full Dclose failed: fd=%d result=0x%08x",
                (int)descriptor, (unsigned)close_result);
        if (close_result < 0) {
            guest_fault(c, (uint32_t)close_result,
                        "FindFirstFileW registry cleanup close failed");
            return;
        }
        guest_fault(c, 0U, "Vita FindFile handle registry exhausted");
        return;
    }
    strcpy(handle->pattern, pattern);
    handle->descriptor = descriptor;
    vita_find_unlock();
    result = vita_find_advance(c, token, output);
    if (result == VITA_FIND_ADVANCE_STALE) {
        guest_fault(c, token,
                    "FindFirstFileW lost its internal search handle");
        return;
    }
    if (result == VITA_FIND_ADVANCE_FAULT)
        return;
    if (!result) {
        int close_result;
        vita_find_lock();
        handle = vita_find_lookup(token);
        if (!handle) {
            vita_find_unlock();
            guest_fault(c, token,
                        "FindFirstFileW lost its exhausted search handle");
            return;
        }
        close_result = sceIoDclose(handle->descriptor);
        if (close_result >= 0)
            vita_find_release(handle);
        vita_find_unlock();
        if (close_result < 0) {
            isaac_vita_log(
                "Vita FindFile empty-result Dclose failed: token=0x%08x result=0x%08x",
                (unsigned)token, (unsigned)close_result);
            guest_fault(c, (uint32_t)close_result,
                        "FindFirstFileW native directory close failed");
            return;
        }
        vita_find_set_last_error(
            c, ISAAC_VITA_FIND_ERROR_FILE_NOT_FOUND);
        c->eax = ISAAC_VITA_FIND_INVALID_HANDLE_VALUE;
    } else {
        c->eax = token;
    }
    vita_find_stdcall_return(c, 8U);
}

static void vita_find_FindNextFileW(CPU *__restrict c)
{
    uint32_t token = vita_find_arg(c, 0U);
    uint32_t output = vita_find_arg(c, 1U);
    int result;

    if (!vita_find_range(c, output, ISAAC_VITA_FIND_DATA_SIZE,
                         "FindNextFileW received an invalid output range"))
        return;
    result = vita_find_advance(c, token, output);
    if (result == VITA_FIND_ADVANCE_STALE) {
        vita_find_set_last_error(
            c, ISAAC_VITA_FIND_ERROR_INVALID_HANDLE);
        c->eax = 0U;
        vita_find_stdcall_return(c, 8U);
        return;
    }
    if (result == VITA_FIND_ADVANCE_FAULT)
        return;
    if (!result)
        vita_find_set_last_error(
            c, ISAAC_VITA_FIND_ERROR_NO_MORE_FILES);
    c->eax = result ? 1U : 0U;
    vita_find_stdcall_return(c, 8U);
}

static void vita_find_FindClose(CPU *__restrict c)
{
    uint32_t token = vita_find_arg(c, 0U);
    vita_find_handle *handle;
    int result;

    vita_find_lock();
    handle = vita_find_lookup(token);
    if (!handle) {
        vita_find_unlock();
        vita_find_set_last_error(
            c, ISAAC_VITA_FIND_ERROR_INVALID_HANDLE);
        c->eax = 0U;
        vita_find_stdcall_return(c, 4U);
        return;
    }
    result = handle->descriptor < 0 ? 0 : sceIoDclose(handle->descriptor);
    if (result >= 0)
        vita_find_release(handle);
    vita_find_unlock();
    if (result < 0) {
        isaac_vita_log(
            "Vita FindFile sceIoDclose failed: token=0x%08x result=0x%08x",
            (unsigned)token, (unsigned)result);
        guest_fault(c, (uint32_t)result,
                    "FindClose native directory close failed");
        return;
    }
    c->eax = 1U;
    vita_find_stdcall_return(c, 4U);
}

static const vita_find_import_entry s_vita_find_imports[] = {
    { ISAAC_VITA_FIND_CLOSE_NAME, vita_find_FindClose },
    { ISAAC_VITA_FIND_FIRST_NAME, vita_find_FindFirstFileW },
    { ISAAC_VITA_FIND_NEXT_NAME, vita_find_FindNextFileW }
};

_Static_assert(sizeof s_vita_find_imports /
               sizeof s_vita_find_imports[0] ==
               ISAAC_VITA_FIND_IMPORT_COUNT,
               "Vita FindFile import count drifted");
_Static_assert(ISAAC_VITA_FIND_DATA_NAME_OFFSET +
               ISAAC_VITA_FIND_DATA_NAME_UNITS * 2U ==
               ISAAC_VITA_FIND_DATA_ALT_NAME_OFFSET,
               "WIN32_FIND_DATAW filename offset drifted");
_Static_assert(ISAAC_VITA_FIND_DATA_ALT_NAME_OFFSET +
               ISAAC_VITA_FIND_DATA_ALT_NAME_UNITS * 2U ==
               ISAAC_VITA_FIND_DATA_SIZE,
               "WIN32_FIND_DATAW size drifted");
_Static_assert(ISAAC_VITA_FIND_HANDLE_CAPACITY <=
               VITA_FIND_TOKEN_INDEX_MASK + 1U,
               "Vita FindFile handle token has too few index bits");

const char *isaac_vita_find_import_name(uint32_t index)
{
    return index < ISAAC_VITA_FIND_IMPORT_COUNT
        ? s_vita_find_imports[index].name : NULL;
}

int isaac_vita_find_import_indexed(CPU *__restrict c, uint32_t index,
                                   unsigned *call_count)
{
    if (index >= ISAAC_VITA_FIND_IMPORT_COUNT)
        return 0;
    if (call_count)
        ++*call_count;
    s_vita_find_imports[index].fn(c);
    return 1;
}

static int vita_find_dispatch(CPU *__restrict c, const char *name,
                              unsigned *call_count)
{
    uint32_t i;

    for (i = 0U; i < ISAAC_VITA_FIND_IMPORT_COUNT; ++i) {
        if (strcmp(s_vita_find_imports[i].name, name) == 0)
            return isaac_vita_find_import_indexed(c, i, call_count);
    }
    return 0;
}

int isaac_vita_find_import(CPU *__restrict c, const char *name)
{
    return vita_find_dispatch(c, name, NULL);
}

int isaac_vita_find_import_counted(CPU *__restrict c, const char *name,
                                   unsigned *call_count)
{
    return vita_find_dispatch(c, name, call_count);
}
