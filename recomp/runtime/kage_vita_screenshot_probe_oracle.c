/* Executable oracle for the one-shot app-side SceScreenShot ABI. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kage_vita_screenshot_probe.h"

#ifndef ISAAC_VITA_SCREENSHOT_ORACLE_SCENARIO
# define ISAAC_VITA_SCREENSHOT_ORACLE_SCENARIO 0
#endif

#define ORACLE_SUCCESS           0
#define ORACLE_LOAD_FAILURE      1
#define ORACLE_UNTERMINATED_PATH 2
#define ORACLE_CAPTURE_FAILURE   3
#define ORACLE_FAILURE ((int32_t)0x805a10ffu)

static unsigned s_load_calls;
static unsigned s_capture_calls;
static unsigned s_log_calls;
static int s_capture_info_was_zero;
static int s_capture_mode;
static int s_cancel_callback_is_null;
static int s_cancel_userdata_is_null;
static int s_log_result;
static char s_log[512];

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Vita screenshot oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int sceSysmoduleLoadModule(int module_id)
{
    ++s_load_calls;
    if (module_id != 0x001d)
        return -1;
    return ISAAC_VITA_SCREENSHOT_ORACLE_SCENARIO == ORACLE_LOAD_FAILURE
        ? ORACLE_FAILURE : 0;
}

int sceScreenShotCapture(
    SceScreenShotCaptureMode mode,
    SceScreenShotCaptureFileInfo *file_info,
    SceScreenShotCaptureCancelCallback cancel_callback,
    void *userdata)
{
    const unsigned char *bytes = (const unsigned char *)file_info;
    unsigned index;

    ++s_capture_calls;
    s_capture_mode = mode;
    s_cancel_callback_is_null = cancel_callback == NULL;
    s_cancel_userdata_is_null = userdata == NULL;
    s_capture_info_was_zero = 1;
    for (index = 0u; index < sizeof *file_info; ++index) {
        if (bytes[index] != 0u)
            s_capture_info_was_zero = 0;
    }
    if (ISAAC_VITA_SCREENSHOT_ORACLE_SCENARIO == ORACLE_UNTERMINATED_PATH ||
        ISAAC_VITA_SCREENSHOT_ORACLE_SCENARIO == ORACLE_CAPTURE_FAILURE) {
        memset(file_info->path, 'A', sizeof file_info->path);
        file_info->path[3] = '\n';
        file_info->path[5] = '\'';
    } else {
        (void)snprintf(file_info->path, sizeof file_info->path,
                       "ux0:/picture/AB/isaac-v14.png");
    }
    return ISAAC_VITA_SCREENSHOT_ORACLE_SCENARIO == ORACLE_CAPTURE_FAILURE
        ? ORACLE_FAILURE : 0;
}

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;

    ++s_log_calls;
    va_start(arguments, format);
    s_log_result = vsnprintf(s_log, sizeof s_log, format, arguments);
    va_end(arguments);
}

int main(void)
{
    unsigned present;

    for (present = 0u; present < KAGE_VITA_SCREENSHOT_TRIGGER_PRESENT;
         ++present)
        kage_vita_screenshot_probe_post_swap(present);
    CHECK(s_load_calls == 0u && s_capture_calls == 0u && s_log_calls == 0u);

    kage_vita_screenshot_probe_post_swap(
        KAGE_VITA_SCREENSHOT_TRIGGER_PRESENT);
    CHECK(s_load_calls == 1u && s_log_calls == 1u);
    CHECK(s_log_result > 0 && s_log_result < 384);
    CHECK((int)strlen(s_log) == s_log_result);
    CHECK(strchr(s_log, '\r') == NULL);
    CHECK(strchr(s_log, '\n') == NULL);
    CHECK(strstr(s_log, "p=600") != NULL);

#if ISAAC_VITA_SCREENSHOT_ORACLE_SCENARIO == ORACLE_LOAD_FAILURE
    CHECK(s_capture_calls == 0u);
    CHECK(strstr(s_log, "load=0x805a10ff") != NULL);
    CHECK(strstr(s_log, "capture=0x80000000") != NULL);
#elif ISAAC_VITA_SCREENSHOT_ORACLE_SCENARIO == ORACLE_CAPTURE_FAILURE
    CHECK(s_capture_calls == 1u);
    CHECK(s_capture_info_was_zero != 0);
    CHECK(strstr(s_log, "capture=0x805a10ff") != NULL);
    CHECK(strstr(s_log, "path_len=0 trunc=0 path=''") != NULL);
#else
    CHECK(s_capture_calls == 1u);
    CHECK(s_capture_info_was_zero != 0);
    CHECK(s_capture_mode == SCE_SCREEN_SHOT_CAPTURE_MODE_FORCE_CAPTURE);
    CHECK(s_cancel_callback_is_null && s_cancel_userdata_is_null);
    CHECK(strstr(s_log, "load=0x00000000") != NULL);
    CHECK(strstr(s_log, "capture=0x00000000") != NULL);
# if ISAAC_VITA_SCREENSHOT_ORACLE_SCENARIO == ORACLE_UNTERMINATED_PATH
    CHECK(strstr(s_log, "path_len=1023 trunc=1") != NULL);
    CHECK(strstr(s_log, "path='AAA?A?") != NULL);
# else
    CHECK(strstr(s_log, "path_len=29 trunc=0") != NULL);
    CHECK(strstr(s_log, "path='ux0:/picture/AB/isaac-v14.png'") != NULL);
# endif
#endif

    /* Exact-trigger duplicates and every later frame are inert, including
     * after a failed load or capture. */
    kage_vita_screenshot_probe_post_swap(
        KAGE_VITA_SCREENSHOT_TRIGGER_PRESENT);
    for (present = KAGE_VITA_SCREENSHOT_TRIGGER_PRESENT + 1u;
         present < KAGE_VITA_SCREENSHOT_TRIGGER_PRESENT + 100u; ++present)
        kage_vita_screenshot_probe_post_swap(present);
    CHECK(s_load_calls == 1u && s_log_calls == 1u);
#if ISAAC_VITA_SCREENSHOT_ORACLE_SCENARIO == ORACLE_LOAD_FAILURE
    CHECK(s_capture_calls == 0u);
#else
    CHECK(s_capture_calls == 1u);
#endif

    puts("Vita app-side one-shot screenshot oracle: PASS");
    return 0;
}
