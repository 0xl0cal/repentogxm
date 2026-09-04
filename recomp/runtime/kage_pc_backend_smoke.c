/* Standalone host proof for kage_pc_backend.c.  The full recompiler harness
 * gets a separate explicit wrapper/ABI mode; this executable proves the native
 * core without linking 156 generated objects. */
#include "kage_pc_backend.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#pragma comment(lib, "user32.lib")

typedef struct smoke_key {
    WPARAM virtual_key;
    unsigned scancode;
    unsigned extended;
    unsigned key;
} smoke_key;

static LPARAM smoke_key_lparam(unsigned scancode, unsigned extended,
                               unsigned was_down, unsigned released)
{
    uint32_t bits = 1u | ((scancode & 0xffu) << 16);
    if (extended)
        bits |= 1u << 24;
    if (was_down)
        bits |= 1u << 30;
    if (released)
        bits |= 1u << 31;
    return (LPARAM)(uintptr_t)bits;
}

static HWND smoke_backend_window(void)
{
    HWND window = NULL;
    DWORD process;

    do {
        window = FindWindowExA(NULL, window, "repentogxm-kage-wgl",
                               "repentogxm KAGE PC backend");
        if (!window)
            return NULL;
        process = 0;
        GetWindowThreadProcessId(window, &process);
    } while (process != GetCurrentProcessId());
    return window;
}

static int smoke_keys_released(const uint8_t keys[KAGE_PC_KEY_COUNT])
{
    unsigned index;
    for (index = 0; index < KAGE_PC_KEY_COUNT; ++index) {
        if (keys[index])
            return 0;
    }
    return 1;
}

static void smoke_store_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static LRESULT smoke_send_command_frame(HWND window, ULONG_PTR magic,
                                        uint32_t version,
                                        uint32_t target_pid,
                                        uint32_t sequence,
                                        const uint8_t *text,
                                        uint32_t text_size,
                                        DWORD frame_size)
{
    uint8_t packet[KAGE_PC_COMMAND_COPYDATA_HEADER_SIZE +
                   KAGE_PC_COMMAND_TEXT_MAX + 1u];
    COPYDATASTRUCT copy;

    if (!window || !text || text_size > KAGE_PC_COMMAND_TEXT_MAX + 1u)
        return 0;
    smoke_store_le32(packet + 0u, version);
    smoke_store_le32(packet + 4u, target_pid);
    smoke_store_le32(packet + 8u, sequence);
    smoke_store_le32(packet + 12u, text_size);
    memcpy(packet + KAGE_PC_COMMAND_COPYDATA_HEADER_SIZE, text, text_size);
    memset(&copy, 0, sizeof copy);
    copy.dwData = magic;
    copy.cbData = frame_size;
    copy.lpData = packet;
    return SendMessageA(window, WM_COPYDATA, 0, (LPARAM)&copy);
}

static LRESULT smoke_send_command(HWND window, uint32_t version,
                                  uint32_t target_pid, uint32_t sequence,
                                  const uint8_t *text, uint32_t text_size)
{
    return smoke_send_command_frame(
        window, (ULONG_PTR)KAGE_PC_COMMAND_COPYDATA_MAGIC, version,
        target_pid, sequence, text, text_size,
        KAGE_PC_COMMAND_COPYDATA_HEADER_SIZE + text_size);
}

typedef struct smoke_cross_thread_open {
    int handled;
    uint32_t result;
} smoke_cross_thread_open;

static DWORD WINAPI smoke_open_on_other_thread(void *parameter)
{
    smoke_cross_thread_open *probe = (smoke_cross_thread_open *)parameter;
    probe->result = 99u;
    probe->handled = kage_pc_backend_virtual_clipboard_open(
        (uintptr_t)0, &probe->result);
    return 0u;
}

static int smoke_virtual_clipboard(void)
{
    static const uint8_t first[] = "spawn 10\n";
    static const uint8_t second[] = "giveitem c1\nspawn 10\n";
    static const uint8_t embedded_nul[] = {'a', 0, 'b', '\n', 0};
    static const uint8_t empty_line[] = {'a', '\n', '\n', 0};
    static const uint8_t no_newline[] = {'a', 'b', 0};
    HWND window = smoke_backend_window();
    DWORD process = GetCurrentProcessId();
    DWORD sequence_before = GetClipboardSequenceNumber();
    DWORD sequence_after;
    HANDLE thread;
    smoke_cross_thread_open cross_thread;
    uintptr_t virtual_handle;
    uintptr_t pointer;
    uint32_t result;

    if (!window || !kage_pc_backend_console_trace_enabled())
        return 0;

    /* Trace-enabled OpenClipboard(NULL) is private even before a packet is
     * armed.  It must fail virtually instead of exposing the real clipboard. */
    result = 99u;
    if (!kage_pc_backend_virtual_clipboard_open((uintptr_t)0, &result) ||
        result != 0u)
        return 0;

    /* Every rejected packet leaves transport sequence zero and cannot occupy
     * the one pending slot. */
    if (smoke_send_command(window, KAGE_PC_COMMAND_COPYDATA_VERSION,
                           process, 2u, first, (uint32_t)sizeof first) != FALSE ||
        smoke_send_command(window, KAGE_PC_COMMAND_COPYDATA_VERSION + 1u,
                           process, 1u, first,
                           (uint32_t)sizeof first) != FALSE ||
        smoke_send_command(window, KAGE_PC_COMMAND_COPYDATA_VERSION,
                           process + 1u, 1u, first,
                           (uint32_t)sizeof first) != FALSE ||
        smoke_send_command_frame(
            window, (ULONG_PTR)KAGE_PC_COMMAND_COPYDATA_MAGIC + 1u,
            KAGE_PC_COMMAND_COPYDATA_VERSION, process, 1u, first,
            (uint32_t)sizeof first,
            KAGE_PC_COMMAND_COPYDATA_HEADER_SIZE + sizeof first) != FALSE ||
        smoke_send_command_frame(
            window, (ULONG_PTR)KAGE_PC_COMMAND_COPYDATA_MAGIC,
            KAGE_PC_COMMAND_COPYDATA_VERSION, process, 1u, first,
            (uint32_t)sizeof first,
            KAGE_PC_COMMAND_COPYDATA_HEADER_SIZE + sizeof first - 1u) != FALSE ||
        smoke_send_command(window, KAGE_PC_COMMAND_COPYDATA_VERSION,
                           process, 1u, embedded_nul,
                           (uint32_t)sizeof embedded_nul) != FALSE ||
        smoke_send_command(window, KAGE_PC_COMMAND_COPYDATA_VERSION,
                           process, 1u, empty_line,
                           (uint32_t)sizeof empty_line) != FALSE ||
        smoke_send_command(window, KAGE_PC_COMMAND_COPYDATA_VERSION,
                           process, 1u, no_newline,
                           (uint32_t)sizeof no_newline) != FALSE)
        return 0;

    if (smoke_send_command(window, KAGE_PC_COMMAND_COPYDATA_VERSION,
                           process, 1u, first,
                           (uint32_t)sizeof first) != TRUE ||
        /* Pending data is immutable until its one complete consume. */
        smoke_send_command(window, KAGE_PC_COMMAND_COPYDATA_VERSION,
                           process, 2u, second,
                           (uint32_t)sizeof second) != FALSE)
        return 0;

    /* A close after a format miss is not a consume.  The same pending packet
     * reopens, while sequence two remains blocked until lock/unlock/close. */
    if (!kage_pc_backend_virtual_clipboard_open((uintptr_t)0, &result) ||
        result != 1u ||
        !kage_pc_backend_virtual_clipboard_get_data(
            CF_UNICODETEXT, &virtual_handle) || virtual_handle != 0u ||
        !kage_pc_backend_virtual_clipboard_close(&result) || result != 1u ||
        smoke_send_command(window, KAGE_PC_COMMAND_COPYDATA_VERSION,
                           process, 2u, second,
                           (uint32_t)sizeof second) != FALSE)
        return 0;

    memset(&cross_thread, 0, sizeof cross_thread);
    thread = CreateThread(NULL, 0u, smoke_open_on_other_thread,
                          &cross_thread, 0u, NULL);
    if (!thread)
        return 0;
    if (WaitForSingleObject(thread, 5000u) != WAIT_OBJECT_0) {
        CloseHandle(thread);
        return 0;
    }
    CloseHandle(thread);
    if (cross_thread.handled != 1 || cross_thread.result != 0u)
        return 0;

    if (!kage_pc_backend_virtual_clipboard_open((uintptr_t)0, &result) ||
        result != 1u ||
        !kage_pc_backend_virtual_clipboard_get_data(
            CF_TEXT, &virtual_handle) || !virtual_handle ||
        !kage_pc_backend_virtual_global_lock(virtual_handle, &pointer) ||
        !pointer || memcmp((const void *)pointer, first, sizeof first) != 0 ||
        !kage_pc_backend_virtual_global_unlock(virtual_handle, &result) ||
        !kage_pc_backend_virtual_clipboard_close(&result) || result != 1u)
        return 0;

    /* Sequence one was consumed exactly once; stale replay is rejected, while
     * the next multiline packet is accepted and read through the same ABI. */
    if (!kage_pc_backend_virtual_clipboard_open((uintptr_t)0, &result) ||
        result != 0u ||
        smoke_send_command(window, KAGE_PC_COMMAND_COPYDATA_VERSION,
                           process, 1u, first,
                           (uint32_t)sizeof first) != FALSE ||
        smoke_send_command(window, KAGE_PC_COMMAND_COPYDATA_VERSION,
                           process, 2u, second,
                           (uint32_t)sizeof second) != TRUE ||
        !kage_pc_backend_virtual_clipboard_open((uintptr_t)0, &result) ||
        result != 1u ||
        !kage_pc_backend_virtual_clipboard_get_data(
            CF_TEXT, &virtual_handle) || !virtual_handle ||
        !kage_pc_backend_virtual_global_lock(virtual_handle, &pointer) ||
        !pointer || memcmp((const void *)pointer, second, sizeof second) != 0 ||
        !kage_pc_backend_virtual_global_unlock(virtual_handle, &result) ||
        !kage_pc_backend_virtual_clipboard_close(&result) || result != 1u)
        return 0;

    sequence_after = GetClipboardSequenceNumber();
    return sequence_after == sequence_before;
}

static int smoke_keyboard(void)
{
    static const unsigned letter_scancodes[26] = {
        0x1eu, 0x30u, 0x2eu, 0x20u, 0x12u, 0x21u, 0x22u,
        0x23u, 0x17u, 0x24u, 0x25u, 0x26u, 0x32u, 0x31u,
        0x18u, 0x19u, 0x10u, 0x13u, 0x1fu, 0x14u, 0x16u,
        0x2fu, 0x11u, 0x2du, 0x15u, 0x2cu
    };
    static const smoke_key controls[] = {
        {VK_RETURN, 0x1cu, 0u, 257u},
        {VK_LEFT,   0x4bu, 1u, 263u},
        {VK_RIGHT,  0x4du, 1u, 262u},
        {VK_DOWN,   0x50u, 1u, 264u},
        {VK_UP,     0x48u, 1u, 265u},
        {VK_ESCAPE, 0x01u, 0u, 256u},
        {VK_SPACE,  0x39u, 0u, 32u}
    };
    uint8_t down[KAGE_PC_KEY_COUNT];
    uint8_t held[KAGE_PC_KEY_COUNT];
    uint8_t up[KAGE_PC_KEY_COUNT];
    HWND window = smoke_backend_window();
    unsigned index;

    if (!window)
        return 0;
    kage_pc_backend_keyboard_snapshot(up);
    if (!smoke_keys_released(up))
        return 0;

    SendMessageA(window, WM_KEYDOWN, 'A',
                 smoke_key_lparam(0x1eu, 0u, 0u, 0u));
    kage_pc_backend_keyboard_snapshot(down);
    SendMessageA(window, WM_KEYDOWN, 'A',
                 smoke_key_lparam(0x1eu, 0u, 1u, 0u));
    kage_pc_backend_keyboard_snapshot(held);
    SendMessageA(window, WM_KEYUP, 'A',
                 smoke_key_lparam(0x1eu, 0u, 1u, 1u));
    kage_pc_backend_keyboard_snapshot(up);
    if (down['A'] != 1u || memcmp(down, held, sizeof down) != 0 ||
        up['A'] != 0u)
        return 0;

    for (index = 0; index < 26u; ++index) {
        const WPARAM virtual_key = (WPARAM)('A' + index);
        SendMessageA(window, WM_KEYDOWN, virtual_key,
                     smoke_key_lparam(letter_scancodes[index], 0u, 0u, 0u));
        kage_pc_backend_keyboard_snapshot(down);
        if (down['A' + index] != 1u)
            return 0;
        SendMessageA(window, WM_KEYUP, virtual_key,
                     smoke_key_lparam(letter_scancodes[index], 0u, 1u, 1u));
    }
    for (index = 0; index < sizeof controls / sizeof controls[0]; ++index) {
        const smoke_key *key = &controls[index];
        SendMessageA(window, WM_KEYDOWN, key->virtual_key,
                     smoke_key_lparam(key->scancode, key->extended, 0u, 0u));
        kage_pc_backend_keyboard_snapshot(down);
        if (down[key->key] != 1u)
            return 0;
        SendMessageA(window, WM_KEYUP, key->virtual_key,
                     smoke_key_lparam(key->scancode, key->extended, 1u, 1u));
    }
    kage_pc_backend_keyboard_snapshot(up);
    if (!smoke_keys_released(up))
        return 0;

    SendMessageA(window, WM_KEYDOWN, 'Z',
                 smoke_key_lparam(0x2cu, 0u, 0u, 0u));
    SendMessageA(window, WM_KEYDOWN, VK_LEFT,
                 smoke_key_lparam(0x4bu, 1u, 0u, 0u));
    SendMessageA(window, WM_KILLFOCUS, 0, 0);
    kage_pc_backend_keyboard_snapshot(up);
    return smoke_keys_released(up);
}
#endif

int main(void)
{
    uint8_t pixel[4];
    uint32_t refresh;
    int ok;

#if defined(_WIN32)
    if (!SetEnvironmentVariableA("REPENTOGXM_PC_CONSOLE_TRACE", "1")) {
        printf("environment: FAIL %lu\n", (unsigned long)GetLastError());
        return 1;
    }
#endif
    if (!kage_pc_backend_set_mode(KAGE_PC_BACKEND_HIDDEN)) {
        printf("mode: FAIL %s\n", kage_pc_backend_last_error());
        return 1;
    }
    if (!kage_pc_backend_initialize(64u, 64u)) {
        printf("initialize: FAIL %s\n", kage_pc_backend_last_error());
        return 1;
    }
    kage_pc_backend_probe_rgba(pixel);
    printf("GL_VERSION  : %s\n", kage_pc_backend_gl_version());
    printf("accelerated : %s\n",
           kage_pc_backend_accelerated() ? "yes" : "no");
    refresh = kage_pc_backend_refresh_rate();
    printf("refresh     : %u Hz\n", refresh);
    printf("clear pixel : %u,%u,%u,%u\n",
           pixel[0], pixel[1], pixel[2], pixel[3]);
    printf("after init  : clears=%u presents=%u\n",
           kage_pc_backend_clear_count(),
           kage_pc_backend_present_count());

    ok = kage_pc_backend_ready() &&
         kage_pc_backend_width() == 64u &&
         kage_pc_backend_height() == 64u &&
         kage_pc_backend_clear_count() == 1u &&
         kage_pc_backend_present_count() == 1u &&
         refresh > 1u &&
         kage_pc_backend_vsync_change_count() == 0u &&
         !kage_pc_backend_vsync_enabled();
    if (!kage_pc_backend_set_vsync(1) ||
        !kage_pc_backend_vsync_enabled() ||
        !kage_pc_backend_set_vsync(0) ||
        kage_pc_backend_vsync_enabled()) {
        printf("vsync: FAIL %s\n", kage_pc_backend_last_error());
        ok = 0;
    }
    ok = ok && kage_pc_backend_vsync_change_count() == 2u;
#if defined(_WIN32)
    if (!smoke_keyboard()) {
        printf("keyboard    : down/hold/up/focus FAIL\n");
        ok = 0;
    } else {
        printf("keyboard    : down/hold/up/focus PASS\n");
    }
    if (!smoke_virtual_clipboard()) {
        printf("command IPC : validation/state/clipboard isolation FAIL\n");
        ok = 0;
    } else {
        printf("command IPC : validation/state/clipboard isolation PASS\n");
    }
#endif
    if (!kage_pc_backend_present()) {
        printf("present: FAIL %s\n", kage_pc_backend_last_error());
        ok = 0;
    }
    printf("after swap  : clears=%u presents=%u\n",
           kage_pc_backend_clear_count(),
           kage_pc_backend_present_count());
    ok = ok && kage_pc_backend_present_count() == 2u;
    kage_pc_backend_shutdown();
    ok = ok && !kage_pc_backend_ready();
    printf("VERDICT     : %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
