#include <stdio.h>
#include <string.h>

#include "kage_vita_touch.h"
#include "kage_vita_touch_test_touch.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "touch oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

typedef struct fake_sampling_call {
    SceUInt32 port;
    SceTouchSamplingState state;
} fake_sampling_call;

static SceTouchPanelInfo s_panel_info;
static SceTouchData s_next_data;
static int s_panel_result;
static int s_peek_result;
static int s_start_result;
static int s_stop_result;
static unsigned s_panel_calls;
static unsigned s_peek_calls;
static unsigned s_sampling_calls;
static SceUInt32 s_last_panel_port;
static SceUInt32 s_last_peek_port;
static SceUInt32 s_last_peek_buffers;
static fake_sampling_call s_sampling_log[16];

int sceTouchGetPanelInfo(SceUInt32 port, SceTouchPanelInfo *panel_info)
{
    ++s_panel_calls;
    s_last_panel_port = port;
    if (s_panel_result >= 0 && panel_info)
        *panel_info = s_panel_info;
    return s_panel_result;
}

int sceTouchPeek(SceUInt32 port, SceTouchData *data, SceUInt32 buffers)
{
    ++s_peek_calls;
    s_last_peek_port = port;
    s_last_peek_buffers = buffers;
    if (s_peek_result == 1 && data)
        *data = s_next_data;
    return s_peek_result;
}

int sceTouchSetSamplingState(SceUInt32 port, SceTouchSamplingState state)
{
    if (s_sampling_calls <
            sizeof s_sampling_log / sizeof s_sampling_log[0]) {
        s_sampling_log[s_sampling_calls].port = port;
        s_sampling_log[s_sampling_calls].state = state;
    }
    ++s_sampling_calls;
    return state == SCE_TOUCH_SAMPLING_STATE_START
        ? s_start_result : s_stop_result;
}

static void fake_reset(void)
{
    memset(&s_panel_info, 0, sizeof s_panel_info);
    memset(&s_next_data, 0, sizeof s_next_data);
    memset(s_sampling_log, 0, sizeof s_sampling_log);
    s_panel_info.minAaX = 1000;
    s_panel_info.maxAaX = 5000;
    s_panel_info.minAaY = -500;
    s_panel_info.maxAaY = 1500;
    s_panel_result = 0;
    s_peek_result = 1;
    s_start_result = 0;
    s_stop_result = 0;
    s_panel_calls = 0U;
    s_peek_calls = 0U;
    s_sampling_calls = 0U;
    s_last_panel_port = UINT32_C(0xffffffff);
    s_last_peek_port = UINT32_C(0xffffffff);
    s_last_peek_buffers = 0U;
}

static void reports_clear(void)
{
    memset(&s_next_data, 0, sizeof s_next_data);
}

static void report_add(uint8_t id, int16_t x, int16_t y)
{
    SceTouchReport *report;

    if (s_next_data.reportNum >= SCE_TOUCH_MAX_REPORT)
        return;
    report = &s_next_data.report[s_next_data.reportNum++];
    memset(report, 0, sizeof *report);
    report->id = id;
    report->x = x;
    report->y = y;
}

static int snapshot_is(uint8_t actions, uint16_t buttons,
                       uint8_t left_trigger, uint32_t packet_number)
{
    kage_vita_touch_snapshot snapshot;

    memset(&snapshot, 0xcc, sizeof snapshot);
    if (!kage_vita_touch_snapshot_read(&snapshot))
        return 0;
    return snapshot.actions == actions &&
           snapshot.xinput_buttons == buttons &&
           snapshot.xinput_left_trigger == left_trigger &&
           snapshot.packet_number == packet_number;
}

static int snapshot_unavailable(void)
{
    kage_vita_touch_snapshot snapshot;
    kage_vita_touch_snapshot zero;

    memset(&snapshot, 0xcc, sizeof snapshot);
    memset(&zero, 0, sizeof zero);
    return !kage_vita_touch_snapshot_read(&snapshot) &&
           memcmp(&snapshot, &zero, sizeof snapshot) == 0;
}

static int all_recorded_ports_are_front(void)
{
    unsigned index;

    if (s_panel_calls && s_last_panel_port != SCE_TOUCH_PORT_FRONT)
        return 0;
    if (s_peek_calls && (s_last_peek_port != SCE_TOUCH_PORT_FRONT ||
                         s_last_peek_buffers != 1U))
        return 0;
    for (index = 0U; index < s_sampling_calls &&
            index < sizeof s_sampling_log / sizeof s_sampling_log[0];
         ++index) {
        if (s_sampling_log[index].port != SCE_TOUCH_PORT_FRONT)
            return 0;
    }
    return 1;
}

int main(void)
{
    kage_vita_touch_snapshot first_order;
    kage_vita_touch_snapshot reverse_order;
    unsigned peek_before;
    unsigned sampling_before;

    CHECK(!kage_vita_touch_snapshot_read(NULL));
    CHECK(KAGE_VITA_TOUCH_LEFT_QUARTER_MAX <
          KAGE_VITA_TOUCH_RIGHT_QUARTER_MIN);
    CHECK(KAGE_VITA_TOUCH_TOP_THIRD_MAX <
          KAGE_VITA_TOUCH_BOTTOM_THIRD_MIN);

    /* Initialization is fail-closed and never starts sampling without valid
     * bounds. */
    fake_reset();
    s_panel_result = -1;
    CHECK(!kage_vita_touch_initialize());
    CHECK(s_panel_calls == 1U && s_sampling_calls == 0U &&
          s_peek_calls == 0U && snapshot_unavailable());
    CHECK(all_recorded_ports_are_front());

    fake_reset();
    s_panel_info.maxAaX = s_panel_info.minAaX;
    CHECK(!kage_vita_touch_initialize());
    CHECK(s_panel_calls == 1U && s_sampling_calls == 0U &&
          snapshot_unavailable());

    fake_reset();
    s_start_result = -1;
    CHECK(!kage_vita_touch_initialize());
    CHECK(s_panel_calls == 1U && s_sampling_calls == 1U &&
          s_sampling_log[0].state == SCE_TOUCH_SAMPLING_STATE_START &&
          snapshot_unavailable());
    CHECK(all_recorded_ports_are_front());

    /* These deliberately non-zero, non-1920x1088 bounds prove that raw
     * coordinates are normalized from panel info rather than divided by 2. */
    fake_reset();
    CHECK(kage_vita_touch_initialize());
    CHECK(s_panel_calls == 1U && s_sampling_calls == 1U &&
          s_sampling_log[0].state == SCE_TOUCH_SAMPLING_STATE_START);
    CHECK(snapshot_is(KAGE_VITA_TOUCH_ACTION_NONE, 0U, 0U, 0U));
    reports_clear();
    CHECK(kage_vita_touch_sample());
    CHECK(s_peek_calls == 1U &&
          snapshot_is(KAGE_VITA_TOUCH_ACTION_NONE, 0U, 0U, 0U));

    /* Bottom-right is XInput RB (pocket item), held as a level. */
    reports_clear();
    report_add(7U, 4600, 1300);
    CHECK(kage_vita_touch_sample());
    CHECK(snapshot_is(KAGE_VITA_TOUCH_ACTION_PILL_CARD,
                      KAGE_VITA_TOUCH_XINPUT_RIGHT_SHOULDER, 0U, 1U));

    /* The ID is pinned: moving the same contact into the top-right minimap
     * keeps RB and does not advance the effective-state packet. */
    reports_clear();
    report_add(7U, 4600, -400);
    CHECK(kage_vita_touch_sample());
    CHECK(snapshot_is(KAGE_VITA_TOUCH_ACTION_PILL_CARD,
                      KAGE_VITA_TOUCH_XINPUT_RIGHT_SHOULDER, 0U, 1U));

    /* Two simultaneous actions OR deterministically, independent of report
     * order. */
    reports_clear();
    report_add(7U, 1200, -400);
    report_add(2U, 4600, -400);
    CHECK(kage_vita_touch_sample());
    CHECK(snapshot_is(KAGE_VITA_TOUCH_ACTION_PILL_CARD |
                      KAGE_VITA_TOUCH_ACTION_MAP,
                      KAGE_VITA_TOUCH_XINPUT_BACK |
                      KAGE_VITA_TOUCH_XINPUT_RIGHT_SHOULDER,
                      0U, 2U));
    reports_clear();
    report_add(2U, 4600, 500);
    report_add(7U, 3000, 500);
    CHECK(kage_vita_touch_sample());
    CHECK(snapshot_is(KAGE_VITA_TOUCH_ACTION_PILL_CARD |
                      KAGE_VITA_TOUCH_ACTION_MAP,
                      KAGE_VITA_TOUCH_XINPUT_BACK |
                      KAGE_VITA_TOUCH_XINPUT_RIGHT_SHOULDER,
                      0U, 2U));

    /* A contact born outside all zones remains NONE even after it moves into
     * the pill corner. */
    reports_clear();
    report_add(9U, 4600, 500);
    report_add(2U, 3000, 500);
    report_add(7U, 3000, 500);
    CHECK(kage_vita_touch_sample());
    reports_clear();
    report_add(9U, 4600, 1300);
    report_add(2U, 3000, 500);
    report_add(7U, 3000, 500);
    CHECK(kage_vita_touch_sample());
    CHECK(snapshot_is(KAGE_VITA_TOUCH_ACTION_PILL_CARD |
                      KAGE_VITA_TOUCH_ACTION_MAP,
                      KAGE_VITA_TOUCH_XINPUT_BACK |
                      KAGE_VITA_TOUCH_XINPUT_RIGHT_SHOULDER,
                      0U, 2U));

    /* Releasing one ID preserves the other level; releasing a NONE-pinned ID
     * and touching down again permits a fresh classification. */
    reports_clear();
    report_add(9U, 4600, 1300);
    report_add(2U, 3000, 500);
    CHECK(kage_vita_touch_sample());
    CHECK(snapshot_is(KAGE_VITA_TOUCH_ACTION_MAP,
                      KAGE_VITA_TOUCH_XINPUT_BACK, 0U, 3U));
    reports_clear();
    report_add(9U, 4600, 1300);
    CHECK(kage_vita_touch_sample());
    CHECK(snapshot_is(KAGE_VITA_TOUCH_ACTION_NONE, 0U, 0U, 4U));
    reports_clear();
    CHECK(kage_vita_touch_sample());
    CHECK(snapshot_is(KAGE_VITA_TOUCH_ACTION_NONE, 0U, 0U, 4U));
    reports_clear();
    report_add(9U, 4600, 1300);
    CHECK(kage_vita_touch_sample());
    CHECK(snapshot_is(KAGE_VITA_TOUCH_ACTION_PILL_CARD,
                      KAGE_VITA_TOUCH_XINPUT_RIGHT_SHOULDER, 0U, 5U));

    /* Any read error neutralizes immediately.  Repeated errors cannot create
     * packet churn, and a later successful contact is a fresh touchdown. */
    s_peek_result = -1;
    CHECK(!kage_vita_touch_sample());
    CHECK(snapshot_is(KAGE_VITA_TOUCH_ACTION_NONE, 0U, 0U, 6U));
    CHECK(!kage_vita_touch_sample());
    CHECK(snapshot_is(KAGE_VITA_TOUCH_ACTION_NONE, 0U, 0U, 6U));
    s_peek_result = 1;
    reports_clear();
    report_add(9U, 4600, 1300);
    CHECK(kage_vita_touch_sample());
    CHECK(snapshot_is(KAGE_VITA_TOUCH_ACTION_PILL_CARD,
                      KAGE_VITA_TOUCH_XINPUT_RIGHT_SHOULDER, 0U, 7U));

    s_next_data.reportNum = SCE_TOUCH_MAX_REPORT + 1U;
    CHECK(!kage_vita_touch_sample());
    CHECK(snapshot_is(KAGE_VITA_TOUCH_ACTION_NONE, 0U, 0U, 8U));

    /* Deactivation clears a held action before attempting STOP.  A STOP
     * failure cannot leave a visible level behind, and repeated deactivation
     * never touches the kernel again. */
    reports_clear();
    report_add(3U, 4600, 1300);
    CHECK(kage_vita_touch_sample());
    CHECK(snapshot_is(KAGE_VITA_TOUCH_ACTION_PILL_CARD,
                      KAGE_VITA_TOUCH_XINPUT_RIGHT_SHOULDER, 0U, 9U));
    s_stop_result = -1;
    sampling_before = s_sampling_calls;
    kage_vita_touch_deactivate();
    CHECK(s_sampling_calls == sampling_before + 1U &&
          s_sampling_log[sampling_before].state ==
              SCE_TOUCH_SAMPLING_STATE_STOP &&
          snapshot_unavailable());
    peek_before = s_peek_calls;
    CHECK(!kage_vita_touch_sample() && s_peek_calls == peek_before);
    kage_vita_touch_deactivate();
    CHECK(s_sampling_calls == sampling_before + 1U);
    CHECK(all_recorded_ports_are_front());

    /* Duplicate touches for one action form one level.  Neither adding nor
     * removing a redundant finger changes the XInput packet. */
    fake_reset();
    CHECK(kage_vita_touch_initialize());
    reports_clear();
    report_add(5U, 4600, 1300);
    CHECK(kage_vita_touch_sample());
    CHECK(snapshot_is(KAGE_VITA_TOUCH_ACTION_PILL_CARD,
                      KAGE_VITA_TOUCH_XINPUT_RIGHT_SHOULDER, 0U, 1U));
    reports_clear();
    report_add(5U, 4600, 1300);
    report_add(1U, 4700, 1400);
    CHECK(kage_vita_touch_sample());
    CHECK(snapshot_is(KAGE_VITA_TOUCH_ACTION_PILL_CARD,
                      KAGE_VITA_TOUCH_XINPUT_RIGHT_SHOULDER, 0U, 1U));
    reports_clear();
    report_add(1U, 4700, 1400);
    CHECK(kage_vita_touch_sample());
    CHECK(snapshot_is(KAGE_VITA_TOUCH_ACTION_PILL_CARD,
                      KAGE_VITA_TOUCH_XINPUT_RIGHT_SHOULDER, 0U, 1U));
    reports_clear();
    CHECK(kage_vita_touch_sample());
    CHECK(snapshot_is(KAGE_VITA_TOUCH_ACTION_NONE, 0U, 0U, 2U));

    /* Reversing two fresh, conflicting report orders produces byte-identical
     * effective state. */
    reports_clear();
    report_add(8U, 4600, -400);
    report_add(3U, 4600, 1300);
    CHECK(kage_vita_touch_sample());
    CHECK(kage_vita_touch_snapshot_read(&first_order));
    CHECK(first_order.actions == (KAGE_VITA_TOUCH_ACTION_PILL_CARD |
                                  KAGE_VITA_TOUCH_ACTION_MAP));
    reports_clear();
    CHECK(kage_vita_touch_sample());
    reports_clear();
    report_add(3U, 4600, 1300);
    report_add(8U, 4600, -400);
    CHECK(kage_vita_touch_sample());
    CHECK(kage_vita_touch_snapshot_read(&reverse_order));
    CHECK(first_order.actions == reverse_order.actions &&
          first_order.xinput_buttons == reverse_order.xinput_buttons &&
          first_order.xinput_left_trigger ==
              reverse_order.xinput_left_trigger);

    /* Malformed duplicate IDs are also order-independent: sorting chooses
     * the lexicographically first touchdown once, then pins it. */
    reports_clear();
    CHECK(kage_vita_touch_sample());
    reports_clear();
    report_add(4U, 4600, 1300);
    report_add(4U, 4600, -400);
    CHECK(kage_vita_touch_sample());
    CHECK((snapshot_is(KAGE_VITA_TOUCH_ACTION_MAP,
                       KAGE_VITA_TOUCH_XINPUT_BACK, 0U,
                       reverse_order.packet_number + 2U)));
    reports_clear();
    CHECK(kage_vita_touch_sample());
    reports_clear();
    report_add(4U, 4600, -400);
    report_add(4U, 4600, 1300);
    CHECK(kage_vita_touch_sample());
    CHECK(snapshot_is(KAGE_VITA_TOUCH_ACTION_MAP,
                      KAGE_VITA_TOUCH_XINPUT_BACK, 0U,
                      reverse_order.packet_number + 4U));

    /* Top-left active is LT.  It combines with simultaneous top-right map and
     * bottom-right pocket contacts without losing any held level. */
    reports_clear();
    CHECK(kage_vita_touch_sample());
    reports_clear();
    report_add(6U, 1200, -400);
    CHECK(kage_vita_touch_sample());
    CHECK(snapshot_is(KAGE_VITA_TOUCH_ACTION_ACTIVE,
                      0U, KAGE_VITA_TOUCH_XINPUT_TRIGGER_FULL,
                      reverse_order.packet_number + 6U));
    reports_clear();
    report_add(6U, 1200, -400);
    report_add(2U, 4600, -400);
    report_add(9U, 4600, 1300);
    CHECK(kage_vita_touch_sample());
    CHECK(snapshot_is(KAGE_VITA_TOUCH_ACTION_ACTIVE |
                      KAGE_VITA_TOUCH_ACTION_MAP |
                      KAGE_VITA_TOUCH_ACTION_PILL_CARD,
                      KAGE_VITA_TOUCH_XINPUT_BACK |
                      KAGE_VITA_TOUCH_XINPUT_RIGHT_SHOULDER,
                      KAGE_VITA_TOUCH_XINPUT_TRIGGER_FULL,
                      reverse_order.packet_number + 7U));

    /* Reinitialization stops the old producer and clears every pin before
     * starting a fresh front-panel session. */
    sampling_before = s_sampling_calls;
    CHECK(kage_vita_touch_initialize());
    CHECK(s_sampling_calls == sampling_before + 2U &&
          s_sampling_log[sampling_before].state ==
              SCE_TOUCH_SAMPLING_STATE_STOP &&
          s_sampling_log[sampling_before + 1U].state ==
              SCE_TOUCH_SAMPLING_STATE_START &&
          snapshot_is(KAGE_VITA_TOUCH_ACTION_NONE, 0U, 0U, 0U));
    CHECK(all_recorded_ports_are_front());
    kage_vita_touch_deactivate();
    CHECK(snapshot_unavailable());

    puts("Vita front-touch panel-normalization/pinning/XInput oracle: PASS");
    return 0;
}
