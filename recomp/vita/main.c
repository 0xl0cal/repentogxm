#include "fixed_image.h"
#include "platform.h"

#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/power.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef GUEST_IMAGE_BASE
#define GUEST_IMAGE_BASE 0x98000000u
#endif

#ifndef ISAAC_VITA_CLOCK_BOOST
#define ISAAC_VITA_CLOCK_BOOST 0
#endif

#ifndef ISAAC_VITA_HAS_RUNTIME
#define ISAAC_VITA_HAS_RUNTIME 0
#endif

#ifndef ISAAC_VITA_BUILD_ID
#define ISAAC_VITA_BUILD_ID "link:unstamped"
#endif

#if ISAAC_VITA_HAS_RUNTIME
int isaac_vita_run_first_fault(const char *pe_path);
#endif

/*
 * A LiveArea target is delivered as an app event, not argv.  Keep this before
 * log reset and every game subsystem: a successful LoadExec replaces this
 * process, while any unavailable/malformed manager launch fails open into the
 * normal game.  This follows the established FF3/GTA Vita companion pattern.
 */
static void isaac_vita_livearea_dispatch(void)
{
    SceAppUtilInitParam init_param;
    SceAppUtilBootParam boot_param;
    SceAppUtilAppEventParam event_param;
    char target[2048];

    memset(&init_param, 0, sizeof(init_param));
    memset(&boot_param, 0, sizeof(boot_param));
    if (sceAppUtilInit(&init_param, &boot_param) < 0)
        return;

    memset(&event_param, 0, sizeof(event_param));
    memset(target, 0, sizeof(target));
    if (sceAppUtilReceiveAppEvent(&event_param) >= 0 &&
        event_param.type == 0x05u &&
        sceAppUtilAppEventParseLiveArea(&event_param, target) >= 0) {
        target[sizeof(target) - 1u] = '\0';
        if (strstr(target, "-manager") != NULL)
            (void)sceAppMgrLoadExec("app0:/isaac-manager.bin", NULL, NULL);
    }

    (void)sceAppUtilShutdown();
}

/*
 * A Vita application starts at the system default clocks, not the maximum:
 * CPU 333 MHz, BUS 222 MHz, GPU core 111 MHz, GPU crossbar 111 MHz.  Neither
 * vitaGL (pinned 73dd57a contains no scePowerSet* call) nor this runtime ever
 * raised them, so every hardware bundle so far -- including Bundle9 -- ran the
 * GPU at half its retail-game clock and the CPU at three quarters.
 *
 * The Bundle29 phase profile shows the consequence directly: a title screen
 * with 9 draws, 3 clears and 3 framebuffer binds per frame costs 18 ms inside
 * vglSwapBuffers, and in-game frames spend 18--19 ms there waiting on the GPU.
 *
 * Always log the frequencies actually in effect, before and after, so the
 * hardware log itself is the evidence in both A/B arms.  The maximums below are
 * the documented retail limits for scePowerSet*ClockFrequency.
 */
static void isaac_vita_clock_boost(void)
{
    int cpu_before = scePowerGetArmClockFrequency();
    int bus_before = scePowerGetBusClockFrequency();
    int gpu_before = scePowerGetGpuClockFrequency();
    int xbar_before = scePowerGetGpuXbarClockFrequency();
#if ISAAC_VITA_CLOCK_BOOST
    int cpu_result = scePowerSetArmClockFrequency(444);
    int bus_result = scePowerSetBusClockFrequency(222);
    int gpu_result = scePowerSetGpuClockFrequency(222);
    int xbar_result = scePowerSetGpuXbarClockFrequency(166);
#endif

    isaac_vita_log(
        "KAGE VITA CLOCK: policy=%s before cpu=%d bus=%d gpu=%d xbar=%d "
        "after cpu=%d bus=%d gpu=%d xbar=%d",
#if ISAAC_VITA_CLOCK_BOOST
        "boost-444/222/222/166",
#else
        "system-default",
#endif
        cpu_before, bus_before, gpu_before, xbar_before,
        scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(),
        scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());
#if ISAAC_VITA_CLOCK_BOOST
    if (cpu_result < 0 || bus_result < 0 || gpu_result < 0 ||
            xbar_result < 0) {
        isaac_vita_log(
            "KAGE VITA CLOCK: set FAILED cpu=0x%08x bus=0x%08x gpu=0x%08x "
            "xbar=0x%08x",
            (unsigned)cpu_result, (unsigned)bus_result,
            (unsigned)gpu_result, (unsigned)xbar_result);
    }
#endif
}

int main(void)
{
    int result;

    isaac_vita_livearea_dispatch();
    isaac_vita_log_reset();
    isaac_vita_log("build %s; guest base=0x%08x; softfp ARMv7",
                   ISAAC_VITA_BUILD_ID, (unsigned)GUEST_IMAGE_BASE);
    isaac_vita_clock_boost();

#if ISAAC_VITA_HAS_RUNTIME
    result = isaac_vita_run_first_fault(
        "app0:/isaac-ng.exe.unpacked.exe");
    isaac_vita_log("first-arm-fault boundary returned %d", result);
    return result;
#else
    {
        void *probe = isaac_vita_fixed_alloc(GUEST_IMAGE_BASE, 0x1000u);
        if (probe == NULL) {
            isaac_vita_log("scaffold fixed-allocation probe FAILED");
            return 2;
        }
        isaac_vita_fixed_free(probe);
    }
    result = 0;
    isaac_vita_log("scaffold fixed-allocation probe PASS; runtime not linked");
    return result;
#endif
}
