#ifndef KAGE_VITA_TOUCH_H
#define KAGE_VITA_TOUCH_H

#include <stdint.h>

/* The touch producer exposes semantic level signals plus their exact XInput
 * representation.  The guest remains responsible for tap/hold/edge policy. */
enum kage_vita_touch_action {
    KAGE_VITA_TOUCH_ACTION_NONE      = 0U,
    KAGE_VITA_TOUCH_ACTION_PILL_CARD = 1U << 0,
    KAGE_VITA_TOUCH_ACTION_MAP       = 1U << 1,
    KAGE_VITA_TOUCH_ACTION_ACTIVE    = 1U << 2
};

enum kage_vita_touch_xinput {
    KAGE_VITA_TOUCH_XINPUT_BACK           = 0x0020U,
    KAGE_VITA_TOUCH_XINPUT_RIGHT_SHOULDER = 0x0200U,
    KAGE_VITA_TOUCH_XINPUT_TRIGGER_FULL   = 0x00ffU
};

/* Conservative normalized hit boxes around the three HUD targets.  No exact
 * retail hit-box constants have surfaced, so keep these named and easy to
 * calibrate while leaving the playfield and menu centre inert:
 *
 *   active item: left 1/4,  top 1/3
 *   minimap:     right 1/4, top 1/3
 *   pocket slot: right 1/4, bottom 1/3
 *
 * Coordinates are inclusive Q0.16-like values in [0, 65535], derived from
 * sceTouchGetPanelInfo rather than the common but device-specific `/ 2`. */
#define KAGE_VITA_TOUCH_NORMALIZED_MAX UINT16_C(65535)
#define KAGE_VITA_TOUCH_LEFT_QUARTER_MAX UINT16_C(16383)
#define KAGE_VITA_TOUCH_RIGHT_QUARTER_MIN UINT16_C(49152)
#define KAGE_VITA_TOUCH_TOP_THIRD_MAX UINT16_C(21845)
#define KAGE_VITA_TOUCH_BOTTOM_THIRD_MIN UINT16_C(43690)

typedef struct kage_vita_touch_snapshot {
    uint32_t packet_number;
    uint16_t xinput_buttons;
    uint8_t xinput_left_trigger;
    uint8_t actions;
} kage_vita_touch_snapshot;

/* Exactly one main-loop producer calls sample().  Snapshot consumers never
 * call SceTouch and may run more often than the producer.  Only the front
 * panel is started or polled; the rear panel is intentionally unsupported. */
int  kage_vita_touch_initialize(void);
void kage_vita_touch_deactivate(void);
int  kage_vita_touch_sample(void);
int  kage_vita_touch_snapshot_read(kage_vita_touch_snapshot *snapshot);

_Static_assert(sizeof(kage_vita_touch_snapshot) == 8U,
               "Vita touch snapshot ABI changed");
_Static_assert((KAGE_VITA_TOUCH_ACTION_PILL_CARD |
                KAGE_VITA_TOUCH_ACTION_MAP |
                KAGE_VITA_TOUCH_ACTION_ACTIVE) < 8U,
               "touch actions must fit the combined XInput packet tag");

#endif
