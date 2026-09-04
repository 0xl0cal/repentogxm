#ifndef KAGE_VITA_INPUT_H
#define KAGE_VITA_INPUT_H

#include <stdint.h>

#include "kage_pc_backend.h"

/* Platform-neutral copy of the one producer-owned SceCtrlData sample.  The
 * synthetic XInput consumer deliberately receives no SDK type and has no
 * route back to sceCtrl, so one guest tick cannot observe two pad samples. */
typedef struct kage_vita_gamepad_snapshot {
    uint32_t packet_number;
    uint32_t buttons;
    uint16_t xinput_buttons;
    uint8_t xinput_left_trigger;
    uint8_t reserved;
    uint8_t lx, ly, rx, ry;
} kage_vita_gamepad_snapshot;

/* XInput owns the physical pad after module discovery.  Start is then a safe
 * prefix: tap Start for pause, Start+L for pill/card, Start+R for map, or
 * hold Start alone for the guest's original keyboard-R restart action.  The
 * 800 ms prefix commit happens before R becomes level-high, so a short pause
 * tap can never also satisfy an unknown guest restart threshold. */
#define KAGE_VITA_INPUT_START_HOLD_US UINT64_C(800000)
#define KAGE_VITA_INPUT_XINPUT_BACK UINT16_C(0x0020)
#define KAGE_VITA_INPUT_XINPUT_RIGHT_SHOULDER UINT16_C(0x0200)
#define KAGE_VITA_INPUT_XINPUT_TRIGGER_FULL UINT8_C(0xff)

/* One or two missed peeks retain the last coherent sample.  Three
 * consecutive failed producer attempts publish a neutral sample exactly
 * once, bounding a held button/stick without turning a transient miss into a
 * release/repress pair.  This is an attempt count, independent of frame rate. */
#define KAGE_VITA_INPUT_FAILURE_NEUTRALIZE_COUNT 3u

/* Producer/consumer boundary for Vita controls.  sceCtrl is sampled only by
 * the main-loop producer; DeviceKeyboard consumers copy this cached GLFW-key
 * level snapshot and let the original guest build its own rising edges.  Once
 * XInput owns the pad, raw packet numbers still track every changed sample;
 * console telemetry is limited to physical button edges and synthetic
 * RB/BACK/R changes.  Analog-only packets stay intentionally silent. */
int      kage_vita_input_initialize(void);
void     kage_vita_input_deactivate(void);
int      kage_vita_input_sample(void);
int      kage_vita_input_gamepad_snapshot(
             kage_vita_gamepad_snapshot *snapshot);
/* Once the guest has accepted our synthetic XInput module, its original
 * DeviceController path owns all pad semantics.  This module-ownership latch
 * intentionally survives producer deactivate/reinitialize: KAGE keeps its
 * vitaGL context and guest controller globals across Shutdown -> Initialize,
 * so there is no second LoadLibraryA call to restore it.  FreeLibrary is the
 * boundary that clears ownership.  Suppress the legacy keyboard translation
 * while owned so one physical button never fires two action paths. */
void     kage_vita_input_set_xinput_active(int active);
void     kage_vita_input_keyboard_snapshot(
             uint8_t keys[KAGE_PC_KEY_COUNT]);

_Static_assert(sizeof(kage_vita_gamepad_snapshot) == 16u,
               "cached Vita gamepad snapshot ABI changed");
_Static_assert(KAGE_VITA_INPUT_FAILURE_NEUTRALIZE_COUNT > 0u,
               "failed-peek neutralization must be bounded");

#endif
