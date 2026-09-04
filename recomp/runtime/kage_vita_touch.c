/* Cached front-touch producer for the synthetic XInput backend.
 *
 * SceTouch is polled once at the main-loop boundary.  Each contact ID is
 * classified only when it first appears and stays pinned to that action until
 * release, so sliding across HUD/map boundaries cannot create extra actions. */
#include "kage_vita_touch.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(ISAAC_KAGE_VITA_TOUCH_ORACLE)
# include "kage_vita_touch_test_touch.h"
#else
# include <psp2/touch.h>
#endif

_Static_assert(SCE_TOUCH_MAX_REPORT == 8,
               "touch slot census assumes the VitaSDK report capacity");
_Static_assert(sizeof(SceTouchPanelInfo) == 0x30U,
               "VitaSDK SceTouchPanelInfo layout changed");
_Static_assert(sizeof(SceTouchReport) == 0x10U,
               "VitaSDK SceTouchReport layout changed");
_Static_assert(sizeof(SceTouchData) == 0x90U,
               "VitaSDK SceTouchData layout changed");

typedef struct kage_vita_touch_bounds {
    int32_t min_x;
    int32_t min_y;
    int32_t max_x;
    int32_t max_y;
} kage_vita_touch_bounds;

typedef struct kage_vita_touch_slot {
    uint8_t in_use;
    uint8_t id;
    uint8_t action;
    uint8_t reserved;
} kage_vita_touch_slot;

static volatile int s_touch_lock;
static volatile int s_initialized;
static kage_vita_touch_bounds s_bounds;
static kage_vita_touch_slot s_slots[SCE_TOUCH_MAX_REPORT];
static uint32_t s_packet_number;
static uint8_t s_actions;

static void kage_vita_touch_lock(void)
{
    while (__sync_lock_test_and_set(&s_touch_lock, 1)) {
        /* The producer holds this only while updating eight tiny slots. */
    }
    __sync_synchronize();
}

static void kage_vita_touch_unlock(void)
{
    __sync_synchronize();
    __sync_lock_release(&s_touch_lock);
}

static void kage_vita_touch_publish_locked(uint8_t actions)
{
    if (s_actions == actions)
        return;
    s_actions = actions;
    ++s_packet_number;
    if (!s_packet_number)
        s_packet_number = 1U;
}

static void kage_vita_touch_neutralize_locked(void)
{
    memset(s_slots, 0, sizeof s_slots);
    kage_vita_touch_publish_locked(KAGE_VITA_TOUCH_ACTION_NONE);
}

static uint16_t kage_vita_touch_normalize(int32_t value,
                                          int32_t minimum,
                                          int32_t maximum)
{
    uint32_t numerator;
    uint32_t denominator;

    if (value <= minimum)
        return 0U;
    if (value >= maximum)
        return KAGE_VITA_TOUCH_NORMALIZED_MAX;
    numerator = (uint32_t)(value - minimum) *
                (uint32_t)KAGE_VITA_TOUCH_NORMALIZED_MAX;
    denominator = (uint32_t)(maximum - minimum);
    return (uint16_t)(numerator / denominator);
}

static uint8_t kage_vita_touch_classify(uint16_t x, uint16_t y)
{
    if (x <= KAGE_VITA_TOUCH_LEFT_QUARTER_MAX &&
            y <= KAGE_VITA_TOUCH_TOP_THIRD_MAX)
        return KAGE_VITA_TOUCH_ACTION_ACTIVE;
    if (x >= KAGE_VITA_TOUCH_RIGHT_QUARTER_MIN &&
            y <= KAGE_VITA_TOUCH_TOP_THIRD_MAX)
        return KAGE_VITA_TOUCH_ACTION_MAP;
    if (x >= KAGE_VITA_TOUCH_RIGHT_QUARTER_MIN &&
            y >= KAGE_VITA_TOUCH_BOTTOM_THIRD_MIN)
        return KAGE_VITA_TOUCH_ACTION_PILL_CARD;
    return KAGE_VITA_TOUCH_ACTION_NONE;
}

static int kage_vita_touch_report_less(const SceTouchReport *left,
                                       const SceTouchReport *right)
{
    if (left->id != right->id)
        return left->id < right->id;
    if (left->x != right->x)
        return left->x < right->x;
    return left->y < right->y;
}

static void kage_vita_touch_sort_reports(SceTouchReport *reports,
                                         uint32_t report_count)
{
    uint32_t index;

    for (index = 1U; index < report_count; ++index) {
        SceTouchReport current = reports[index];
        uint32_t position = index;

        while (position > 0U &&
               kage_vita_touch_report_less(&current,
                                           &reports[position - 1U])) {
            reports[position] = reports[position - 1U];
            --position;
        }
        reports[position] = current;
    }
}

static int kage_vita_touch_report_has_id(const SceTouchReport *reports,
                                         uint32_t report_count,
                                         uint8_t id)
{
    uint32_t index;

    for (index = 0U; index < report_count; ++index) {
        if (reports[index].id == id)
            return 1;
    }
    return 0;
}

static int kage_vita_touch_slot_for_id(uint8_t id)
{
    uint32_t index;

    for (index = 0U; index < SCE_TOUCH_MAX_REPORT; ++index) {
        if (s_slots[index].in_use && s_slots[index].id == id)
            return (int)index;
    }
    return -1;
}

static int kage_vita_touch_free_slot(void)
{
    uint32_t index;

    for (index = 0U; index < SCE_TOUCH_MAX_REPORT; ++index) {
        if (!s_slots[index].in_use)
            return (int)index;
    }
    return -1;
}

static void kage_vita_touch_apply_locked(SceTouchReport *reports,
                                         uint32_t report_count,
                                         const kage_vita_touch_bounds *bounds)
{
    uint32_t index;
    uint8_t actions = KAGE_VITA_TOUCH_ACTION_NONE;

    kage_vita_touch_sort_reports(reports, report_count);

    /* A successful snapshot is authoritative: missing IDs were released. */
    for (index = 0U; index < SCE_TOUCH_MAX_REPORT; ++index) {
        if (s_slots[index].in_use &&
                !kage_vita_touch_report_has_id(
                    reports, report_count, s_slots[index].id))
            memset(&s_slots[index], 0, sizeof s_slots[index]);
    }

    /* Sorting by ID/x/y makes even malformed duplicate-ID input deterministic.
     * Every ID, including contacts classified as NONE, gets a slot so movement
     * into a zone cannot turn a drag into a new press. */
    for (index = 0U; index < report_count; ++index) {
        int slot;
        uint16_t x;
        uint16_t y;

        if (index > 0U && reports[index - 1U].id == reports[index].id)
            continue;
        if (kage_vita_touch_slot_for_id(reports[index].id) >= 0)
            continue;
        slot = kage_vita_touch_free_slot();
        if (slot < 0)
            break;
        x = kage_vita_touch_normalize(
            reports[index].x, bounds->min_x, bounds->max_x);
        y = kage_vita_touch_normalize(
            reports[index].y, bounds->min_y, bounds->max_y);
        s_slots[slot].in_use = 1U;
        s_slots[slot].id = reports[index].id;
        s_slots[slot].action = kage_vita_touch_classify(x, y);
    }

    for (index = 0U; index < SCE_TOUCH_MAX_REPORT; ++index) {
        if (s_slots[index].in_use)
            actions |= s_slots[index].action;
    }
    kage_vita_touch_publish_locked(actions);
}

int kage_vita_touch_initialize(void)
{
    SceTouchPanelInfo panel_info;
    kage_vita_touch_bounds bounds;
    int was_initialized;
    int result;

    /* Reinitialization is a full release/start boundary. */
    kage_vita_touch_lock();
    was_initialized = s_initialized != 0;
    s_initialized = 0;
    memset(&s_bounds, 0, sizeof s_bounds);
    memset(s_slots, 0, sizeof s_slots);
    s_actions = KAGE_VITA_TOUCH_ACTION_NONE;
    s_packet_number = 0U;
    kage_vita_touch_unlock();
    if (was_initialized) {
        (void)sceTouchSetSamplingState(
            SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_STOP);
    }

    memset(&panel_info, 0, sizeof panel_info);
    result = sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &panel_info);
    if (result < 0)
        return 0;
    bounds.min_x = panel_info.minAaX;
    bounds.min_y = panel_info.minAaY;
    bounds.max_x = panel_info.maxAaX;
    bounds.max_y = panel_info.maxAaY;
    if (bounds.max_x <= bounds.min_x || bounds.max_y <= bounds.min_y)
        return 0;
    result = sceTouchSetSamplingState(
        SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    if (result < 0)
        return 0;

    kage_vita_touch_lock();
    s_bounds = bounds;
    s_initialized = 1;
    kage_vita_touch_unlock();
    return 1;
}

void kage_vita_touch_deactivate(void)
{
    int was_initialized;

    kage_vita_touch_lock();
    was_initialized = s_initialized != 0;
    s_initialized = 0;
    kage_vita_touch_neutralize_locked();
    memset(&s_bounds, 0, sizeof s_bounds);
    kage_vita_touch_unlock();
    if (was_initialized) {
        (void)sceTouchSetSamplingState(
            SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_STOP);
    }
}

int kage_vita_touch_sample(void)
{
    SceTouchData data;
    SceTouchReport reports[SCE_TOUCH_MAX_REPORT];
    kage_vita_touch_bounds bounds;
    int result;

    kage_vita_touch_lock();
    if (!s_initialized) {
        kage_vita_touch_unlock();
        return 0;
    }
    bounds = s_bounds;
    kage_vita_touch_unlock();

    memset(&data, 0, sizeof data);
    result = sceTouchPeek(SCE_TOUCH_PORT_FRONT, &data, 1U);
    if (result != 1 || data.reportNum > SCE_TOUCH_MAX_REPORT) {
        /* Unlike pad motion, a stuck touch can spend a consumable.  Fail
         * closed immediately on a missing/malformed snapshot. */
        kage_vita_touch_lock();
        if (s_initialized)
            kage_vita_touch_neutralize_locked();
        kage_vita_touch_unlock();
        return 0;
    }
    memcpy(reports, data.report,
           (size_t)data.reportNum * sizeof reports[0]);

    kage_vita_touch_lock();
    if (!s_initialized) {
        kage_vita_touch_unlock();
        return 0;
    }
    kage_vita_touch_apply_locked(reports, data.reportNum, &bounds);
    kage_vita_touch_unlock();
    return 1;
}

int kage_vita_touch_snapshot_read(kage_vita_touch_snapshot *snapshot)
{
    int available;

    if (!snapshot)
        return 0;
    memset(snapshot, 0, sizeof *snapshot);
    kage_vita_touch_lock();
    available = s_initialized != 0;
    if (available) {
        snapshot->packet_number = s_packet_number;
        snapshot->actions = s_actions;
        if (s_actions & KAGE_VITA_TOUCH_ACTION_PILL_CARD)
            snapshot->xinput_buttons |=
                KAGE_VITA_TOUCH_XINPUT_RIGHT_SHOULDER;
        if (s_actions & KAGE_VITA_TOUCH_ACTION_MAP)
            snapshot->xinput_buttons |= KAGE_VITA_TOUCH_XINPUT_BACK;
        if (s_actions & KAGE_VITA_TOUCH_ACTION_ACTIVE)
            snapshot->xinput_left_trigger =
                KAGE_VITA_TOUCH_XINPUT_TRIGGER_FULL;
    }
    kage_vita_touch_unlock();
    return available;
}
