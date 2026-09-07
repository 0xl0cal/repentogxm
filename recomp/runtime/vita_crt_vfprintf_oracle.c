/* Executable host oracle and link-only ARM oracle for the allocation-free
 * Vita vfprintf sink.  The host build is non-PIE so its static guest data is
 * addressable through the production 32-bit identity-mapped ld/st helpers.
 */
#include <errno.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest.h"

static size_t oracle_fwrite(const void *buffer, size_t size, size_t count,
                            FILE *stream);
static int oracle_fflush(FILE *stream);
static int oracle_fclose(FILE *stream);
static void oracle_after_first_pass(CPU *cpu, uint32_t token,
                                    uint32_t format, uint32_t arguments,
                                    uint32_t length);

#define ISAAC_VITA_CRT_QSORT_HOST_ORACLE 1
#define ISAAC_VITA_CRT_VFPRINTF_AFTER_FIRST_PASS(                    \
    cpu, token, format, arguments, length)                            \
    oracle_after_first_pass((cpu), (token), (format), (arguments),   \
                            (length))
#define fwrite oracle_fwrite
#define fflush oracle_fflush
#define fclose oracle_fclose
#if defined(ISAAC_VITA_CRT_ATOF_SMALLINT_TEST)
static double oracle_strtod(const char *text, char **end);
#define strtod oracle_strtod
#endif
#include "host_vita_crt.c"
#if defined(ISAAC_VITA_CRT_ATOF_SMALLINT_TEST)
#undef strtod
#endif
#undef fclose
#undef fflush
#undef fwrite
#undef ISAAC_VITA_CRT_VFPRINTF_AFTER_FIRST_PASS

/* Keep Windows' RPC macros out of the production CRT/musl inclusion. */
#if defined(ISAAC_VITA_ROOM_LOG_MARKERS_TEST)
# if defined(_WIN32)
#  include <windows.h>
# else
#  include <sys/mman.h>
# endif
#endif

enum {
    ORACLE_TOKEN = 0x7f123456U,
    ORACLE_TOKEN_2 = 0x7f123457U,
    ORACLE_RETURN = 0xaabbccddU,
    ORACLE_OUTPUT_CAPACITY = 160000U,
    ORACLE_LONG_CAPACITY = 140000U,
    ORACLE_GUEST_ERRNO_SENTINEL = 0x1357,
    ORACLE_HOST_ERRNO_SENTINEL = EACCES
};

enum oracle_hook_mode {
    ORACLE_HOOK_NONE,
    ORACLE_HOOK_CLOSE_TOKEN,
    ORACLE_HOOK_MUTATE
};

_Alignas(16) static uint32_t s_guest_stack[128];
_Alignas(16) static uint32_t s_arguments[16];
_Alignas(16) static unsigned char s_output[ORACLE_OUTPUT_CAPACITY];
_Alignas(16) static unsigned char s_expected[ORACLE_OUTPUT_CAPACITY];
_Alignas(16) static unsigned char s_vsprintf_storage[96];
_Alignas(16) static char s_long_source[ORACLE_LONG_CAPACITY];
_Alignas(16) static char s_long_format[ORACLE_LONG_CAPACITY];
static char s_percent_s[] = "%s";
static char s_empty_format[] = "";
static char s_bad_format[] = "%q";
static char s_numeric_format[] =
    "d=%d u=%u x=%08x q=%llu h=%016llx s=%.16s f=%.2f %%";
static char s_numeric_string[] = "abcdefghijklmnop-tail";
static char s_float_format[] = "%f|%g|%.f|%.1f|%.2f|%.4f";
static char s_vsprintf_float_format[] =
    "Framebuffer Width: %d Multiplier: %f";
static char s_vsprintf_float_expected[] =
    "Framebuffer Width: 960 Multiplier: 2.250010";
static char s_mutable_format[ORACLE_LONG_CAPACITY];

static FILE *const s_fake_stream = (FILE *)(uintptr_t)0x12340000U;
static FILE *const s_fake_stream_2 = (FILE *)(uintptr_t)0x12340010U;
static FILE *s_expected_stream;
static uint32_t s_output_used;
static unsigned s_fwrite_calls;
static unsigned s_bad_fwrite_call;
static unsigned s_fail_call;
static uint32_t s_fail_accept;
static int s_fail_errno;
static int s_fail_sets_errno;
static int s_success_errno;
static unsigned s_mutate_fwrite_call;
static char *s_fwrite_mutate_first;
static char s_fwrite_mutate_first_value;
static char *s_fwrite_mutate_second;
static char s_fwrite_mutate_second_value;
static enum oracle_hook_mode s_hook_mode;
static char *s_hook_mutate_first;
static char s_hook_mutate_first_value;
static char *s_hook_mutate_second;
static char s_hook_mutate_second_value;
static unsigned s_hook_calls;
static uint32_t s_hook_length;
static unsigned s_unexpected_calls;
static unsigned s_fflush_calls;
static unsigned s_fclose_calls;
static unsigned s_native_fail_call;
static int s_native_fail_errno;
static FILE *s_last_flush_stream;

static uint32_t pointer32(const void *pointer)
{
    return (uint32_t)(uintptr_t)pointer;
}

static int pointer_fits(const void *pointer)
{
    return (uintptr_t)pointer <= UINT32_MAX;
}

void guest_fault(CPU *__restrict cpu, uint32_t address, const char *what)
{
    if (!cpu->fault) {
        cpu->fault = what;
        cpu->fault_addr = address;
    }
}

/* Sanitizers retain more externally visible sections from the included
 * production translation unit than an ordinary --gc-sections link.  These
 * fail-observable stubs satisfy those cold sections without weakening the
 * focused oracle: none may be reached by vfprintf. */
void guest_call(CPU *__restrict cpu, uint32_t address)
{
    ++s_unexpected_calls;
    guest_fault(cpu, address, "unexpected guest_call in vfprintf oracle");
}

void guest_exit(CPU *__restrict cpu, int32_t code, const char *api)
{
    (void)cpu;
    (void)code;
    (void)api;
    ++s_unexpected_calls;
}

void *isaac_vita_guest_malloc(size_t size)
{
    (void)size;
    ++s_unexpected_calls;
    return NULL;
}

int isaac_vita_guest_free(void *pointer)
{
    (void)pointer;
    ++s_unexpected_calls;
    return 0;
}

int isaac_vita_guest_heap_terminal(void)
{
    ++s_unexpected_calls;
    return 1;
}

int isaac_vita_startup_map_path(CPU *__restrict cpu, uint32_t guest_path,
                                char *native_path, uint32_t capacity)
{
    (void)cpu;
    (void)guest_path;
    (void)native_path;
    (void)capacity;
    ++s_unexpected_calls;
    return 0;
}

int isaac_vita_get_win32_filetime(uint64_t *filetime)
{
    (void)filetime;
    ++s_unexpected_calls;
    return 0;
}

void isaac_vita_log(const char *format, ...)
{
    (void)format;
    ++s_unexpected_calls;
}

unsigned int sceRtcGetTickResolution(void)
{
    ++s_unexpected_calls;
    return 1U;
}

int sceRtcConvertUtcToLocalTime(const SceRtcTick *utc,
                                SceRtcTick *local_time)
{
    (void)utc;
    (void)local_time;
    ++s_unexpected_calls;
    return -1;
}

int sceRtcSetTime64_t(SceDateTime *time, SceUInt64 value)
{
    (void)time;
    (void)value;
    ++s_unexpected_calls;
    return -1;
}

int sceRtcSetTick(SceDateTime *time, const SceRtcTick *tick)
{
    (void)time;
    (void)tick;
    ++s_unexpected_calls;
    return -1;
}

int sceRtcGetTick(const SceDateTime *time, SceRtcTick *tick)
{
    (void)time;
    (void)tick;
    ++s_unexpected_calls;
    return -1;
}

static void oracle_mutate(char *first, char first_value,
                          char *second, char second_value)
{
    if (first)
        *first = first_value;
    if (second)
        *second = second_value;
}

static void oracle_after_first_pass(CPU *cpu, uint32_t token,
                                    uint32_t format, uint32_t arguments,
                                    uint32_t length)
{
    (void)cpu;
    (void)token;
    (void)format;
    (void)arguments;
    ++s_hook_calls;
    s_hook_length = length;
    if (s_hook_mode == ORACLE_HOOK_CLOSE_TOKEN) {
        s_file_tokens[0].stream = NULL;
    } else if (s_hook_mode == ORACLE_HOOK_MUTATE) {
        oracle_mutate(s_hook_mutate_first, s_hook_mutate_first_value,
                      s_hook_mutate_second, s_hook_mutate_second_value);
    }
}

static size_t oracle_fwrite(const void *buffer, size_t size, size_t count,
                            FILE *stream)
{
    uint32_t accepted = (uint32_t)count;

    ++s_fwrite_calls;
    if (size != 1U || count > VITA_CRT_VFPRINTF_CHUNK_BYTES ||
        stream != s_expected_stream || s_file_lock != 1U ||
        count > ORACLE_OUTPUT_CAPACITY - s_output_used) {
        ++s_bad_fwrite_call;
        errno = EFAULT;
        return 0U;
    }
    if (s_fail_call == s_fwrite_calls && accepted > s_fail_accept)
        accepted = s_fail_accept;
    memcpy(s_output + s_output_used, buffer, accepted);
    s_output_used += accepted;
    if (s_mutate_fwrite_call == s_fwrite_calls)
        oracle_mutate(s_fwrite_mutate_first,
                      s_fwrite_mutate_first_value,
                      s_fwrite_mutate_second,
                      s_fwrite_mutate_second_value);
    if (s_fail_call == s_fwrite_calls) {
        if (s_fail_sets_errno)
            errno = s_fail_errno;
        return accepted;
    }
    if (s_success_errno)
        errno = s_success_errno;
    return count;
}

static int oracle_fflush(FILE *stream)
{
    ++s_fflush_calls;
    s_last_flush_stream = stream;
    if (s_file_lock != 1U ||
        (stream && stream != s_fake_stream && stream != s_fake_stream_2)) {
        ++s_unexpected_calls;
        errno = EFAULT;
        return EOF;
    }
    if (s_native_fail_call == s_fflush_calls) {
        errno = s_native_fail_errno;
        return EOF;
    }
    return 0;
}

static int oracle_fclose(FILE *stream)
{
    ++s_fclose_calls;
    if (s_file_lock != 1U ||
        (stream != s_fake_stream && stream != s_fake_stream_2)) {
        ++s_unexpected_calls;
        errno = EFAULT;
        return EOF;
    }
    if (s_native_fail_call == s_fclose_calls) {
        errno = s_native_fail_errno;
        return EOF;
    }
    return 0;
}

static void oracle_reset(void)
{
    memset(s_file_tokens, 0, sizeof s_file_tokens);
    s_file_tokens[0].stream = s_fake_stream;
    s_file_tokens[0].token = ORACLE_TOKEN;
    s_file_tokens[1].stream = s_fake_stream_2;
    s_file_tokens[1].token = ORACLE_TOKEN_2;
    s_expected_stream = s_fake_stream;
    s_file_lock = 0U;
    memset(s_output, 0xcc, sizeof s_output);
    s_output_used = 0U;
    s_fwrite_calls = 0U;
    s_bad_fwrite_call = 0U;
    s_fail_call = 0U;
    s_fail_accept = 0U;
    s_fail_errno = 0;
    s_fail_sets_errno = 1;
    s_success_errno = 0;
    s_mutate_fwrite_call = 0U;
    s_fwrite_mutate_first = NULL;
    s_fwrite_mutate_second = NULL;
    s_hook_mode = ORACLE_HOOK_NONE;
    s_hook_mutate_first = NULL;
    s_hook_mutate_second = NULL;
    s_hook_calls = 0U;
    s_hook_length = UINT32_MAX;
    s_unexpected_calls = 0U;
    s_fflush_calls = 0U;
    s_fclose_calls = 0U;
    s_native_fail_call = 0U;
    s_native_fail_errno = EIO;
    s_last_flush_stream = NULL;
#if defined(ISAAC_VITA_GAME_LOG_BATCH)
    memset(&s_log_batch, 0, sizeof s_log_batch);
    s_log_batch.abi_version = ISAAC_VITA_CRT_LOG_BATCH_ABI;
    s_log_batch.enabled = 1U;
    s_log_batch_token = 0U;
#endif
    g_isaac_vita_crt_errno = ORACLE_GUEST_ERRNO_SENTINEL;
}

static uint32_t oracle_prepare_call(CPU *cpu, uint32_t token,
                                    const char *format,
                                    const uint32_t *arguments)
{
    uint32_t esp = pointer32(&s_guest_stack[48]);

    memset(cpu, 0, sizeof *cpu);
    memset(s_guest_stack, 0xcc, sizeof s_guest_stack);
    st32(esp, ORACLE_RETURN);
    st32(esp + 4U, 0U);
    st32(esp + 8U, 0U);
    st32(esp + 12U, token);
    st32(esp + 16U, pointer32(format));
    st32(esp + 20U, 0U);
    st32(esp + 24U, pointer32(arguments));
    cpu->esp = esp;
    cpu->stack_owner = cpu;
    cpu->stack_floor = pointer32(&s_guest_stack[0]);
    cpu->stack_ceiling = pointer32(&s_guest_stack[128]);
    cpu->stack_low_water = cpu->stack_ceiling;
    cpu->eax = 0x5aa55aa5U;
    return esp;
}

static int oracle_frame_unchanged(uint32_t esp, uint32_t token,
                                  const char *format,
                                  const uint32_t *arguments)
{
    return ld32(esp) == ORACLE_RETURN && ld32(esp + 4U) == 0U &&
           ld32(esp + 8U) == 0U && ld32(esp + 12U) == token &&
           ld32(esp + 16U) == pointer32(format) &&
           ld32(esp + 20U) == 0U &&
           ld32(esp + 24U) == pointer32(arguments);
}

static uint32_t oracle_call(CPU *cpu, uint32_t token, const char *format,
                            const uint32_t *arguments)
{
    uint32_t esp = oracle_prepare_call(cpu, token, format, arguments);

    errno = ORACLE_HOST_ERRNO_SENTINEL;
    vita_crt_stdio_common_vfprintf(cpu);
    if (!oracle_frame_unchanged(esp, token, format, arguments))
        ++s_bad_fwrite_call;
    return esp;
}

static int oracle_success(const char *format, const uint32_t *arguments,
                          const unsigned char *expected, uint32_t length)
{
    CPU cpu;
    uint32_t esp;
    uint32_t expected_calls =
        (length + VITA_CRT_VFPRINTF_CHUNK_BYTES - 1U) /
        VITA_CRT_VFPRINTF_CHUNK_BYTES;

    oracle_reset();
    esp = oracle_call(&cpu, ORACLE_TOKEN, format, arguments);
    return !cpu.fault && cpu.esp == esp + 4U && cpu.eax == length &&
           errno == ORACLE_HOST_ERRNO_SENTINEL &&
           g_isaac_vita_crt_errno == ORACLE_GUEST_ERRNO_SENTINEL &&
           s_file_lock == 0U && !s_bad_fwrite_call &&
           !s_unexpected_calls &&
           s_hook_calls == 1U && s_hook_length == length &&
           s_fwrite_calls == expected_calls && s_output_used == length &&
           memcmp(s_output, expected, length) == 0;
}

static int oracle_vsprintf_float(uint32_t count)
{
    enum { OUTPUT_OFFSET = 8 };
    CPU cpu;
    double x87_before[8];
    unsigned char *buffer = s_vsprintf_storage + OUTPUT_OFFSET;
    uint32_t length = (uint32_t)strlen(s_vsprintf_float_expected);
    uint32_t copied = count ? (length >= count ? count - 1U : length) : 0U;
    uint32_t esp = pointer32(&s_guest_stack[48]);
    uint32_t index;
    uint64_t raw = UINT64_C(0x4002000540000000);

    if (count > sizeof s_vsprintf_storage - OUTPUT_OFFSET)
        return 0;
    oracle_reset();
    memset(s_vsprintf_storage, 0xa5, sizeof s_vsprintf_storage);
    memset(s_arguments, 0, sizeof s_arguments);
    s_arguments[0] = 960U;
    s_arguments[1] = (uint32_t)raw;
    s_arguments[2] = (uint32_t)(raw >> 32);

    memset(&cpu, 0, sizeof cpu);
    memset(s_guest_stack, 0xcc, sizeof s_guest_stack);
    st32(esp, ORACLE_RETURN);
    st32(esp + 4U, 2U); /* standard snprintf-compatible truncation */
    st32(esp + 8U, 0U);
    st32(esp + 12U, pointer32(buffer));
    st32(esp + 16U, count);
    st32(esp + 20U, pointer32(s_vsprintf_float_format));
    st32(esp + 24U, 0U);
    st32(esp + 28U, pointer32(s_arguments));
    cpu.esp = esp;
    cpu.stack_owner = &cpu;
    cpu.stack_floor = pointer32(&s_guest_stack[0]);
    cpu.stack_ceiling = pointer32(&s_guest_stack[128]);
    cpu.stack_low_water = cpu.stack_ceiling;
    for (index = 0U; index < 8U; ++index)
        cpu.st[index] = 1000.25 + (double)index;
    memcpy(x87_before, cpu.st, sizeof x87_before);
    cpu.st_top = 5;
    cpu.fsw = UINT16_C(0x4100);
    cpu.eax = UINT32_C(0x5aa55aa5);

    errno = ORACLE_HOST_ERRNO_SENTINEL;
    vita_crt_stdio_common_vsprintf(&cpu);
    if (cpu.fault || cpu.esp != esp + 4U || cpu.eax != length ||
        errno != ORACLE_HOST_ERRNO_SENTINEL ||
        g_isaac_vita_crt_errno != ORACLE_GUEST_ERRNO_SENTINEL ||
        cpu.st_top != 5 || cpu.fsw != UINT16_C(0x4100) ||
        memcmp(cpu.st, x87_before, sizeof x87_before) != 0 ||
        ld32(esp) != ORACLE_RETURN || ld32(esp + 4U) != 2U ||
        ld32(esp + 8U) != 0U ||
        ld32(esp + 12U) != pointer32(buffer) ||
        ld32(esp + 16U) != count ||
        ld32(esp + 20U) != pointer32(s_vsprintf_float_format) ||
        ld32(esp + 24U) != 0U ||
        ld32(esp + 28U) != pointer32(s_arguments) ||
        memcmp(buffer, s_vsprintf_float_expected, copied) != 0 ||
        (count && buffer[copied] != '\0') || s_fwrite_calls != 0U ||
        s_unexpected_calls != 0U)
        return 0;
    for (index = 0U; index < OUTPUT_OFFSET; ++index) {
        if (s_vsprintf_storage[index] != 0xa5)
            return 0;
    }
    for (index = OUTPUT_OFFSET + copied + (count ? 1U : 0U);
         index < sizeof s_vsprintf_storage; ++index) {
        if (s_vsprintf_storage[index] != 0xa5)
            return 0;
    }
    return 1;
}

static uint32_t oracle_random(uint32_t *state)
{
    uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static int oracle_numeric_differential(void)
{
    static char formats[][8] = {
        "%d", "%u", "%x", "%X", "%08x", "%02u", "%03d", "%4d"
    };
    uint32_t state = 0x2599c0U;
    uint32_t index;

    for (index = 0U; index < 4096U; ++index) {
        uint32_t value = oracle_random(&state);
        uint32_t which = oracle_random(&state) % 8U;
        int length;

        s_arguments[0] = value;
        if (which == 0U || which == 6U || which == 7U)
            length = snprintf((char *)s_expected, sizeof s_expected,
                              formats[which], (int32_t)value);
        else
            length = snprintf((char *)s_expected, sizeof s_expected,
                              formats[which], value);
        if (length < 0 ||
            !oracle_success(formats[which], s_arguments, s_expected,
                            (uint32_t)length))
            return 0;
    }
    return 1;
}

#if defined(ISAAC_VITA_GAME_LOG_BATCH)
static uint32_t oracle_prepare_flush_call(CPU *cpu, uint32_t token,
                                          uint32_t adapter_return,
                                          uint32_t logger_return,
                                          uint32_t severity)
{
    uint32_t esp = pointer32(&s_guest_stack[48]);
    uint32_t ebp = pointer32(&s_guest_stack[80]);

    memset(cpu, 0, sizeof *cpu);
    memset(s_guest_stack, 0xcc, sizeof s_guest_stack);
    st32(esp, adapter_return);
    st32(esp + 4U, token);
    st32(esp + 8U, logger_return);
    st32(ebp + 8U, severity);
    cpu->esp = esp;
    cpu->ebp = ebp;
    cpu->stack_owner = cpu;
    cpu->stack_floor = pointer32(&s_guest_stack[0]);
    cpu->stack_ceiling = pointer32(&s_guest_stack[128]);
    cpu->stack_low_water = cpu->stack_ceiling;
    cpu->eax = 0x5aa55aa5U;
    return esp;
}

static uint32_t oracle_log_flush(CPU *cpu, uint32_t token,
                                 uint32_t adapter_return,
                                 uint32_t logger_return,
                                 uint32_t severity)
{
    uint32_t esp = oracle_prepare_flush_call(
        cpu, token, adapter_return, logger_return, severity);

    errno = ORACLE_HOST_ERRNO_SENTINEL;
    vita_crt_fflush(cpu);
    return esp;
}

static uint32_t oracle_close(CPU *cpu, uint32_t token)
{
    uint32_t esp = oracle_prepare_flush_call(
        cpu, token, ORACLE_RETURN, 0U, 0U);

    errno = ORACLE_HOST_ERRNO_SENTINEL;
    vita_crt_fclose(cpu);
    return esp;
}

static int oracle_log_batch_snapshot(
    isaac_vita_crt_log_batch_snapshot *snapshot)
{
    return isaac_vita_crt_log_batch_get_snapshot(snapshot) == 1 &&
           snapshot->abi_version == ISAAC_VITA_CRT_LOG_BATCH_ABI &&
           snapshot->enabled == 1U;
}

static int oracle_log_batch(void)
{
    CPU cpu;
    isaac_vita_crt_log_batch_snapshot snapshot;
    uint32_t esp;
    uint32_t index;

#define LOG_CHECK(condition)                                               \
    do {                                                                   \
        if (!(condition)) {                                                \
            fprintf(stderr, "log-batch oracle failed at line %d\n",      \
                    __LINE__);                                             \
            return __LINE__;                                               \
        }                                                                  \
    } while (0)

    oracle_reset();
    LOG_CHECK(oracle_log_batch_snapshot(&snapshot) &&
              snapshot.pending_info == 0U &&
              snapshot.sticky_fail_open == 0U);

    /* At every possible crash point before the batch boundary, no more than
     * seven INFO records are pending and libc fflush has not run. */
    for (index = 1U; index < ISAAC_VITA_CRT_LOG_BATCH_SIZE; ++index) {
        esp = oracle_log_flush(
            &cpu, ORACLE_TOKEN,
            ISAAC_VITA_CRT_FFLUSH_FIRST_RETURN_RVA,
            ISAAC_VITA_CRT_FFLUSH_FIRST_ORIGIN_RETURN_RVA, 0U);
        LOG_CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax == 0U &&
                  errno == ORACLE_HOST_ERRNO_SENTINEL &&
                  g_isaac_vita_crt_errno == ORACLE_GUEST_ERRNO_SENTINEL &&
                  s_fflush_calls == 0U && s_file_lock == 0U &&
                  oracle_log_batch_snapshot(&snapshot) &&
                  snapshot.pending_info == index &&
                  snapshot.max_deferred == index);
    }
    esp = oracle_log_flush(
        &cpu, ORACLE_TOKEN,
        ISAAC_VITA_CRT_FFLUSH_FIRST_RETURN_RVA,
        ISAAC_VITA_CRT_FFLUSH_FIRST_ORIGIN_RETURN_RVA, 0U);
    LOG_CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax == 0U &&
              s_fflush_calls == 1U && s_last_flush_stream == s_fake_stream &&
              oracle_log_batch_snapshot(&snapshot) &&
              snapshot.info_seen == 8U && snapshot.info_deferred == 7U &&
              snapshot.info_batch_flushes == 1U &&
              snapshot.native_flush_calls == 1U &&
              snapshot.pending_info == 0U && snapshot.max_deferred == 7U);

    /* A high-severity record flushes itself and every earlier INFO record. */
    oracle_reset();
    for (index = 0U; index < 3U; ++index)
        (void)oracle_log_flush(
            &cpu, ORACLE_TOKEN,
            ISAAC_VITA_CRT_FFLUSH_FIRST_RETURN_RVA,
            ISAAC_VITA_CRT_FFLUSH_FIRST_ORIGIN_RETURN_RVA, 0U);
    esp = oracle_log_flush(
        &cpu, ORACLE_TOKEN,
        ISAAC_VITA_CRT_FFLUSH_FIRST_RETURN_RVA,
        ISAAC_VITA_CRT_FFLUSH_FIRST_ORIGIN_RETURN_RVA, 1U);
    LOG_CHECK(!cpu.fault && cpu.esp == esp + 4U && s_fflush_calls == 1U &&
              oracle_log_batch_snapshot(&snapshot) &&
              snapshot.warn_forwarded == 1U &&
              snapshot.pending_info == 0U);
    (void)oracle_log_flush(
        &cpu, ORACLE_TOKEN,
        ISAAC_VITA_CRT_FFLUSH_FIRST_RETURN_RVA,
        ISAAC_VITA_CRT_FFLUSH_FIRST_ORIGIN_RETURN_RVA, 2U);
    (void)oracle_log_flush(
        &cpu, ORACLE_TOKEN,
        ISAAC_VITA_CRT_FFLUSH_FIRST_RETURN_RVA,
        ISAAC_VITA_CRT_FFLUSH_FIRST_ORIGIN_RETURN_RVA, 7U);
    LOG_CHECK(oracle_log_batch_snapshot(&snapshot) &&
              snapshot.error_forwarded == 1U &&
              snapshot.assert_forwarded == 1U && s_fflush_calls == 3U);

    /* An unrelated fflush is untouched.  Once the exact adapter return is
     * seen, malformed parent/frame/token evidence is counted and forwarded. */
    oracle_reset();
    esp = oracle_log_flush(
        &cpu, ORACLE_TOKEN, ORACLE_RETURN,
        ISAAC_VITA_CRT_FFLUSH_FIRST_ORIGIN_RETURN_RVA, 0U);
    LOG_CHECK(!cpu.fault && cpu.esp == esp + 4U && s_fflush_calls == 1U &&
              oracle_log_batch_snapshot(&snapshot) &&
              snapshot.chain_rejects == 0U && snapshot.info_seen == 0U);
    esp = oracle_log_flush(
        &cpu, ORACLE_TOKEN,
        ISAAC_VITA_CRT_FFLUSH_FIRST_RETURN_RVA, ORACLE_RETURN, 0U);
    LOG_CHECK(!cpu.fault && cpu.esp == esp + 4U && s_fflush_calls == 2U &&
              oracle_log_batch_snapshot(&snapshot) &&
              snapshot.chain_rejects == 1U);
    esp = oracle_prepare_flush_call(
        &cpu, ORACLE_TOKEN,
        ISAAC_VITA_CRT_FFLUSH_FIRST_RETURN_RVA,
        ISAAC_VITA_CRT_FFLUSH_FIRST_ORIGIN_RETURN_RVA, 0U);
    cpu.ebp = UINT32_MAX;
    errno = ORACLE_HOST_ERRNO_SENTINEL;
    vita_crt_fflush(&cpu);
    LOG_CHECK(!cpu.fault && cpu.esp == esp + 4U && s_fflush_calls == 3U &&
              oracle_log_batch_snapshot(&snapshot) &&
              snapshot.chain_rejects == 2U);

    oracle_reset();
    (void)oracle_log_flush(
        &cpu, ORACLE_TOKEN,
        ISAAC_VITA_CRT_FFLUSH_FIRST_RETURN_RVA,
        ISAAC_VITA_CRT_FFLUSH_FIRST_ORIGIN_RETURN_RVA, 0U);
    esp = oracle_log_flush(
        &cpu, ORACLE_TOKEN_2,
        ISAAC_VITA_CRT_FFLUSH_FIRST_RETURN_RVA,
        ISAAC_VITA_CRT_FFLUSH_FIRST_ORIGIN_RETURN_RVA, 0U);
    LOG_CHECK(!cpu.fault && cpu.esp == esp + 4U &&
              s_last_flush_stream == s_fake_stream_2 &&
              oracle_log_batch_snapshot(&snapshot) &&
              snapshot.chain_rejects == 1U &&
              snapshot.pending_info == 1U);

    oracle_reset();
    esp = oracle_log_flush(
        &cpu, 0xdeadbeefU,
        ISAAC_VITA_CRT_FFLUSH_FIRST_RETURN_RVA,
        ISAAC_VITA_CRT_FFLUSH_FIRST_ORIGIN_RETURN_RVA, 0U);
    LOG_CHECK(cpu.fault && cpu.esp == esp && s_fflush_calls == 0U &&
              s_file_lock == 0U);

    /* A native error enables sticky fail-open.  Even a later successful
     * flush cannot silently re-enable suppression in this process. */
    oracle_reset();
    for (index = 1U; index < ISAAC_VITA_CRT_LOG_BATCH_SIZE; ++index)
        (void)oracle_log_flush(
            &cpu, ORACLE_TOKEN,
            ISAAC_VITA_CRT_FFLUSH_FIRST_RETURN_RVA,
            ISAAC_VITA_CRT_FFLUSH_FIRST_ORIGIN_RETURN_RVA, 0U);
    s_native_fail_call = 1U;
    s_native_fail_errno = ENOSPC;
    esp = oracle_log_flush(
        &cpu, ORACLE_TOKEN,
        ISAAC_VITA_CRT_FFLUSH_FIRST_RETURN_RVA,
        ISAAC_VITA_CRT_FFLUSH_FIRST_ORIGIN_RETURN_RVA, 0U);
    LOG_CHECK(!cpu.fault && cpu.esp == esp + 4U &&
              cpu.eax == UINT32_MAX && errno == ORACLE_HOST_ERRNO_SENTINEL &&
              g_isaac_vita_crt_errno == ENOSPC &&
              oracle_log_batch_snapshot(&snapshot) &&
              snapshot.native_failures == 1U &&
              snapshot.sticky_fail_open == 1U &&
              snapshot.pending_info == 8U);
    s_native_fail_call = 0U;
    esp = oracle_log_flush(
        &cpu, ORACLE_TOKEN,
        ISAAC_VITA_CRT_FFLUSH_FIRST_RETURN_RVA,
        ISAAC_VITA_CRT_FFLUSH_FIRST_ORIGIN_RETURN_RVA, 0U);
    LOG_CHECK(!cpu.fault && cpu.esp == esp + 4U && s_fflush_calls == 2U &&
              oracle_log_batch_snapshot(&snapshot) &&
              snapshot.sticky_fail_open == 1U &&
              snapshot.pending_info == 0U);
    (void)oracle_log_flush(
        &cpu, ORACLE_TOKEN,
        ISAAC_VITA_CRT_FFLUSH_FIRST_RETURN_RVA,
        ISAAC_VITA_CRT_FFLUSH_FIRST_ORIGIN_RETURN_RVA, 0U);
    LOG_CHECK(s_fflush_calls == 3U &&
              oracle_log_batch_snapshot(&snapshot) &&
              snapshot.info_deferred == 7U);

    /* Normal all-stream flush and fclose remain durability boundaries. */
    oracle_reset();
    (void)oracle_log_flush(
        &cpu, ORACLE_TOKEN,
        ISAAC_VITA_CRT_FFLUSH_FIRST_RETURN_RVA,
        ISAAC_VITA_CRT_FFLUSH_FIRST_ORIGIN_RETURN_RVA, 0U);
    errno = ORACLE_HOST_ERRNO_SENTINEL;
    LOG_CHECK(isaac_vita_crt_flush_all() == 0 &&
              errno == ORACLE_HOST_ERRNO_SENTINEL &&
              s_last_flush_stream == NULL &&
              oracle_log_batch_snapshot(&snapshot) &&
              snapshot.pending_info == 0U);
    (void)oracle_log_flush(
        &cpu, ORACLE_TOKEN,
        ISAAC_VITA_CRT_FFLUSH_FIRST_RETURN_RVA,
        ISAAC_VITA_CRT_FFLUSH_FIRST_ORIGIN_RETURN_RVA, 0U);
    esp = oracle_close(&cpu, ORACLE_TOKEN);
    LOG_CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax == 0U &&
              s_fclose_calls == 1U && s_file_tokens[0].stream == NULL &&
              oracle_log_batch_snapshot(&snapshot) &&
              snapshot.pending_info == 0U);

    printf("Vita CRT exact logger INFO batch oracle: PASS "
           "(N=8, high severity, hostile chain, sticky failure, exit/close)\n");
    return 0;
#undef LOG_CHECK
}
#endif

static int run_oracle(void)
{
    static const uint32_t boundaries[] = {
        1U, 2U, 511U, 512U, 513U, 1023U, 1024U, 1025U,
        65535U, 65536U, 65537U, 131071U
    };
    CPU cpu;
    uint32_t esp;
    uint32_t index;
    uint64_t integer;
    uint64_t raw_double;
    double real = -1234.25;
    int expected_length;

#define CHECK(condition)                                                   \
    do {                                                                   \
        if (!(condition)) {                                                \
            fprintf(stderr, "vfprintf oracle failed at line %d\n",        \
                    __LINE__);                                             \
            return __LINE__;                                               \
        }                                                                  \
    } while (0)

    CHECK(pointer_fits(s_guest_stack) && pointer_fits(s_arguments) &&
          pointer_fits(s_output) && pointer_fits(s_expected) &&
          pointer_fits(s_long_source) && pointer_fits(s_long_format) &&
          pointer_fits(s_percent_s) && pointer_fits(s_empty_format) &&
          pointer_fits(s_bad_format) && pointer_fits(s_numeric_format) &&
          pointer_fits(s_numeric_string) && pointer_fits(s_float_format) &&
          pointer_fits(s_vsprintf_storage) &&
          pointer_fits(s_vsprintf_float_format) &&
          pointer_fits(s_mutable_format));
    CHECK(setlocale(LC_ALL, "C") != NULL);
    memset(s_arguments, 0, sizeof s_arguments);
    CHECK(oracle_success(s_empty_format, s_arguments,
                         (const unsigned char *)s_empty_format, 0U));

    for (index = 0U; index < sizeof boundaries / sizeof boundaries[0];
         ++index) {
        uint32_t length = boundaries[index];
        uint32_t byte;

        for (byte = 0U; byte < length; ++byte)
            s_long_source[byte] = (char)('a' + byte % 23U);
        s_long_source[length] = '\0';
        s_arguments[0] = pointer32(s_long_source);
        CHECK(oracle_success(s_percent_s, s_arguments,
                             (const unsigned char *)s_long_source, length));

        for (byte = 0U; byte < length; ++byte)
            s_long_format[byte] = (char)('A' + byte % 23U);
        s_long_format[length] = '\0';
        CHECK(oracle_success(s_long_format, s_arguments,
                             (const unsigned char *)s_long_format, length));
    }

    /* Forty-eight frozen callers obtain __acrt_iob_func(2), so exercise the
     * standard-stream token path with more than one bounded chunk.  fwrite is
     * intercepted above; this never writes to the host oracle's stderr. */
    memset(s_long_source, 'e', 1300U);
    s_long_source[1300] = '\0';
    s_arguments[0] = pointer32(s_long_source);
    oracle_reset();
    s_expected_stream = stderr;
    esp = oracle_call(
        &cpu, (uint32_t)(uintptr_t)stderr, s_percent_s, s_arguments);
    CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax == 1300U &&
          s_output_used == 1300U &&
          s_fwrite_calls ==
              (1300U + VITA_CRT_VFPRINTF_CHUNK_BYTES - 1U) /
                  VITA_CRT_VFPRINTF_CHUNK_BYTES &&
          memcmp(s_output, s_long_source, 1300U) == 0 &&
          !s_bad_fwrite_call && s_file_lock == 0U &&
          errno == ORACLE_HOST_ERRNO_SENTINEL &&
          g_isaac_vita_crt_errno == ORACLE_GUEST_ERRNO_SENTINEL);

    memset(s_arguments, 0, sizeof s_arguments);
    s_arguments[0] = UINT32_C(0x80000000);
    s_arguments[1] = UINT32_MAX;
    s_arguments[2] = UINT32_C(0x1234abcd);
    integer = UINT64_C(0xfedcba9876543210);
    s_arguments[3] = (uint32_t)integer;
    s_arguments[4] = (uint32_t)(integer >> 32);
    integer = UINT64_C(0x0123456789abcdef);
    s_arguments[5] = (uint32_t)integer;
    s_arguments[6] = (uint32_t)(integer >> 32);
    s_arguments[7] = pointer32(s_numeric_string);
    memcpy(&raw_double, &real, sizeof raw_double);
    s_arguments[8] = (uint32_t)raw_double;
    s_arguments[9] = (uint32_t)(raw_double >> 32);
    expected_length = snprintf(
        (char *)s_expected, sizeof s_expected, s_numeric_format,
        INT32_MIN, UINT32_MAX, UINT32_C(0x1234abcd),
        (unsigned long long)UINT64_C(0xfedcba9876543210),
        (unsigned long long)UINT64_C(0x0123456789abcdef),
        s_numeric_string, real);
    CHECK(expected_length > 0 &&
          oracle_success(s_numeric_format, s_arguments, s_expected,
                         (uint32_t)expected_length));

    raw_double = UINT64_C(0x4002000540000000);
    memcpy(&real, &raw_double, sizeof real);
    for (index = 0U; index < 6U; ++index) {
        s_arguments[index * 2U] = (uint32_t)raw_double;
        s_arguments[index * 2U + 1U] = (uint32_t)(raw_double >> 32);
    }
    expected_length = snprintf(
        (char *)s_expected, sizeof s_expected, s_float_format,
        real, real, real, real, real, real);
    CHECK(expected_length > 0 &&
          memcmp(s_expected, "2.250010|", 9U) == 0 &&
          oracle_success(s_float_format, s_arguments, s_expected,
                         (uint32_t)expected_length));
    CHECK(oracle_numeric_differential());
    CHECK(oracle_vsprintf_float(64U));
    CHECK(oracle_vsprintf_float(16U));
    CHECK(oracle_vsprintf_float(0U));

    /* Same-length mutation retains the predecessor's behavior: the second
     * pass writes the changed bytes because only the total length is stable. */
    strcpy(s_mutable_format, "abc");
    oracle_reset();
    s_hook_mode = ORACLE_HOOK_MUTATE;
    s_hook_mutate_first = &s_mutable_format[0];
    s_hook_mutate_first_value = 'Z';
    esp = oracle_call(&cpu, ORACLE_TOKEN, s_mutable_format, s_arguments);
    CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax == 3U &&
          s_output_used == 3U && memcmp(s_output, "Zbc", 3U) == 0 &&
          s_file_lock == 0U && errno == ORACLE_HOST_ERRNO_SENTINEL &&
          g_isaac_vita_crt_errno == ORACLE_GUEST_ERRNO_SENTINEL);

    /* A pre-stream length mutation is rejected before the pending short
     * chunk is flushed. */
    strcpy(s_mutable_format, "abcd");
    oracle_reset();
    s_hook_mode = ORACLE_HOOK_MUTATE;
    s_hook_mutate_first = &s_mutable_format[2];
    s_hook_mutate_first_value = '\0';
    esp = oracle_call(&cpu, ORACLE_TOKEN, s_mutable_format, s_arguments);
    CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax == UINT32_MAX &&
          !s_output_used && !s_fwrite_calls && s_file_lock == 0U &&
          errno == ORACLE_HOST_ERRNO_SENTINEL &&
          g_isaac_vita_crt_errno == EINVAL);

    /* Token close between the two passes faults with RET still live and no
     * registry lock retained. */
    strcpy(s_mutable_format, "token");
    oracle_reset();
    s_hook_mode = ORACLE_HOOK_CLOSE_TOKEN;
    esp = oracle_call(&cpu, ORACLE_TOKEN, s_mutable_format, s_arguments);
    CHECK(cpu.fault &&
          strcmp(cpu.fault,
                 "vfprintf FILE token closed while formatting") == 0 &&
          cpu.fault_addr == ORACLE_TOKEN && cpu.esp == esp &&
          !s_output_used && !s_fwrite_calls && s_file_lock == 0U &&
          errno == ORACLE_HOST_ERRNO_SENTINEL &&
          g_isaac_vita_crt_errno == ORACLE_GUEST_ERRNO_SENTINEL);

    oracle_reset();
    esp = oracle_call(&cpu, ORACLE_TOKEN, s_bad_format, s_arguments);
    CHECK(cpu.fault &&
          strcmp(cpu.fault, "guest stdio unsupported format") == 0 &&
          cpu.fault_addr == pointer32(s_bad_format) && cpu.esp == esp &&
          !s_hook_calls && !s_fwrite_calls && s_file_lock == 0U);

    oracle_reset();
    s_file_tokens[0].stream = NULL;
    esp = oracle_call(&cpu, ORACLE_TOKEN, s_bad_format, s_arguments);
    CHECK(cpu.fault &&
          strcmp(cpu.fault,
                 "vfprintf received an unknown FILE token") == 0 &&
          cpu.fault_addr == ORACLE_TOKEN && cpu.esp == esp &&
          !s_hook_calls && !s_fwrite_calls && s_file_lock == 0U);

    /* Unsupported foreign mutation after one flushed chunk may retain that
     * prefix, but it is an ordinary EINVAL return and always unlocks. */
    memset(s_mutable_format, 'm', 700U);
    s_mutable_format[700] = '\0';
    oracle_reset();
    s_mutate_fwrite_call = 1U;
    s_fwrite_mutate_first = &s_mutable_format[600];
    s_fwrite_mutate_first_value = '%';
    s_fwrite_mutate_second = &s_mutable_format[601];
    s_fwrite_mutate_second_value = 'q';
    esp = oracle_call(&cpu, ORACLE_TOKEN, s_mutable_format, s_arguments);
    CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax == UINT32_MAX &&
          s_output_used == VITA_CRT_VFPRINTF_CHUNK_BYTES &&
          s_fwrite_calls == 1U && s_file_lock == 0U &&
          errno == ORACLE_HOST_ERRNO_SENTINEL &&
          g_isaac_vita_crt_errno == EINVAL);

    /* A later string contraction takes the same cleanup path and discards
     * the not-yet-flushed tail. */
    memset(s_long_source, 's', 900U);
    s_long_source[900] = '\0';
    s_arguments[0] = pointer32(s_long_source);
    oracle_reset();
    s_mutate_fwrite_call = 1U;
    s_fwrite_mutate_first = &s_long_source[700];
    s_fwrite_mutate_first_value = '\0';
    esp = oracle_call(&cpu, ORACLE_TOKEN, s_percent_s, s_arguments);
    CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax == UINT32_MAX &&
          s_output_used == VITA_CRT_VFPRINTF_CHUNK_BYTES &&
          s_fwrite_calls == 1U && s_file_lock == 0U &&
          g_isaac_vita_crt_errno == EINVAL);

    memset(s_long_source, 'w', 1300U);
    s_long_source[1300] = '\0';
    s_arguments[0] = pointer32(s_long_source);
    oracle_reset();
    s_fail_call = 1U;
    s_fail_accept = 117U;
    s_fail_errno = ENOSPC;
    esp = oracle_call(&cpu, ORACLE_TOKEN, s_percent_s, s_arguments);
    CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax == UINT32_MAX &&
          s_output_used == 117U && s_fwrite_calls == 1U &&
          !s_bad_fwrite_call && s_file_lock == 0U &&
          errno == ORACLE_HOST_ERRNO_SENTINEL &&
          g_isaac_vita_crt_errno == ENOSPC);

    oracle_reset();
    s_fail_call = 2U;
    s_fail_accept = 31U;
    s_fail_errno = EFBIG;
    esp = oracle_call(&cpu, ORACLE_TOKEN, s_percent_s, s_arguments);
    CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax == UINT32_MAX &&
          s_output_used == VITA_CRT_VFPRINTF_CHUNK_BYTES + 31U &&
          s_fwrite_calls == 2U && s_file_lock == 0U &&
          g_isaac_vita_crt_errno == EFBIG);

    oracle_reset();
    s_fail_call = 1U;
    s_fail_accept = 0U;
    s_fail_errno = 0;
    esp = oracle_call(&cpu, ORACLE_TOKEN, s_percent_s, s_arguments);
    CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax == UINT32_MAX &&
          !s_output_used && s_fwrite_calls == 1U && s_file_lock == 0U &&
          g_isaac_vita_crt_errno == EIO);

    /* A successful chunk is permitted to leave errno nonzero.  It must not
     * be misattributed to a later short write which leaves errno untouched. */
    oracle_reset();
    s_success_errno = EAGAIN;
    s_fail_call = 2U;
    s_fail_accept = 0U;
    s_fail_sets_errno = 0;
    esp = oracle_call(&cpu, ORACLE_TOKEN, s_percent_s, s_arguments);
    CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax == UINT32_MAX &&
          s_output_used == VITA_CRT_VFPRINTF_CHUNK_BYTES &&
          s_fwrite_calls == 2U && s_file_lock == 0U &&
          g_isaac_vita_crt_errno == EIO);

    oracle_reset();
    s_success_errno = EAGAIN;
    esp = oracle_call(&cpu, ORACLE_TOKEN, s_percent_s, s_arguments);
    CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax == 1300U &&
          s_output_used == 1300U && s_file_lock == 0U &&
          errno == ORACLE_HOST_ERRNO_SENTINEL &&
          g_isaac_vita_crt_errno == ORACLE_GUEST_ERRNO_SENTINEL);

    printf("Vita CRT vfprintf bounded/no-direct-allocation oracle: PASS "
           "(4096 differential cases; 131071-byte output; mutation, token "
           "race and short-write cleanup)\n");
    return 0;
#undef CHECK
}

#if defined(ISAAC_VITA_ROOM_LOG_MARKERS_TEST)
static unsigned s_room_calls, s_room_callback_bad;
static CPU *s_room_cpu;
static uint32_t s_room_expected_esp, s_room_expected_buffer, s_room_expected_length;
static const char s_room_expected[] = "Room -2147483648.-1(Start)\n";
static char s_room_name[] = "Start";
#if defined(ISAAC_VITA_ROOM_LOG_MARKERS) && ISAAC_VITA_ROOM_LOG_MARKERS
void kage_vita_phase_profile_room_log(uint32_t first, uint32_t second)
{
    ++s_room_calls;
    if (first != UINT32_C(0x80000000) || second != UINT32_MAX ||
        !s_room_cpu || s_room_cpu->esp != s_room_expected_esp ||
        s_room_cpu->eax != s_room_expected_length ||
        memcmp((void *)(uintptr_t)s_room_expected_buffer, s_room_expected,
               sizeof s_room_expected) != 0)
        ++s_room_callback_bad;
    errno = EDOM; /* the CRT observer must preserve the caller's errno */
}
#endif

static void *oracle_room_map(uint32_t address)
{
#if defined(_WIN32)
    return VirtualAlloc((void *)(uintptr_t)address, 65536u,
                        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *p = mmap((void *)(uintptr_t)address, 65536u, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

static int oracle_room_log(void)
{
    unsigned mode;
    void *format_map = oracle_room_map(UINT32_C(0x98740000));
    void *buffer_map = oracle_room_map(UINT32_C(0x98800000));
#define ROOM_CHECK(x) do { if (!(x)) { fprintf(stderr, "Room CRT check failed line %d mode %u\n", __LINE__, mode); return __LINE__; } } while (0)
    mode = 0u;
    ROOM_CHECK(format_map == (void *)(uintptr_t)UINT32_C(0x98740000) &&
               buffer_map == (void *)(uintptr_t)UINT32_C(0x98800000));
    for (mode = 0u; mode < 13u; ++mode) {
        CPU cpu, expected;
        uint32_t esp = pointer32(&s_guest_stack[16]);
        uint32_t frame = pointer32(&s_guest_stack[80]);
        uint32_t args = frame + 16u;
        uint32_t buffer = ISAAC_VITA_CRT_ROOM_LOG_BUFFER_VA + 9u;
        uint32_t count = ISAAC_VITA_CRT_ROOM_LOG_BUFFER_END - buffer;
        uint32_t format = ISAAC_VITA_CRT_ROOM_LOG_FORMAT_VA;
        uint32_t length = sizeof s_room_expected - 1u;
        unsigned calls = s_room_calls;
        memset(&cpu, 0, sizeof cpu);
        memset(s_guest_stack, 0xcc, sizeof s_guest_stack);
        memset(buffer_map, 0xa5, 65536u);
        memcpy((void *)(uintptr_t)format, ISAAC_VITA_CRT_ROOM_LOG_FORMAT,
               sizeof ISAAC_VITA_CRT_ROOM_LOG_FORMAT);
        st32(frame + 4u, ISAAC_VITA_CRT_ROOM_LOG_ORIGIN_RETURN_RVA);
        st32(frame + 8u, 0u); st32(frame + 12u, format);
        st32(args, UINT32_C(0x80000000)); st32(args + 4u, UINT32_MAX);
        st32(args + 8u, pointer32(s_room_name));
        st32(esp, ISAAC_VITA_CRT_VSPRINTF_TIMER_RETURN_RVA);
        st32(esp + 4u, 2u); st32(esp + 8u, 0u);
        if (mode == 1u) st32(frame + 4u, 0x003b1112u);
        if (mode == 2u) st32(esp, 0x0055e45bu);
        if (mode == 3u) st32(frame + 8u, 1u);
        if (mode == 4u) st32(frame + 12u, format + 1u);
        if (mode == 5u) { --count; } /* complete but not exact owner capacity */
        if (mode == 6u) { buffer = ISAAC_VITA_CRT_ROOM_LOG_BUFFER_END - 4u; count = 4u; }
        if (mode == 7u) { st8(format + sizeof ISAAC_VITA_CRT_ROOM_LOG_FORMAT - 2u, '!'); }
        if (mode == 8u) { st8(format + sizeof ISAAC_VITA_CRT_ROOM_LOG_FORMAT - 1u, 'X'); st8(format + sizeof ISAAC_VITA_CRT_ROOM_LOG_FORMAT, 0u); ++length; }
        if (mode == 9u) { memcpy((void *)(uintptr_t)(format + 64u), (void *)(uintptr_t)format, sizeof ISAAC_VITA_CRT_ROOM_LOG_FORMAT); format += 64u; st32(frame + 12u, format); }
        if (mode == 10u) { memcpy(s_arguments, (void *)(uintptr_t)args, 12u); args = pointer32(s_arguments); }
        if (mode == 11u) { st32(esp + 4u, 0u); }
        if (mode == 12u) { buffer = ISAAC_VITA_CRT_ROOM_LOG_BUFFER_VA - 32u; count = 32u; }
        st32(esp + 12u, buffer); st32(esp + 16u, count);
        st32(esp + 20u, format); st32(esp + 24u, 0u); st32(esp + 28u, args);
        cpu.esp = esp; cpu.ebp = frame; cpu.eax = 0x5aa55aa5u;
        cpu.stack_owner = &cpu; cpu.stack_floor = pointer32(s_guest_stack);
        cpu.stack_ceiling = pointer32(s_guest_stack + 128);
        cpu.stack_low_water = cpu.stack_ceiling;
        expected = cpu; expected.esp += 4u; expected.eax = length;
        /* vita_crt_arg checks ESP+4 first; the existing gpop_at validates
         * its range but does not call guest_stack_note_low on the RET word. */
        expected.stack_low_water = esp + 4u;
        s_room_cpu = &cpu; s_room_expected_esp = esp;
        s_room_expected_buffer = buffer; s_room_expected_length = length;
        errno = EACCES; g_isaac_vita_crt_errno = ORACLE_GUEST_ERRNO_SENTINEL;
        vita_crt_stdio_common_vsprintf(&cpu);
        ROOM_CHECK(memcmp(&cpu, &expected, sizeof cpu) == 0 && errno == EACCES &&
                   g_isaac_vita_crt_errno == ORACLE_GUEST_ERRNO_SENTINEL);
#if defined(ISAAC_VITA_ROOM_LOG_MARKERS) && ISAAC_VITA_ROOM_LOG_MARKERS
        ROOM_CHECK(s_room_calls == calls + (mode == 0u));
#else
        ROOM_CHECK(s_room_calls == calls);
#endif
        ROOM_CHECK(!s_room_callback_bad);
        if (mode == 0u) ROOM_CHECK(memcmp((void *)(uintptr_t)buffer, s_room_expected, sizeof s_room_expected) == 0);
    }
#if defined(ISAAC_VITA_ROOM_LOG_MARKERS) && ISAAC_VITA_ROOM_LOG_MARKERS
    /* Guard-only hostile ranges: no formatter execution is claimed for these
     * invalid synthetic frames. The actual helper must skip without touching
     * CPU state, reading unbound memory, or calling the recorder. */
    for (mode = 0u; mode < 8u; ++mode) {
        CPU cpu, before;
        uint32_t frame = pointer32(&s_guest_stack[80]);
        uint32_t buffer = ISAAC_VITA_CRT_ROOM_LOG_BUFFER_VA;
        uint32_t count = ISAAC_VITA_CRT_ROOM_LOG_BUFFER_END - buffer;
        unsigned calls = s_room_calls;
        memset(&cpu, 0, sizeof cpu);
        cpu.esp = pointer32(&s_guest_stack[16]); cpu.ebp = frame;
        cpu.stack_owner = &cpu; cpu.stack_floor = pointer32(s_guest_stack);
        cpu.stack_ceiling = pointer32(s_guest_stack + 128); cpu.stack_low_water = cpu.stack_ceiling;
        st32(cpu.esp, ISAAC_VITA_CRT_VSPRINTF_TIMER_RETURN_RVA);
        if (mode == 0u) cpu.stack_owner = NULL;
        if (mode == 1u) cpu.esp = UINT32_MAX;
        if (mode == 2u) cpu.ebp = UINT32_MAX - 8u;
        if (mode == 3u) cpu.ebp = cpu.stack_ceiling - 8u;
        if (mode == 4u) count = UINT32_MAX;
        if (mode == 5u) buffer = UINT32_MAX;
        if (mode == 6u || mode == 7u) {
            cpu.stack_floor = buffer; cpu.stack_ceiling = ISAAC_VITA_CRT_ROOM_LOG_BUFFER_END;
            cpu.stack_low_water = cpu.stack_ceiling;
            cpu.esp = buffer + 64u; cpu.ebp = buffer + 128u;
            st32(cpu.esp, ISAAC_VITA_CRT_VSPRINTF_TIMER_RETURN_RVA);
            if (mode == 7u) { buffer += 128u; count -= 128u; }
        }
        before = cpu; errno = EACCES;
        vita_crt_room_log_completed(&cpu, buffer, count, ISAAC_VITA_CRT_ROOM_LOG_FORMAT_VA, cpu.ebp + 16u, 1u, 1);
        ROOM_CHECK(memcmp(&cpu, &before, sizeof cpu) == 0 && errno == EACCES && s_room_calls == calls);
    }
#endif
#if defined(_WIN32)
    ROOM_CHECK(VirtualFree(format_map, 0u, MEM_RELEASE));
    ROOM_CHECK(VirtualFree(buffer_map, 0u, MEM_RELEASE));
#else
    ROOM_CHECK(munmap(format_map, 65536u) == 0);
    ROOM_CHECK(munmap(buffer_map, 65536u) == 0);
#endif
    puts("Room CRT actual vsprintf seam: PASS (complete CPU/errno, exact chain/format, full output, fail-open ranges)");
    return 0;
#undef ROOM_CHECK
}
#endif

#if defined(ISAAC_VITA_CRT_ATOF_SMALLINT_TEST)
static unsigned s_strtod_calls;
static double oracle_strtod(const char *text, char **end)
{
    ++s_strtod_calls;
    return strtod(text, end);
}

static int oracle_atof_smallint(void)
{
    static const struct { const char *text; unsigned accepted; } cases[] = {
        { "0", 1 }, { "-0", 1 }, { "000000000", 1 }, { "-000000000", 1 },
        { "1", 1 }, { "-1", 1 }, { "100", 1 }, { "255", 1 },
        { "9", 1 }, { "10", 1 }, { "99", 1 }, { "999", 1 },
        { "1000", 1 }, { "9999", 1 }, { "10000", 1 }, { "99999", 1 },
        { "100000", 1 }, { "999999", 1 }, { "1000000", 1 },
        { "9999999", 1 }, { "10000000", 1 }, { "99999999", 1 },
        { "100000000", 1 }, { "-100000000", 1 },
        { "999999999", 1 }, { "-999999999", 1 }, { "012345678", 1 },
        { "", 0 }, { "-", 0 }, { "+", 0 }, { "+1", 0 }, { "--1", 0 },
        { "1000000000", 0 }, { "0000000000", 0 }, { "-1000000000", 0 },
        { "123x", 0 }, { "1 ", 0 }, { " 1", 0 }, { "\t1", 0 },
        { "1e0", 0 }, { "-0e1", 0 }, { "1e", 0 }, { "1.0", 0 },
        { "1,5", 0 }, { "0x10", 0 }, { "inf", 0 }, { "-inf", 0 },
        { "nan", 0 }, { "1e309", 0 }, { "1e-999", 0 },
        { "2147483647", 0 }, { "-2147483648", 0 },
        { "\xff", 0 }, { "1\xff", 0 }
    };
    unsigned i;
    uint32_t esp;
    CPU cpu, expected;
    unsigned checks = 0u;
#define ATOF_CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "atof check failed line=%d case=%u\n", __LINE__, i); return 210; } } while (0)
    for (i = 0u; i < sizeof cases / sizeof cases[0]; ++i) {
        double reference;
        unsigned expected_calls = 1u;
        strcpy(s_long_source, cases[i].text);
        reference = strtod(s_long_source, NULL);
        esp = oracle_prepare_call(&cpu, ORACLE_TOKEN, s_empty_format, s_arguments);
        st32(esp + 4u, pointer32(s_long_source));
        cpu.st_top = 5u;
        cpu.st[4] = 7.0; cpu.st[5] = -11.0;
        expected = cpu;
        expected.stack_owner = &expected;
        (void)vita_crt_arg(&expected, 0u); /* unchanged guarded argument read */
        fpush(&expected, reference);
        vita_crt_cdecl_return(&expected);
        expected.stack_owner = &cpu;
#if defined(ISAAC_VITA_CRT_ATOF_SMALLINT) && ISAAC_VITA_CRT_ATOF_SMALLINT
        expected_calls -= cases[i].accepted;
#endif
        s_strtod_calls = 0u;
        errno = EACCES; g_isaac_vita_crt_errno = ORACLE_GUEST_ERRNO_SENTINEL;
        vita_crt_atof(&cpu);
        ATOF_CHECK(memcmp(&cpu, &expected, sizeof cpu) == 0);
        ATOF_CHECK(s_strtod_calls == expected_calls && errno == EACCES &&
                   g_isaac_vita_crt_errno == ORACLE_GUEST_ERRNO_SENTINEL);
        ATOF_CHECK(strcmp(s_long_source, cases[i].text) == 0 &&
                   ld32(esp) == ORACLE_RETURN && ld32(esp + 4u) == pointer32(s_long_source));
    }
    /* A valid 4095-byte snapshot falls back; 4096 non-NUL bytes must fault
     * BEFORE any fast-path/parser work, leaving stack/x87 unchanged. */
    for (i = 0u; i < 3u; ++i) {
        memset(s_long_source, '0', 4096u); s_long_source[i == 0u ? 4095u : 4096u] = 0;
        esp = oracle_prepare_call(&cpu, ORACLE_TOKEN, s_empty_format, s_arguments);
        st32(esp + 4u, i == 2u ? 0u : pointer32(s_long_source));
        cpu.st_top = 6u; cpu.st[6] = -3.0;
        expected = cpu;
        if (i == 0u) {
            expected.stack_owner = &expected;
            (void)vita_crt_arg(&expected, 0u);
            fpush(&expected, 0.0); vita_crt_cdecl_return(&expected);
            expected.stack_owner = &cpu;
        }
        s_strtod_calls = 0u;
        errno = EACCES; g_isaac_vita_crt_errno = ORACLE_GUEST_ERRNO_SENTINEL;
        vita_crt_atof(&cpu);
        if (i == 0u) {
            ATOF_CHECK(memcmp(&cpu, &expected, sizeof cpu) == 0 && s_strtod_calls == 1u);
        } else {
            ATOF_CHECK(cpu.fault && cpu.esp == expected.esp && cpu.st_top == expected.st_top &&
                       memcmp(cpu.st, expected.st, sizeof cpu.st) == 0 && s_strtod_calls == 0u);
            ATOF_CHECK(strcmp(cpu.fault, i == 2u ? "atof received a null guest pointer" :
                             "atof input is unreadable or exceeds 4095 bytes") == 0);
        }
        ATOF_CHECK(errno == EACCES && g_isaac_vita_crt_errno == ORACLE_GUEST_ERRNO_SENTINEL);
    }
    printf("atof actual CRT boundary: PASS checks=%u cases=%u (full CPU/errno, fallback calls, snapshot limits)\n",
           checks, (unsigned)(sizeof cases / sizeof cases[0]) + 3u);
    return 0;
#undef ATOF_CHECK
}
#endif

int main(void)
{
    int result = run_oracle();

#if defined(ISAAC_VITA_GAME_LOG_BATCH)
    if (!result)
        result = oracle_log_batch();
#endif
#if defined(ISAAC_VITA_ROOM_LOG_MARKERS_TEST)
    if (!result)
        result = oracle_room_log();
#endif
#if defined(ISAAC_VITA_CRT_ATOF_SMALLINT_TEST)
    if (!result)
        result = oracle_atof_smallint();
#endif
    return result;
}
