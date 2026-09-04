/* Executable host oracle for cached sceCtrl sampling and GLFW translation. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kage_vita_input.h"
#include "kage_vita_input_test_ctrl.h"
#include "kage_vita_touch.h"

static int s_mode_result;
static int s_peek_result = 1;
static uint32_t s_next_buttons;
static uint8_t s_next_lx = 128u;
static uint8_t s_next_ly = 128u;
static uint8_t s_next_rx = 128u;
static uint8_t s_next_ry = 128u;
static unsigned s_mode_calls;
static unsigned s_peek_calls;
static unsigned s_log_calls;
static unsigned s_keyboard_log_calls;
static unsigned s_xinput_log_calls;
static unsigned s_printf_reentry_calls;
static unsigned s_printf_reentry_successes;
static unsigned s_time_calls;
static uint64_t s_now_us;
static int s_inside_printf;
static SceCtrlPadInputMode s_last_mode;
static char s_last_log[256];
static char s_last_keyboard_log[256];
static char s_last_xinput_log[256];
static int s_touch_initialize_result = 1;
static int s_touch_sample_result = 1;
static int s_touch_available;
static unsigned s_touch_initialize_calls;
static unsigned s_touch_deactivate_calls;
static unsigned s_touch_sample_calls;
static kage_vita_touch_snapshot s_touch_snapshot;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Vita input oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int sceCtrlSetSamplingMode(SceCtrlPadInputMode mode)
{
    ++s_mode_calls;
    s_last_mode = mode;
    return s_mode_result;
}

int sceCtrlPeekBufferPositive(int port, SceCtrlData *pad_data, int count)
{
    ++s_peek_calls;
    if (s_peek_result == 1 && port == 0 && pad_data && count == 1) {
        memset(pad_data, 0, sizeof *pad_data);
        pad_data->buttons = s_next_buttons;
        pad_data->lx = s_next_lx;
        pad_data->ly = s_next_ly;
        pad_data->rx = s_next_rx;
        pad_data->ry = s_next_ry;
    }
    return s_peek_result;
}

int kage_vita_touch_initialize(void)
{
    ++s_touch_initialize_calls;
    memset(&s_touch_snapshot, 0, sizeof s_touch_snapshot);
    s_touch_available = s_touch_initialize_result != 0;
    return s_touch_available;
}

void kage_vita_touch_deactivate(void)
{
    ++s_touch_deactivate_calls;
    s_touch_available = 0;
    memset(&s_touch_snapshot, 0, sizeof s_touch_snapshot);
}

int kage_vita_touch_sample(void)
{
    ++s_touch_sample_calls;
    if (!s_touch_available || !s_touch_sample_result) {
        memset(&s_touch_snapshot, 0, sizeof s_touch_snapshot);
        return 0;
    }
    return 1;
}

int kage_vita_touch_snapshot_read(kage_vita_touch_snapshot *snapshot)
{
    if (!snapshot)
        return 0;
    memset(snapshot, 0, sizeof *snapshot);
    if (!s_touch_available)
        return 0;
    *snapshot = s_touch_snapshot;
    return 1;
}

uint64_t sceKernelGetProcessTimeWide(void)
{
    ++s_time_calls;
    return s_now_us;
}

int sceClibPrintf(const char *format, ...)
{
    kage_vita_gamepad_snapshot reentry_snapshot;
    va_list arguments;
    int result;

    ++s_log_calls;
    va_start(arguments, format);
    result = vsnprintf(s_last_log, sizeof s_last_log, format, arguments);
    va_end(arguments);
    if (strstr(s_last_log, "] xinput snapshot ")) {
        ++s_xinput_log_calls;
        snprintf(s_last_xinput_log, sizeof s_last_xinput_log, "%s",
                 s_last_log);
    } else if (strstr(s_last_log, "] snapshot ")) {
        ++s_keyboard_log_calls;
        snprintf(s_last_keyboard_log, sizeof s_last_keyboard_log, "%s",
                 s_last_log);
    }

    /* Re-enter the cache consumer from the logger.  This terminates only if
     * production released its spin lock before sceClibPrintf, and the
     * already-consumed raw generation prevents a recursive log line. */
    if (!s_inside_printf) {
        s_inside_printf = 1;
        ++s_printf_reentry_calls;
        if (kage_vita_input_gamepad_snapshot(&reentry_snapshot))
            ++s_printf_reentry_successes;
        s_inside_printf = 0;
    }
    return result;
}

static int snapshot(uint8_t keys[KAGE_PC_KEY_COUNT])
{
    memset(keys, 0xcc, KAGE_PC_KEY_COUNT);
    kage_vita_input_keyboard_snapshot(keys);
    return keys[257] != 0u;
}

int main(void)
{
    uint8_t guarded[KAGE_PC_KEY_COUNT + 2u];
    uint8_t empty[KAGE_PC_KEY_COUNT];
    uint8_t previous[KAGE_PC_KEY_COUNT];
    uint8_t current[KAGE_PC_KEY_COUNT];
    kage_vita_gamepad_snapshot gamepad;
    unsigned peek_before;
    unsigned packet_before;
    unsigned time_before;

    memset(empty, 0, sizeof empty);

    s_mode_result = -1;
    CHECK(!kage_vita_input_initialize());
    CHECK(s_mode_calls == 1u && s_last_mode == SCE_CTRL_MODE_ANALOG_WIDE);
    CHECK(!kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.packet_number == 0u && gamepad.buttons == 0u &&
          gamepad.lx == 128u && gamepad.ly == 128u &&
          gamepad.rx == 128u && gamepad.ry == 128u);
    CHECK(!snapshot(current));
    CHECK(s_peek_calls == 0u && s_log_calls == 0u);
    CHECK(s_keyboard_log_calls == 0u && s_xinput_log_calls == 0u);

    s_mode_result = 0;
    CHECK(kage_vita_input_initialize());
    CHECK(s_mode_calls == 2u && s_last_mode == SCE_CTRL_MODE_ANALOG_WIDE);
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.packet_number == 0u && gamepad.buttons == 0u);
    memset(guarded, 0xa5, sizeof guarded);
    kage_vita_input_keyboard_snapshot(guarded + 1u);
    CHECK(guarded[0] == 0xa5u &&
          guarded[KAGE_PC_KEY_COUNT + 1u] == 0xa5u);
    CHECK(guarded[1u + 257u] == 0u);

    s_next_buttons = SCE_CTRL_START;
    CHECK(kage_vita_input_sample());
    CHECK(s_peek_calls == 1u && s_log_calls == 0u);
    memcpy(previous, current, sizeof previous);
    CHECK(snapshot(current));
    CHECK(!previous[257] && current[257]);
    CHECK(s_log_calls == 1u);
    CHECK(s_keyboard_log_calls == 1u && s_xinput_log_calls == 0u);
    CHECK(strstr(s_last_log,
                 "seq=1 buttons=00000000->00000008") != NULL);
    CHECK(strstr(s_last_log, "enter=1") != NULL);
    peek_before = s_peek_calls;
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.packet_number == 1u &&
          gamepad.buttons == SCE_CTRL_START &&
          gamepad.lx == 128u && gamepad.ly == 128u &&
          gamepad.rx == 128u && gamepad.ry == 128u);
    CHECK(s_peek_calls == peek_before);

    peek_before = s_peek_calls;
    CHECK(snapshot(current));
    CHECK(snapshot(current));
    CHECK(s_peek_calls == peek_before && s_log_calls == 1u);

    kage_vita_input_set_xinput_active(1);
    CHECK(!snapshot(current));
    CHECK(memcmp(current, empty, sizeof empty) == 0);
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.packet_number == 2u && gamepad.buttons == 0U);
    CHECK(s_peek_calls == peek_before && s_log_calls == 1u);
    CHECK(s_xinput_log_calls == 0u);
    kage_vita_input_set_xinput_active(0);
    CHECK(!snapshot(current));

    CHECK(kage_vita_input_sample());
    CHECK(s_peek_calls == peek_before + 1u && s_log_calls == 1u);
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.packet_number == 3u);

    s_next_buttons = SCE_CTRL_START | SCE_CTRL_CROSS;
    CHECK(kage_vita_input_sample());
    memcpy(previous, current, sizeof previous);
    CHECK(snapshot(current));
    CHECK(!previous[257] && current[257]);
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.packet_number == 4u &&
          gamepad.buttons == (SCE_CTRL_START | SCE_CTRL_CROSS));
    CHECK(strstr(s_last_log,
                 "seq=4 buttons=00000008->00004008") != NULL);
    CHECK(strstr(s_last_log, "enter=1") != NULL);

    s_next_buttons = SCE_CTRL_CROSS;
    CHECK(kage_vita_input_sample());
    memcpy(previous, current, sizeof previous);
    CHECK(snapshot(current));
    CHECK(previous[257] && current[257]);

    s_next_buttons = 0u;
    CHECK(kage_vita_input_sample());
    memcpy(previous, current, sizeof previous);
    CHECK(!snapshot(current));
    CHECK(previous[257] && !current[257]);
    CHECK(strstr(s_last_log,
                 "seq=6 buttons=00004000->00000000") != NULL);
    CHECK(strstr(s_last_log, "enter=0") != NULL);

    s_next_buttons = SCE_CTRL_CROSS;
    CHECK(kage_vita_input_sample());
    memcpy(previous, current, sizeof previous);
    CHECK(snapshot(current));
    CHECK(!previous[257] && current[257]);

    s_peek_result = -1;
    s_next_buttons = 0u;
    CHECK(!kage_vita_input_sample());
    CHECK(snapshot(current));
    CHECK(s_log_calls == 5u);
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.packet_number == 7u &&
          gamepad.buttons == SCE_CTRL_CROSS);

    /* A second transient miss retains the coherent held state too.  The
     * configured third consecutive miss publishes one neutral sample. */
    CHECK(!kage_vita_input_sample());
    CHECK(snapshot(current));
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.packet_number == 7u &&
          gamepad.buttons == SCE_CTRL_CROSS);
    CHECK(!kage_vita_input_sample());
    CHECK(!snapshot(current));
    CHECK(s_log_calls == 6u);
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.packet_number == 8u && gamepad.buttons == 0u &&
          gamepad.lx == 128u && gamepad.ly == 128u &&
          gamepad.rx == 128u && gamepad.ry == 128u);
    CHECK(!kage_vita_input_sample());
    CHECK(!snapshot(current));
    CHECK(s_log_calls == 6u &&
          KAGE_VITA_INPUT_FAILURE_NEUTRALIZE_COUNT == 3u);

    /* A successful peek clears the failure streak and can press again. */
    s_peek_result = 1;
    s_next_buttons = SCE_CTRL_CROSS;
    CHECK(kage_vita_input_sample());
    CHECK(snapshot(current));
    CHECK(s_log_calls == 7u);
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.packet_number == 9u &&
          gamepad.buttons == SCE_CTRL_CROSS);

    /* LoadLibraryA owns XInput once per guest module lifetime.  KAGE's
     * native backend deliberately supports Shutdown -> Initialize without
     * replaying Gamepad_init, so this latch must survive input deactivation. */
    kage_vita_input_set_xinput_active(1);
    CHECK(!snapshot(current));
    CHECK(memcmp(current, empty, sizeof empty) == 0);

    peek_before = s_peek_calls;
    kage_vita_input_deactivate();
    CHECK(!kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(!snapshot(current));
    CHECK(!kage_vita_input_sample());
    CHECK(s_peek_calls == peek_before && s_log_calls == 7u);
    CHECK(s_keyboard_log_calls == 7u && s_xinput_log_calls == 0u);

    s_next_lx = s_next_ly = s_next_rx = s_next_ry = 128u;
    CHECK(kage_vita_input_initialize());
    CHECK(kage_vita_input_sample());
    CHECK(!snapshot(current));
    CHECK(memcmp(current, empty, sizeof empty) == 0);
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.packet_number == 1u &&
          gamepad.buttons == SCE_CTRL_CROSS);
    CHECK(s_log_calls == 8u && s_mode_calls == 3u);
    CHECK(s_keyboard_log_calls == 7u && s_xinput_log_calls == 1u);
    CHECK(strstr(s_last_xinput_log,
                 "xinput snapshot seq=1 buttons=00000000->00004000") !=
          NULL);
    CHECK(strstr(s_last_xinput_log,
                 "down=00004000 up=00000000") != NULL);
    CHECK(strstr(s_last_xinput_log,
                 "sticks=128,128,128,128->128,128,128,128") != NULL);

    /* A second guest poll of the same packet is silent.  A raw release gets
     * one new XInput line, while the keyboard log's independent consumed
     * counter remains untouched under legacy-keyboard suppression. */
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(s_log_calls == 8u && s_xinput_log_calls == 1u);
    CHECK(kage_vita_input_sample());
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(s_log_calls == 8u && s_xinput_log_calls == 1u);

    s_next_buttons = 0u;
    CHECK(kage_vita_input_sample());
    CHECK(!snapshot(current));
    CHECK(memcmp(current, empty, sizeof empty) == 0);
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.packet_number == 2u && gamepad.buttons == 0u);
    CHECK(s_log_calls == 9u && s_xinput_log_calls == 2u);
    CHECK(s_keyboard_log_calls == 7u);
    CHECK(strstr(s_last_xinput_log,
                 "xinput snapshot seq=2 buttons=00004000->00000000") !=
          NULL);
    CHECK(strstr(s_last_xinput_log,
                 "down=00000000 up=00004000") != NULL);
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(s_log_calls == 9u && s_xinput_log_calls == 2u);

    s_next_lx = 63u;
    s_next_ly = 193u;
    s_next_rx = 193u;
    s_next_ry = 63u;
    CHECK(kage_vita_input_sample());
    CHECK(!snapshot(current));
    CHECK(memcmp(current, empty, sizeof empty) == 0);
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.packet_number == 3u && gamepad.buttons == 0u &&
          gamepad.lx == 63u && gamepad.ly == 193u &&
          gamepad.rx == 193u && gamepad.ry == 63u);
    CHECK(s_log_calls == 9u && s_xinput_log_calls == 2u);
    CHECK(s_keyboard_log_calls == 7u);
    CHECK(strstr(s_last_xinput_log,
                 "xinput snapshot seq=2 buttons=00004000->00000000") !=
          NULL);
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(s_log_calls == 9u && s_xinput_log_calls == 2u);

    /* Packet semantics retain every raw stick change, but all analog-only
     * packets are silent even when an axis crosses a gameplay deadzone. */
    s_next_lx = 62u;
    CHECK(kage_vita_input_sample());
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.packet_number == 4u && gamepad.lx == 62u &&
          gamepad.ly == 193u && gamepad.rx == 193u &&
          gamepad.ry == 63u);
    CHECK(s_log_calls == 9u && s_xinput_log_calls == 2u);

    kage_vita_input_set_xinput_active(0);
    CHECK(!snapshot(current));
    CHECK(s_keyboard_log_calls == 7u && s_log_calls == 9u);

    s_next_buttons = SCE_CTRL_SELECT | SCE_CTRL_CIRCLE |
                     SCE_CTRL_SQUARE | SCE_CTRL_TRIANGLE |
                     SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER |
                     SCE_CTRL_UP | SCE_CTRL_RIGHT;
    s_next_lx = s_next_ly = s_next_rx = s_next_ry = 128u;
    CHECK(kage_vita_input_sample());
    (void)snapshot(current);
    CHECK(!current[258] && current[256]);
    CHECK(current['E'] && current['Q'] && current[32] && current[341]);
    CHECK(current[265] && current[262]);
    CHECK(!current[257] && !current[263] && !current[264]);

    s_next_buttons = 0u;
    s_next_lx = s_next_ly = s_next_rx = s_next_ry = 128u;
    CHECK(kage_vita_input_sample());
    CHECK(!snapshot(current));

    s_next_lx = 64u;
    s_next_ly = 192u;
    s_next_rx = 64u;
    s_next_ry = 192u;
    CHECK(kage_vita_input_sample());
    CHECK(!snapshot(current));

    s_next_lx = 63u;
    s_next_ly = 193u;
    s_next_rx = 193u;
    s_next_ry = 63u;
    CHECK(kage_vita_input_sample());
    (void)snapshot(current);
    CHECK(current['A'] && current['S']);
    CHECK(!current['D'] && !current['W']);
    CHECK(current[262] && current[265]);
    CHECK(!current[263] && !current[264]);

    s_next_buttons = SCE_CTRL_RIGHT | SCE_CTRL_UP;
    s_next_rx = 63u;
    s_next_ry = 193u;
    CHECK(kage_vita_input_sample());
    (void)snapshot(current);
    CHECK(current[262] && current[263]);
    CHECK(current[264] && current[265]);
    CHECK(strstr(s_last_log, "arrows=1111") != NULL);

    s_next_buttons = 0u;
    s_next_lx = 193u;
    s_next_ly = 63u;
    s_next_rx = 63u;
    s_next_ry = 193u;
    CHECK(kage_vita_input_sample());
    (void)snapshot(current);
    CHECK(current['D'] && current['W']);
    CHECK(!current['A'] && !current['S']);
    CHECK(current[263] && current[264]);
    CHECK(!current[262] && !current[265]);

    CHECK(s_printf_reentry_calls == s_log_calls);
    CHECK(s_printf_reentry_successes == s_log_calls);
    CHECK(s_keyboard_log_calls + s_xinput_log_calls == s_log_calls);
    CHECK(strstr(s_last_keyboard_log, "[kage-vita-input] snapshot ") !=
          NULL);

    /* XInput owns the pad in production.  Start is a lossless prefix: its
     * short release becomes one pause packet, its shoulders become synthetic
     * guest bindings, and its held level is the one permitted keyboard-R
     * lane.  No constituent may leak as bomb/pill/pause. */
    kage_vita_input_deactivate();
    s_next_buttons = 0U;
    s_next_lx = s_next_ly = s_next_rx = s_next_ry = 128U;
    s_peek_result = 1;
    CHECK(kage_vita_input_initialize());
    kage_vita_input_set_xinput_active(1);
    CHECK(kage_vita_input_sample());
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.buttons == 0U && gamepad.xinput_buttons == 0U &&
          gamepad.xinput_left_trigger == 0U);

    time_before = s_time_calls;
    s_now_us = UINT64_C(1000000);
    s_next_buttons = SCE_CTRL_START;
    CHECK(kage_vita_input_sample());
    CHECK(s_time_calls == time_before + 1U);
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.buttons == 0U && gamepad.xinput_buttons == 0U &&
          gamepad.xinput_left_trigger == 0U);
    kage_vita_input_keyboard_snapshot(current);
    CHECK(!current['R'] && !current[257] && !current['E'] &&
          !current['Q']);

    /* Releasing a short Start tap drops R and emits exactly one deferred
     * Start packet.  Reading the monotonic clock on release is unnecessary. */
    s_next_buttons = 0U;
    CHECK(kage_vita_input_sample());
    CHECK(s_time_calls == time_before + 1U);
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.buttons == SCE_CTRL_START &&
          gamepad.xinput_buttons == 0U &&
          gamepad.xinput_left_trigger == 0U);
    kage_vita_input_keyboard_snapshot(current);
    CHECK(!current['R'] && !current[257]);
    CHECK(kage_vita_input_sample());
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.buttons == 0U);

    /* Start first, then L: publish only the frozen RB pill/card binding.
     * Releasing Start first must keep L consumed until its own release. */
    s_now_us = UINT64_C(2000000);
    s_next_buttons = SCE_CTRL_START;
    CHECK(kage_vita_input_sample());
    s_now_us += UINT64_C(1000);
    s_next_buttons = SCE_CTRL_START | SCE_CTRL_LTRIGGER;
    CHECK(kage_vita_input_sample());
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    packet_before = gamepad.packet_number;
    CHECK(gamepad.buttons == 0U &&
          gamepad.xinput_buttons ==
              KAGE_VITA_INPUT_XINPUT_RIGHT_SHOULDER &&
          gamepad.xinput_left_trigger == 0U);
    kage_vita_input_keyboard_snapshot(current);
    CHECK(!current['R'] && !current[257] && !current['E']);
    CHECK(kage_vita_input_sample());
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.packet_number == packet_before);
    s_next_buttons = SCE_CTRL_LTRIGGER;
    CHECK(kage_vita_input_sample());
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.buttons == 0U && gamepad.xinput_buttons == 0U &&
          gamepad.xinput_left_trigger == 0U);
    s_peek_result = -1;
    CHECK(!kage_vita_input_sample());
    CHECK(!kage_vita_input_sample());
    CHECK(!kage_vita_input_sample());
    s_peek_result = 1;
    s_next_buttons = SCE_CTRL_LTRIGGER;
    CHECK(kage_vita_input_sample());
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.buttons == 0U && gamepad.xinput_buttons == 0U &&
          gamepad.xinput_left_trigger == 0U);
    s_next_buttons = 0U;
    CHECK(kage_vita_input_sample());
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.buttons == 0U);

    /* Start first, then R: publish only BACK/map.  Physical R outside the
     * chord remains raw R for downstream LT/active mapping and never becomes
     * keyboard R. */
    s_now_us = UINT64_C(3000000);
    s_next_buttons = SCE_CTRL_START;
    CHECK(kage_vita_input_sample());
    s_now_us += UINT64_C(1000);
    s_next_buttons = SCE_CTRL_START | SCE_CTRL_RTRIGGER;
    CHECK(kage_vita_input_sample());
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.buttons == 0U &&
          gamepad.xinput_buttons == KAGE_VITA_INPUT_XINPUT_BACK &&
          gamepad.xinput_left_trigger == 0U);
    kage_vita_input_keyboard_snapshot(current);
    CHECK(!current['R'] && !current[257] && !current['Q']);
    s_next_buttons = SCE_CTRL_START;
    CHECK(kage_vita_input_sample());
    s_next_buttons = 0U;
    CHECK(kage_vita_input_sample());
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.buttons == 0U && gamepad.xinput_buttons == 0U);

    s_next_buttons = SCE_CTRL_RTRIGGER;
    CHECK(kage_vita_input_sample());
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.buttons == SCE_CTRL_RTRIGGER &&
          gamepad.xinput_buttons == 0U &&
          gamepad.xinput_left_trigger == 0U);
    kage_vita_input_keyboard_snapshot(current);
    CHECK(!current['R']);
    s_next_buttons = 0U;
    CHECK(kage_vita_input_sample());

    /* Both shoulders are an invalid chord and are consumed together. */
    s_now_us = UINT64_C(4000000);
    s_next_buttons = SCE_CTRL_START | SCE_CTRL_LTRIGGER |
                     SCE_CTRL_RTRIGGER;
    CHECK(kage_vita_input_sample());
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.buttons == 0U && gamepad.xinput_buttons == 0U &&
          gamepad.xinput_left_trigger == 0U);
    kage_vita_input_keyboard_snapshot(current);
    CHECK(!current['R'] && !current[257]);
    s_next_buttons = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER;
    CHECK(kage_vita_input_sample());
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.buttons == 0U);
    s_next_buttons = 0U;
    CHECK(kage_vita_input_sample());

    /* At 800 ms the prefix becomes restart-only and starts keyboard R.  The
     * untouched guest still owns its true restart duration, while release
     * can no longer also pause. */
    s_now_us = UINT64_C(5000000);
    s_next_buttons = SCE_CTRL_START;
    CHECK(kage_vita_input_sample());
    kage_vita_input_keyboard_snapshot(current);
    CHECK(!current['R'] && !current[257]);
    s_now_us += KAGE_VITA_INPUT_START_HOLD_US - UINT64_C(1);
    CHECK(kage_vita_input_sample());
    kage_vita_input_keyboard_snapshot(current);
    CHECK(!current['R'] && !current[257]);
    s_now_us += UINT64_C(1);
    CHECK(kage_vita_input_sample());
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.buttons == 0U && gamepad.xinput_buttons == 0U &&
          gamepad.xinput_left_trigger == 0U);
    kage_vita_input_keyboard_snapshot(current);
    CHECK(current['R'] && !current[257]);
    s_next_buttons = 0U;
    CHECK(kage_vita_input_sample());
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.buttons == 0U);
    kage_vita_input_keyboard_snapshot(current);
    CHECK(!current['R'] && !current[257]);

    /* Module ownership changes sanitize cached synthetic state immediately;
     * neither a pending trigger nor held keyboard R may survive FreeLibrary
     * or reappear on a re-enable before the next producer sample. */
    s_now_us = UINT64_C(6000000);
    s_next_buttons = SCE_CTRL_START | SCE_CTRL_LTRIGGER;
    CHECK(kage_vita_input_sample());
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.xinput_buttons ==
          KAGE_VITA_INPUT_XINPUT_RIGHT_SHOULDER);
    kage_vita_input_set_xinput_active(0);
    kage_vita_input_keyboard_snapshot(current);
    CHECK(!current['R'] && !current[257]);
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.xinput_left_trigger == 0U &&
          gamepad.xinput_buttons == 0U && gamepad.buttons == 0U);
    kage_vita_input_set_xinput_active(1);
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.xinput_left_trigger == 0U &&
          gamepad.xinput_buttons == 0U && gamepad.buttons == 0U);

    s_now_us = UINT64_C(7000000);
    s_next_buttons = SCE_CTRL_START;
    CHECK(kage_vita_input_sample());
    s_now_us += KAGE_VITA_INPUT_START_HOLD_US;
    CHECK(kage_vita_input_sample());
    kage_vita_input_keyboard_snapshot(current);
    CHECK(current['R'] && !current[257]);
    kage_vita_input_set_xinput_active(0);
    kage_vita_input_keyboard_snapshot(current);
    CHECK(!current['R'] && !current[257]);
    kage_vita_input_set_xinput_active(1);
    kage_vita_input_keyboard_snapshot(current);
    CHECK(!current['R'] && !current[257]);
    s_next_buttons = 0U;
    CHECK(kage_vita_input_sample());
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.buttons == 0U && gamepad.xinput_buttons == 0U &&
          gamepad.xinput_left_trigger == 0U);

    /* Front touch is XInput-only and starts disarmed.  A contact inherited
     * from LiveArea/module startup cannot become an action until one neutral
     * sample has established a fresh-touch boundary. */
    kage_vita_input_deactivate();
    s_next_buttons = 0U;
    s_next_lx = s_next_ly = s_next_rx = s_next_ry = 128U;
    s_peek_result = 1;
    s_touch_initialize_result = 1;
    s_touch_sample_result = 1;
    CHECK(kage_vita_input_initialize());
    s_touch_snapshot.actions = KAGE_VITA_TOUCH_ACTION_ACTIVE;
    s_touch_snapshot.xinput_left_trigger =
        KAGE_VITA_TOUCH_XINPUT_TRIGGER_FULL;
    CHECK(kage_vita_input_sample());
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.xinput_buttons == 0U &&
          gamepad.xinput_left_trigger == 0U);
    kage_vita_input_set_xinput_active(1);
    CHECK(kage_vita_input_sample());
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.xinput_buttons == 0U &&
          gamepad.xinput_left_trigger == 0U);
    memset(&s_touch_snapshot, 0, sizeof s_touch_snapshot);
    CHECK(kage_vita_input_sample());

    /* Down/hold/up are levels.  Active is LT; map and pocket are the Back and
     * RB bits, and all three contacts can coexist in one coherent packet. */
    s_touch_snapshot.actions = KAGE_VITA_TOUCH_ACTION_ACTIVE;
    s_touch_snapshot.xinput_left_trigger =
        KAGE_VITA_TOUCH_XINPUT_TRIGGER_FULL;
    CHECK(kage_vita_input_sample());
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    packet_before = gamepad.packet_number;
    CHECK(gamepad.xinput_buttons == 0U &&
          gamepad.xinput_left_trigger ==
              KAGE_VITA_INPUT_XINPUT_TRIGGER_FULL);
    CHECK(kage_vita_input_sample());
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.packet_number == packet_before);

    s_touch_snapshot.actions = KAGE_VITA_TOUCH_ACTION_ACTIVE |
                               KAGE_VITA_TOUCH_ACTION_MAP |
                               KAGE_VITA_TOUCH_ACTION_PILL_CARD;
    s_touch_snapshot.xinput_buttons = KAGE_VITA_TOUCH_XINPUT_BACK |
                                      KAGE_VITA_TOUCH_XINPUT_RIGHT_SHOULDER;
    CHECK(kage_vita_input_sample());
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.xinput_buttons ==
              (KAGE_VITA_INPUT_XINPUT_BACK |
               KAGE_VITA_INPUT_XINPUT_RIGHT_SHOULDER) &&
          gamepad.xinput_left_trigger ==
              KAGE_VITA_INPUT_XINPUT_TRIGGER_FULL);
    memset(&s_touch_snapshot, 0, sizeof s_touch_snapshot);
    CHECK(kage_vita_input_sample());
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.xinput_buttons == 0U &&
          gamepad.xinput_left_trigger == 0U);

    /* A missing touch snapshot releases immediately and disarms.  A contact
     * still held when sampling recovers stays suppressed until release. */
    s_touch_snapshot.actions = KAGE_VITA_TOUCH_ACTION_ACTIVE;
    s_touch_snapshot.xinput_left_trigger =
        KAGE_VITA_TOUCH_XINPUT_TRIGGER_FULL;
    CHECK(kage_vita_input_sample());
    s_touch_sample_result = 0;
    CHECK(kage_vita_input_sample());
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.xinput_buttons == 0U &&
          gamepad.xinput_left_trigger == 0U);
    s_touch_sample_result = 1;
    s_touch_snapshot.actions = KAGE_VITA_TOUCH_ACTION_ACTIVE;
    s_touch_snapshot.xinput_left_trigger =
        KAGE_VITA_TOUCH_XINPUT_TRIGGER_FULL;
    CHECK(kage_vita_input_sample());
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.xinput_left_trigger == 0U);
    memset(&s_touch_snapshot, 0, sizeof s_touch_snapshot);
    CHECK(kage_vita_input_sample());
    s_touch_snapshot.actions = KAGE_VITA_TOUCH_ACTION_ACTIVE;
    s_touch_snapshot.xinput_left_trigger =
        KAGE_VITA_TOUCH_XINPUT_TRIGGER_FULL;
    CHECK(kage_vita_input_sample());
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.xinput_left_trigger ==
          KAGE_VITA_INPUT_XINPUT_TRIGGER_FULL);

    /* Touch release is not delayed by the pad producer's transient two-peek
     * grace period, and deactivation makes both caches unavailable. */
    memset(&s_touch_snapshot, 0, sizeof s_touch_snapshot);
    s_peek_result = -1;
    CHECK(!kage_vita_input_sample());
    CHECK(kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(gamepad.xinput_buttons == 0U &&
          gamepad.xinput_left_trigger == 0U);
    s_peek_result = 1;
    CHECK(kage_vita_input_sample());
    kage_vita_input_deactivate();
    CHECK(!kage_vita_input_gamepad_snapshot(&gamepad));
    CHECK(!s_touch_available);

    puts("Vita input cache/level-edge/translation oracle: PASS");
    return 0;
}
