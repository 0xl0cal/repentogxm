/* One-shot app-side SceScreenShot capture for physical-display evidence.
 * This unit is absent from ordinary builds.  It never allocates, retries,
 * unloads a module, installs a callback or runs on vitaGL's display thread. */
#include "kage_vita_screenshot_probe.h"

#include <stdint.h>
#include <string.h>

#if defined(ISAAC_VITA_SCREENSHOT_ORACLE)
int sceSysmoduleLoadModule(int module_id);
# define SCE_SYSMODULE_SCREEN_SHOT 0x001d
#else
# include <psp2/sysmodule.h>
#endif

void isaac_vita_log(const char *format, ...);

#ifndef ISAAC_VITA_SCREENSHOT_BUILD_ID
# error ISAAC_VITA_SCREENSHOT_BUILD_ID must identify the diagnostic binary
#endif

#define KAGE_VITA_SCREENSHOT_NOT_CALLED ((int32_t)0x80000000u)
#define KAGE_VITA_SCREENSHOT_LOG_FIXED_BYTES \
    (sizeof("[vita-shot] b= p= load=0x capture=0x " \
            "path_len= trunc= path=''") - 1u)
#define KAGE_VITA_SCREENSHOT_LOG_MAX_BODY \
    (KAGE_VITA_SCREENSHOT_LOG_FIXED_BYTES + 96u + 10u + 16u + 4u + 1u + \
     KAGE_VITA_SCREENSHOT_LOG_PATH_BYTES)

_Static_assert(KAGE_VITA_SCREENSHOT_LOG_MAX_BODY < 384u,
               "SceScreenShot diagnostic exceeds durable log body");

static unsigned s_attempted;

static unsigned kage_vita_screenshot_path_length(const char *path)
{
    unsigned length = 0u;
    while (length < KAGE_VITA_SCREENSHOT_PATH_BYTES && path[length] != '\0')
        ++length;
    return length;
}

static void kage_vita_screenshot_copy_log_path(
    char output[KAGE_VITA_SCREENSHOT_LOG_PATH_BYTES + 1u],
    const char *path, unsigned length)
{
    unsigned index;
    unsigned copied = length;

    if (copied > KAGE_VITA_SCREENSHOT_LOG_PATH_BYTES)
        copied = KAGE_VITA_SCREENSHOT_LOG_PATH_BYTES;
    for (index = 0u; index < copied; ++index) {
        unsigned char value = (unsigned char)path[index];
        output[index] = value >= 0x20u && value <= 0x7eu && value != '\''
            ? (char)value : '?';
    }
    output[copied] = '\0';
}

void kage_vita_screenshot_probe_post_swap(uint32_t returned_present_count)
{
    SceScreenShotCaptureFileInfo file_info;
    char log_path[KAGE_VITA_SCREENSHOT_LOG_PATH_BYTES + 1u];
    unsigned path_length;
    int load_result;
    int capture_result = KAGE_VITA_SCREENSHOT_NOT_CALLED;

    if (returned_present_count != KAGE_VITA_SCREENSHOT_TRIGGER_PRESENT ||
        s_attempted != 0u)
        return;
    /* Claim before the first foreign call: every failure is final and cannot
     * turn a later gameplay frame into an accidental retry. */
    s_attempted = 1u;
    memset(&file_info, 0, sizeof file_info);

    load_result = sceSysmoduleLoadModule(SCE_SYSMODULE_SCREEN_SHOT);
    if (load_result >= 0) {
        capture_result = sceScreenShotCapture(
            SCE_SCREEN_SHOT_CAPTURE_MODE_FORCE_CAPTURE,
            &file_info, NULL, NULL);
    }

    if (capture_result == 0) {
        /* TrophyShot's working ABI treats this entire object as the returned
         * path.  Terminate it ourselves before any bounded scan; on failure,
         * do not trust even a partially written path. */
        file_info.path[KAGE_VITA_SCREENSHOT_PATH_BYTES - 1u] = '\0';
        path_length = kage_vita_screenshot_path_length(file_info.path);
        kage_vita_screenshot_copy_log_path(
            log_path, file_info.path, path_length);
    } else {
        path_length = 0u;
        log_path[0] = '\0';
    }
    isaac_vita_log(
        "[vita-shot] b=%.96s p=%u load=0x%08x capture=0x%08x "
        "path_len=%u trunc=%u path='%.128s'",
        ISAAC_VITA_SCREENSHOT_BUILD_ID,
        (unsigned)returned_present_count,
        (unsigned)load_result, (unsigned)capture_result, path_length,
        (unsigned)(path_length > KAGE_VITA_SCREENSHOT_LOG_PATH_BYTES),
        log_path);
}
