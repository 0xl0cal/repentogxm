#ifndef KAGE_VITA_SCREENSHOT_PROBE_H
#define KAGE_VITA_SCREENSHOT_PROBE_H

#include <stddef.h>
#include <stdint.h>

/* SceScreenShot_stub_weak exports sceScreenShotCapture, but the pinned
 * VitaSDK public header intentionally exposes only enable/disable and
 * metadata APIs. Keep the missing app-side ABI local to this diagnostic. */
#define KAGE_VITA_SCREENSHOT_TRIGGER_PRESENT 600u
#define KAGE_VITA_SCREENSHOT_PATH_BYTES      1024u
#define KAGE_VITA_SCREENSHOT_LOG_PATH_BYTES  128u

typedef enum SceScreenShotCaptureMode {
    SCE_SCREEN_SHOT_CAPTURE_MODE_DEFAULT = 0,
    SCE_SCREEN_SHOT_CAPTURE_MODE_FORCE_CAPTURE = 1
} SceScreenShotCaptureMode;

typedef struct SceScreenShotCaptureFileInfo {
    char path[KAGE_VITA_SCREENSHOT_PATH_BYTES];
} SceScreenShotCaptureFileInfo;

typedef int (*SceScreenShotCaptureCancelCallback)(void *userdata);

int sceScreenShotCapture(
    SceScreenShotCaptureMode mode,
    SceScreenShotCaptureFileInfo *file_info,
    SceScreenShotCaptureCancelCallback cancel_callback,
    void *userdata);

_Static_assert(SCE_SCREEN_SHOT_CAPTURE_MODE_FORCE_CAPTURE == 1,
               "SceScreenShot force-capture mode ABI changed");
/* The Vita EABI uses short enums here; AAPCS still passes this by-value
 * parameter in r0. Do not project the host compiler's enum width onto ARM. */
#if defined(__arm__)
_Static_assert(sizeof(SceScreenShotCaptureMode) == 1u,
               "SceScreenShot capture mode lost the pinned Vita enum ABI");
#endif
_Static_assert(sizeof(SceScreenShotCaptureFileInfo) == 1024u,
               "SceScreenShot capture-file ABI changed");
_Static_assert(offsetof(SceScreenShotCaptureFileInfo, path) == 0u,
               "SceScreenShot capture path offset changed");
_Static_assert(_Alignof(SceScreenShotCaptureFileInfo) == 1u,
               "SceScreenShot capture-file alignment changed");

/* Called synchronously by the vitaGL owner after vglSwapBuffers returns. */
void kage_vita_screenshot_probe_post_swap(uint32_t returned_present_count);

#endif
