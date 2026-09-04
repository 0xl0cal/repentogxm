/* Cached Vita controller producer for the original KAGE DeviceKeyboard path.
 *
 * sceCtrl must not be called from kage_pc_backend_keyboard_snapshot(): the
 * guest may poll that consumer far more often than it produces frames.  One
 * main-loop hook samples the kernel API, publishes a coherent 349-key level
 * snapshot, and the untouched guest current/previous buffers detect edges. */
#include "kage_vita_input.h"
#include "kage_vita_touch.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(ISAAC_KAGE_VITA_INPUT_ORACLE)
# include "kage_vita_input_test_ctrl.h"
#else
# include <psp2/ctrl.h>
# include <psp2/kernel/clib.h>
# include <psp2/kernel/processmgr.h>
#endif
/* Transition telemetry runs on the game thread inside the frame; with
 * ISAAC_VITA_LOG_ASYNC the logger thread performs the kernel printf. */
#if defined(ISAAC_VITA_LOG_ASYNC)
# include "host_vita_log_async.h"
#else
# define ISAAC_VITA_LOG_PRINTF sceClibPrintf
#endif

enum {
    KAGE_VITA_GLFW_SPACE = 32,
    KAGE_VITA_GLFW_A = 65,
    KAGE_VITA_GLFW_D = 68,
    KAGE_VITA_GLFW_E = 69,
    KAGE_VITA_GLFW_Q = 81,
    KAGE_VITA_GLFW_R = 82,
    KAGE_VITA_GLFW_S = 83,
    KAGE_VITA_GLFW_W = 87,
    KAGE_VITA_GLFW_ESCAPE = 256,
    KAGE_VITA_GLFW_ENTER = 257,
    KAGE_VITA_GLFW_TAB = 258,
    KAGE_VITA_GLFW_RIGHT = 262,
    KAGE_VITA_GLFW_LEFT = 263,
    KAGE_VITA_GLFW_DOWN = 264,
    KAGE_VITA_GLFW_UP = 265,
    KAGE_VITA_GLFW_LEFT_CONTROL = 341,
    KAGE_VITA_ANALOG_CENTER = 128,
    KAGE_VITA_ANALOG_THRESHOLD = 64,
    KAGE_VITA_ANALOG_LOW = KAGE_VITA_ANALOG_CENTER -
                           KAGE_VITA_ANALOG_THRESHOLD,
    KAGE_VITA_ANALOG_HIGH = KAGE_VITA_ANALOG_CENTER +
                            KAGE_VITA_ANALOG_THRESHOLD
};

_Static_assert(KAGE_VITA_GLFW_LEFT_CONTROL < KAGE_PC_KEY_COUNT,
               "Vita input token exceeds the original 349-key domain");
_Static_assert(KAGE_VITA_GLFW_R < KAGE_PC_KEY_COUNT,
               "restart token exceeds the original 349-key domain");
_Static_assert(sizeof(SceCtrlData) == 0x20u,
               "Vita input oracle/SDK SceCtrlData layout changed");
_Static_assert(KAGE_VITA_TOUCH_XINPUT_BACK ==
                   KAGE_VITA_INPUT_XINPUT_BACK,
               "touch/map XInput Back token drifted");
_Static_assert(KAGE_VITA_TOUCH_XINPUT_RIGHT_SHOULDER ==
                   KAGE_VITA_INPUT_XINPUT_RIGHT_SHOULDER,
               "touch/pocket XInput right-shoulder token drifted");
_Static_assert(KAGE_VITA_TOUCH_XINPUT_TRIGGER_FULL ==
                   KAGE_VITA_INPUT_XINPUT_TRIGGER_FULL,
               "touch/active XInput trigger level drifted");

typedef struct kage_vita_input_map {
    uint32_t buttons;
    uint16_t key;
} kage_vita_input_map;

/* Enter/Escape/arrows/WASD/E/Q/Space are pinned by PC automation in the
 * original GLFW token domain.  LeftControl is the exact GLFW token for the
 * keyboard drop policy.  Select deliberately has no keyboard mapping: once
 * XInput owns the pad it is only RT/Drop, while map is Start+R or front touch.
 * Start/Cross is a level OR for confirmation.  Cross and Circle deliberately
 * do not double as shooting arrows: that would combine navigation with
 * confirm/cancel whenever the menu is active. */
static const kage_vita_input_map s_button_map[] = {
    { SCE_CTRL_START | SCE_CTRL_CROSS, KAGE_VITA_GLFW_ENTER },
    { SCE_CTRL_CIRCLE, KAGE_VITA_GLFW_ESCAPE },
    { SCE_CTRL_LEFT, KAGE_VITA_GLFW_LEFT },
    { SCE_CTRL_RIGHT, KAGE_VITA_GLFW_RIGHT },
    { SCE_CTRL_DOWN, KAGE_VITA_GLFW_DOWN },
    { SCE_CTRL_UP, KAGE_VITA_GLFW_UP },
    { SCE_CTRL_SQUARE, KAGE_VITA_GLFW_E },
    { SCE_CTRL_TRIANGLE, KAGE_VITA_GLFW_Q },
    { SCE_CTRL_RTRIGGER, KAGE_VITA_GLFW_SPACE },
    { SCE_CTRL_LTRIGGER, KAGE_VITA_GLFW_LEFT_CONTROL }
};

typedef struct kage_vita_input_raw {
    uint32_t buttons;
    uint16_t xinput_buttons;
    uint16_t touch_xinput_buttons;
    uint8_t xinput_left_trigger;
    uint8_t touch_xinput_left_trigger;
    uint8_t restart_key;
    uint8_t lx, ly, rx, ry;
} kage_vita_input_raw;

enum kage_vita_start_state {
    KAGE_VITA_START_IDLE = 0,
    KAGE_VITA_START_PENDING,
    KAGE_VITA_START_PILL_CARD,
    KAGE_VITA_START_MAP,
    KAGE_VITA_START_CONSUMED,
    KAGE_VITA_START_RESTART
};

static volatile int s_cache_lock;
static volatile int s_initialized;
static uint8_t s_keyboard[KAGE_PC_KEY_COUNT];
static kage_vita_input_raw s_raw;
static kage_vita_input_raw s_xinput_transition_old;
static kage_vita_input_raw s_xinput_transition_new;
static kage_vita_input_raw s_transition_old;
static kage_vita_input_raw s_transition_new;
static unsigned s_generation;
static unsigned s_consumed_generation;
static unsigned s_raw_generation;
static unsigned s_xinput_consumed_raw_generation;
static unsigned s_failed_samples;
static enum kage_vita_start_state s_start_state;
static uint64_t s_start_pressed_us;
static int s_touch_armed;
/* Guest XInput module ownership is process/module lifetime, not one KAGE
 * graphics session.  Do not clear it from kage_vita_input_clear_locked(). */
static int s_xinput_active;

static void kage_vita_input_lock(void)
{
    while (__sync_lock_test_and_set(&s_cache_lock, 1)) {
        /* A producer holds this only across a 349-byte copy. */
    }
    __sync_synchronize();
}

static void kage_vita_input_unlock(void)
{
    __sync_synchronize();
    __sync_lock_release(&s_cache_lock);
}

static void kage_vita_input_clear_locked(void)
{
    memset(s_keyboard, 0, sizeof s_keyboard);
    memset(&s_raw, 0, sizeof s_raw);
    memset(&s_xinput_transition_old, 0, sizeof s_xinput_transition_old);
    memset(&s_xinput_transition_new, 0, sizeof s_xinput_transition_new);
    memset(&s_transition_old, 0, sizeof s_transition_old);
    memset(&s_transition_new, 0, sizeof s_transition_new);
    s_raw.lx = s_raw.ly = s_raw.rx = s_raw.ry = KAGE_VITA_ANALOG_CENTER;
    s_xinput_transition_old = s_raw;
    s_xinput_transition_new = s_raw;
    s_transition_old = s_raw;
    s_transition_new = s_raw;
    s_generation = 0u;
    s_consumed_generation = 0u;
    s_raw_generation = 0u;
    s_xinput_consumed_raw_generation = 0u;
    s_failed_samples = 0u;
    s_start_state = KAGE_VITA_START_IDLE;
    s_start_pressed_us = 0U;
    s_touch_armed = 0;
}

static void kage_vita_input_start_reset_locked(void)
{
    s_start_state = KAGE_VITA_START_IDLE;
    s_start_pressed_us = 0U;
}

static void kage_vita_input_apply_start_locked(
    uint32_t physical_buttons, uint64_t now_us,
    kage_vita_input_raw *published)
{
    int start = (physical_buttons & SCE_CTRL_START) != 0U;
    int left = (physical_buttons & SCE_CTRL_LTRIGGER) != 0U;
    int right = (physical_buttons & SCE_CTRL_RTRIGGER) != 0U;

    published->xinput_buttons = 0U;
    published->xinput_left_trigger = 0U;
    published->restart_key = 0U;
    if (!s_xinput_active) {
        kage_vita_input_start_reset_locked();
        return;
    }

    if (!start) {
        if (s_start_state == KAGE_VITA_START_PENDING && !left && !right) {
            published->buttons |= SCE_CTRL_START;
        } else if (s_start_state != KAGE_VITA_START_IDLE &&
                   (left || right)) {
            /* Do not leak a constituent when Start is released first.  Keep
             * the chord consumed until both shoulders are physically up. */
            published->buttons &= ~(SCE_CTRL_LTRIGGER |
                                    SCE_CTRL_RTRIGGER);
            s_start_state = KAGE_VITA_START_CONSUMED;
            return;
        }
        kage_vita_input_start_reset_locked();
        return;
    }

    /* Defer Start and both shoulders while the prefix is unresolved.  This is
     * what makes pill/map/restart unable to spend a bomb, active item or
     * pause. */
    published->buttons &= ~(SCE_CTRL_START | SCE_CTRL_LTRIGGER |
                            SCE_CTRL_RTRIGGER);
    if (s_start_state == KAGE_VITA_START_IDLE) {
        s_start_state = KAGE_VITA_START_PENDING;
        s_start_pressed_us = now_us;
    }
    if (s_start_state == KAGE_VITA_START_PENDING) {
        if (left && right)
            s_start_state = KAGE_VITA_START_CONSUMED;
        else if (left)
            s_start_state = KAGE_VITA_START_PILL_CARD;
        else if (right)
            s_start_state = KAGE_VITA_START_MAP;
        else if (now_us >= s_start_pressed_us &&
                 now_us - s_start_pressed_us >=
                     KAGE_VITA_INPUT_START_HOLD_US)
            s_start_state = KAGE_VITA_START_RESTART;
    }
    if (s_start_state == KAGE_VITA_START_PILL_CARD && !left)
        s_start_state = KAGE_VITA_START_CONSUMED;
    if (s_start_state == KAGE_VITA_START_MAP && !right)
        s_start_state = KAGE_VITA_START_CONSUMED;

    if (s_start_state == KAGE_VITA_START_PILL_CARD && left)
        published->xinput_buttons =
            KAGE_VITA_INPUT_XINPUT_RIGHT_SHOULDER;
    else if (s_start_state == KAGE_VITA_START_MAP && right)
        published->xinput_buttons = KAGE_VITA_INPUT_XINPUT_BACK;
    else if (s_start_state == KAGE_VITA_START_RESTART)
        /* Once the prefix is unambiguously a long hold, let the untouched
         * guest own the rest of the keyboard-R restart duration. */
        published->restart_key = 1U;
}

static uint16_t kage_vita_input_xinput_buttons(
    const kage_vita_input_raw *raw)
{
    return (uint16_t)(raw->xinput_buttons | raw->touch_xinput_buttons);
}

static uint8_t kage_vita_input_xinput_left_trigger(
    const kage_vita_input_raw *raw)
{
    return (uint8_t)(raw->xinput_left_trigger |
                     raw->touch_xinput_left_trigger);
}

static void kage_vita_input_apply_touch_locked(
    int sample_ok, int snapshot_available,
    const kage_vita_touch_snapshot *touch,
    kage_vita_input_raw *published)
{
    published->touch_xinput_buttons = 0U;
    published->touch_xinput_left_trigger = 0U;

    if (!s_xinput_active || !sample_ok || !snapshot_available) {
        /* A module boundary or a missed touch snapshot is a release.  Require
         * one later successful neutral sample before another touchdown can
         * spend an item; a finger held across launch/suspend cannot ghost. */
        s_touch_armed = 0;
        return;
    }
    if (!s_touch_armed) {
        if (touch->actions == KAGE_VITA_TOUCH_ACTION_NONE)
            s_touch_armed = 1;
        return;
    }

    published->touch_xinput_buttons = touch->xinput_buttons;
    published->touch_xinput_left_trigger = touch->xinput_left_trigger;
}

static void kage_vita_input_publish_locked(
    const kage_vita_input_raw *new_raw,
    const uint8_t next[KAGE_PC_KEY_COUNT])
{
    kage_vita_input_raw old_raw = s_raw;

    s_raw = *new_raw;
    if (memcmp(&old_raw, new_raw, sizeof old_raw) != 0) {
        s_xinput_transition_old = old_raw;
        s_xinput_transition_new = *new_raw;
        ++s_raw_generation;
        if (!s_raw_generation) {
            s_raw_generation = 1u;
            s_xinput_consumed_raw_generation = 0u;
        }
    }
    if (old_raw.buttons != new_raw->buttons ||
            memcmp(s_keyboard, next, sizeof s_keyboard) != 0) {
        memcpy(s_keyboard, next, sizeof s_keyboard);
        s_transition_old = old_raw;
        s_transition_new = *new_raw;
        ++s_generation;
        if (!s_generation) {
            s_generation = 1u;
            s_consumed_generation = 0u;
        }
    }
}

static void kage_vita_input_translate(
    const SceCtrlData *pad, uint8_t keys[KAGE_PC_KEY_COUNT])
{
    size_t index;

    memset(keys, 0, KAGE_PC_KEY_COUNT);
    for (index = 0; index < sizeof s_button_map / sizeof s_button_map[0];
         ++index) {
        if (pad->buttons & s_button_map[index].buttons)
            keys[s_button_map[index].key] = 1u;
    }

    /* The 64-count deadzone and strict edges are copied from pkgj's mature
     * Vita input path, not inferred from Isaac.  ANALOG_WIDE is already proven
     * in our NFS backend.  D-pad/right-stick arrow sources deliberately OR. */
    if (pad->lx < KAGE_VITA_ANALOG_LOW)
        keys[KAGE_VITA_GLFW_A] = 1u;
    if (pad->lx > KAGE_VITA_ANALOG_HIGH)
        keys[KAGE_VITA_GLFW_D] = 1u;
    if (pad->ly < KAGE_VITA_ANALOG_LOW)
        keys[KAGE_VITA_GLFW_W] = 1u;
    if (pad->ly > KAGE_VITA_ANALOG_HIGH)
        keys[KAGE_VITA_GLFW_S] = 1u;
    if (pad->rx < KAGE_VITA_ANALOG_LOW)
        keys[KAGE_VITA_GLFW_LEFT] = 1u;
    if (pad->rx > KAGE_VITA_ANALOG_HIGH)
        keys[KAGE_VITA_GLFW_RIGHT] = 1u;
    if (pad->ry < KAGE_VITA_ANALOG_LOW)
        keys[KAGE_VITA_GLFW_UP] = 1u;
    if (pad->ry > KAGE_VITA_ANALOG_HIGH)
        keys[KAGE_VITA_GLFW_DOWN] = 1u;
}

int kage_vita_input_initialize(void)
{
    int result = sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);

    if (result >= 0)
        (void)kage_vita_touch_initialize();
    else
        kage_vita_touch_deactivate();
    kage_vita_input_lock();
    kage_vita_input_clear_locked();
    s_initialized = result >= 0;
    kage_vita_input_unlock();
    return result >= 0;
}

void kage_vita_input_deactivate(void)
{
    kage_vita_input_lock();
    s_initialized = 0;
    kage_vita_input_clear_locked();
    kage_vita_input_unlock();
    kage_vita_touch_deactivate();
}

int kage_vita_input_sample(void)
{
    SceCtrlData pad;
    kage_vita_touch_snapshot touch;
    uint8_t next[KAGE_PC_KEY_COUNT];
    kage_vita_input_raw new_raw;
    uint64_t now_us;
    int peek_result;
    int touch_sample_ok;
    int touch_available;

    if (!s_initialized)
        return 0;
    memset(&touch, 0, sizeof touch);
    touch_sample_ok = kage_vita_touch_sample();
    touch_available = kage_vita_touch_snapshot_read(&touch);
    memset(&pad, 0, sizeof pad);
    peek_result = sceCtrlPeekBufferPositive(0, &pad, 1);
    if (peek_result != 1) {
        /* The built-in pad normally cannot disappear, but suspend/emulator
         * boundaries can miss a peek.  Retain two transient misses; on the
         * third, publish one neutral raw+keyboard sample so movement, firing
         * and ACTION_DROP cannot remain held forever. */
        memset(&new_raw, 0, sizeof new_raw);
        new_raw.lx = new_raw.ly = new_raw.rx = new_raw.ry =
            KAGE_VITA_ANALOG_CENTER;
        memset(next, 0, sizeof next);
        kage_vita_input_lock();
        if (s_initialized) {
            if (s_failed_samples <
                    KAGE_VITA_INPUT_FAILURE_NEUTRALIZE_COUNT)
                ++s_failed_samples;
            if (s_failed_samples ==
                    KAGE_VITA_INPUT_FAILURE_NEUTRALIZE_COUNT) {
                s_touch_armed = 0;
                if (s_start_state != KAGE_VITA_START_IDLE) {
                    /* A missing sample cannot prove that a consumed
                     * shoulder was released.  Keep it suppressed until a
                     * later successful neutral sample closes the chord. */
                    s_start_state = KAGE_VITA_START_CONSUMED;
                    s_start_pressed_us = 0U;
                }
                kage_vita_input_publish_locked(&new_raw, next);
            } else {
                /* Pad and touch are independent producers.  A touch release
                 * must publish immediately even while the pad retains its
                 * two-sample grace period. */
                new_raw = s_raw;
                kage_vita_input_apply_touch_locked(
                    touch_sample_ok, touch_available, &touch, &new_raw);
                kage_vita_input_publish_locked(&new_raw, s_keyboard);
            }
        }
        kage_vita_input_unlock();
        return 0;
    }
    memset(&new_raw, 0, sizeof new_raw);
    new_raw.buttons = pad.buttons;
    new_raw.lx = pad.lx;
    new_raw.ly = pad.ly;
    new_raw.rx = pad.rx;
    new_raw.ry = pad.ry;
    now_us = (pad.buttons & SCE_CTRL_START)
        ? sceKernelGetProcessTimeWide() : 0U;

    kage_vita_input_lock();
    if (!s_initialized) {
        kage_vita_input_unlock();
        return 0;
    }
    s_failed_samples = 0u;
    kage_vita_input_apply_start_locked(pad.buttons, now_us, &new_raw);
    kage_vita_input_apply_touch_locked(
        touch_sample_ok, touch_available, &touch, &new_raw);
    pad.buttons = new_raw.buttons;
    kage_vita_input_translate(&pad, next);
    if (new_raw.restart_key)
        next[KAGE_VITA_GLFW_R] = 1U;
    kage_vita_input_publish_locked(&new_raw, next);
    kage_vita_input_unlock();
    return 1;
}

int kage_vita_input_gamepad_snapshot(
    kage_vita_gamepad_snapshot *snapshot)
{
    kage_vita_input_raw old_raw;
    kage_vita_input_raw new_raw;
    unsigned generation = 0u;
    unsigned down = 0u;
    unsigned up = 0u;
    int log_transition = 0;
    int available;

    if (!snapshot)
        return 0;
    memset(snapshot, 0, sizeof *snapshot);
    snapshot->lx = snapshot->ly = snapshot->rx = snapshot->ry =
        KAGE_VITA_ANALOG_CENTER;
    kage_vita_input_lock();
    available = s_initialized != 0;
    if (available) {
        snapshot->packet_number = s_raw_generation;
        snapshot->buttons = s_raw.buttons;
        snapshot->xinput_buttons =
            kage_vita_input_xinput_buttons(&s_raw);
        snapshot->xinput_left_trigger =
            kage_vita_input_xinput_left_trigger(&s_raw);
        snapshot->lx = s_raw.lx;
        snapshot->ly = s_raw.ly;
        snapshot->rx = s_raw.rx;
        snapshot->ry = s_raw.ry;
        if (s_xinput_active &&
                s_xinput_consumed_raw_generation != s_raw_generation) {
            s_xinput_consumed_raw_generation = s_raw_generation;
            old_raw = s_xinput_transition_old;
            new_raw = s_xinput_transition_new;
            generation = s_raw_generation;
            down = new_raw.buttons & ~old_raw.buttons;
            up = old_raw.buttons & ~new_raw.buttons;
            /* Raw packet numbers and all four axes remain exact for the
             * original XInput path.  Console telemetry is deliberately only
             * digital: a moving stick crosses deadzone boundaries often, and
             * each Vita3K sceClibPrintf is synchronous host log I/O. */
            log_transition = down != 0u || up != 0u ||
                kage_vita_input_xinput_buttons(&old_raw) !=
                    kage_vita_input_xinput_buttons(&new_raw) ||
                kage_vita_input_xinput_left_trigger(&old_raw) !=
                    kage_vita_input_xinput_left_trigger(&new_raw) ||
                old_raw.restart_key != new_raw.restart_key;
        }
    }
    kage_vita_input_unlock();

    /* Logging is deliberately outside the cache lock.  Vita3K's durable
     * console path can be slow, while the producer and both guest input
     * consumers must continue to share one short coherent-copy boundary. */
    if (log_transition) {
        ISAAC_VITA_LOG_PRINTF(
            "[kage-vita-input] xinput snapshot seq=%u "
            "buttons=%08x->%08x down=%08x up=%08x "
            "synthetic=%04x/%u/%u->%04x/%u/%u "
            "sticks=%u,%u,%u,%u->%u,%u,%u,%u\n",
            generation, (unsigned)old_raw.buttons,
            (unsigned)new_raw.buttons, down, up,
            (unsigned)kage_vita_input_xinput_buttons(&old_raw),
            (unsigned)kage_vita_input_xinput_left_trigger(&old_raw),
            (unsigned)old_raw.restart_key,
            (unsigned)kage_vita_input_xinput_buttons(&new_raw),
            (unsigned)kage_vita_input_xinput_left_trigger(&new_raw),
            (unsigned)new_raw.restart_key,
            (unsigned)old_raw.lx, (unsigned)old_raw.ly,
            (unsigned)old_raw.rx, (unsigned)old_raw.ry,
            (unsigned)new_raw.lx, (unsigned)new_raw.ly,
            (unsigned)new_raw.rx, (unsigned)new_raw.ry);
    }
    return available;
}

void kage_vita_input_set_xinput_active(int active)
{
    uint8_t neutral_keys[KAGE_PC_KEY_COUNT];
    kage_vita_input_raw filtered;
    int was_active;

    kage_vita_input_lock();
    was_active = s_xinput_active;
    s_xinput_active = active != 0;
    if (was_active != s_xinput_active) {
        kage_vita_input_start_reset_locked();
        s_touch_armed = 0;
        filtered = s_raw;
        filtered.xinput_buttons = 0U;
        filtered.touch_xinput_buttons = 0U;
        filtered.xinput_left_trigger = 0U;
        filtered.touch_xinput_left_trigger = 0U;
        filtered.restart_key = 0U;
        if (s_xinput_active)
            filtered.buttons &= ~(SCE_CTRL_START | SCE_CTRL_LTRIGGER |
                                  SCE_CTRL_RTRIGGER);
        memset(neutral_keys, 0, sizeof neutral_keys);
        kage_vita_input_publish_locked(&filtered, neutral_keys);
        /* Ownership changes are not guest input edges.  The next producer
         * sample republishes whichever physical levels are still held. */
        s_consumed_generation = s_generation;
        s_xinput_consumed_raw_generation = s_raw_generation;
    }
    kage_vita_input_unlock();
}

void kage_vita_input_keyboard_snapshot(
    uint8_t keys[KAGE_PC_KEY_COUNT])
{
    kage_vita_input_raw old_raw;
    kage_vita_input_raw new_raw;
    unsigned generation = 0u;
    unsigned enter = 0u;
    int log_transition = 0;

    if (!keys)
        return;
    memset(&old_raw, 0, sizeof old_raw);
    memset(&new_raw, 0, sizeof new_raw);
    kage_vita_input_lock();
    if (s_xinput_active) {
        memset(keys, 0, sizeof s_keyboard);
        keys[KAGE_VITA_GLFW_R] = s_keyboard[KAGE_VITA_GLFW_R];
        s_consumed_generation = s_generation;
    } else {
        memcpy(keys, s_keyboard, sizeof s_keyboard);
    }
    if (!s_xinput_active && s_consumed_generation != s_generation) {
        s_consumed_generation = s_generation;
        old_raw = s_transition_old;
        new_raw = s_transition_new;
        generation = s_consumed_generation;
        enter = s_keyboard[KAGE_VITA_GLFW_ENTER] != 0u;
        log_transition = 1;
    }
    kage_vita_input_unlock();

    if (log_transition) {
        ISAAC_VITA_LOG_PRINTF(
            "[kage-vita-input] snapshot seq=%u buttons=%08x->%08x "
            "sticks=%u,%u,%u,%u->%u,%u,%u,%u enter=%u esc=%u tab=%u "
            "wasd=%u%u%u%u arrows=%u%u%u%u eqsp=%u%u%u ctrl=%u\n",
            generation, (unsigned)old_raw.buttons,
            (unsigned)new_raw.buttons,
            (unsigned)old_raw.lx, (unsigned)old_raw.ly,
            (unsigned)old_raw.rx, (unsigned)old_raw.ry,
            (unsigned)new_raw.lx, (unsigned)new_raw.ly,
            (unsigned)new_raw.rx, (unsigned)new_raw.ry,
            enter,
            (unsigned)keys[KAGE_VITA_GLFW_ESCAPE],
            (unsigned)keys[KAGE_VITA_GLFW_TAB],
            (unsigned)keys[KAGE_VITA_GLFW_W],
            (unsigned)keys[KAGE_VITA_GLFW_A],
            (unsigned)keys[KAGE_VITA_GLFW_S],
            (unsigned)keys[KAGE_VITA_GLFW_D],
            (unsigned)keys[KAGE_VITA_GLFW_LEFT],
            (unsigned)keys[KAGE_VITA_GLFW_RIGHT],
            (unsigned)keys[KAGE_VITA_GLFW_UP],
            (unsigned)keys[KAGE_VITA_GLFW_DOWN],
            (unsigned)keys[KAGE_VITA_GLFW_E],
            (unsigned)keys[KAGE_VITA_GLFW_Q],
            (unsigned)keys[KAGE_VITA_GLFW_SPACE],
            (unsigned)keys[KAGE_VITA_GLFW_LEFT_CONTROL]);
    }
}
