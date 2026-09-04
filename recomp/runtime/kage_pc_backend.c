/* Minimal real PC graphics backend for the manual KAGE boundary.
 *
 * This deliberately does not emulate GLFW.  It owns one native Win32 window,
 * DC and WGL context and gives the high-level KAGE seam the operations it
 * actually needs: create, present, query framebuffer dimensions and destroy.
 * All non-Kernel32 entry points are resolved from their owning DLL, keeping
 * the generated guest and its IAT out of this platform implementation.
 *
 * The initialization proof is stronger than "a context handle was non-null":
 * it clears the back buffer to a pinned colour, reads one real GL pixel back,
 * checks glGetError, and performs a checked SwapBuffers.  This follows the
 * context/pixel-format order used by GLFW's upstream WGL implementation and
 * documented by Microsoft. */
#include "kage_pc_backend.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifndef WGL_CONTEXT_MAJOR_VERSION_ARB
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB  0x9126
#define WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x00000002
#endif

#define KAGE_GL_COLOR_BUFFER_BIT 0x00004000u
#define KAGE_GL_BACK             0x0405u
#define KAGE_GL_RGBA             0x1908u
#define KAGE_GL_UNSIGNED_BYTE    0x1401u
#define KAGE_GL_VERSION          0x1f02u
#define KAGE_GL_DITHER                    0x0bd0u
#define KAGE_GL_SCISSOR_TEST              0x0c11u
#define KAGE_GL_DRAW_BUFFER               0x0c01u
#define KAGE_GL_READ_BUFFER               0x0c02u
#define KAGE_GL_COLOR_CLEAR_VALUE         0x0c22u
#define KAGE_GL_COLOR_WRITEMASK           0x0c23u
#define KAGE_GL_PACK_SWAP_BYTES           0x0d00u
#define KAGE_GL_PACK_LSB_FIRST            0x0d01u
#define KAGE_GL_PACK_ROW_LENGTH           0x0d02u
#define KAGE_GL_PACK_SKIP_ROWS            0x0d03u
#define KAGE_GL_PACK_SKIP_PIXELS          0x0d04u
#define KAGE_GL_PACK_ALIGNMENT            0x0d05u
#define KAGE_GL_PIXEL_PACK_BUFFER_BINDING 0x88edu
#define KAGE_GL_CURRENT_PROGRAM           0x8b8du
#define KAGE_GL_DRAW_FRAMEBUFFER_BINDING  0x8ca6u
#define KAGE_GL_RENDERBUFFER_BINDING      0x8ca7u
#define KAGE_GL_READ_FRAMEBUFFER_BINDING  0x8caau
#define KAGE_GL_FRAMEBUFFER_SRGB          0x8db9u
#define KAGE_MAX_DIMENSION       8192u
#define KAGE_CAPTURE_WIDTH       960u
#define KAGE_CAPTURE_HEIGHT      540u
#define KAGE_CAPTURE_CHANNELS    4u
#define KAGE_CAPTURE_STALE_ERROR_LIMIT 16u
#define KAGE_GAME_GLOBAL_POINTER 0x307fd65cu
#define KAGE_GAME_ROOM_INDEX     0x00018194u
#define KAGE_GAME_DIMENSION      0x0001819cu
#define KAGE_GAME_PLAYER_MANAGER 0x0001ba40u
#define KAGE_PLAYER_VECTOR_BEGIN 0x00000010u
#define KAGE_PLAYER_VECTOR_END   0x00000014u
#define KAGE_ENTITY_TYPE_OFFSET  0x00000028u
#define KAGE_PLAYER_POSITION_X   0x00000294u
#define KAGE_PLAYER_POSITION_Y   0x00000298u
#define KAGE_ENTITY_PLAYER_VPTR  0x3074d4f0u

enum {
    KAGE_STAGE_LOOP_HEAD = 0,
    KAGE_STAGE_UPDATE_ENTRY,
    KAGE_STAGE_RENDER_ENTRY,
    KAGE_STAGE_RENDER_RETURN,
    KAGE_STAGE_COUNT
};

enum { KAGE_MENU_INIT_PROBE_COUNT = 26 };
enum { KAGE_MENU_RENDER_PROBE_COUNT = 26 };
enum { KAGE_MENU_CHARACTER_PROBE_COUNT = 45 };

enum {
    KAGE_MENU_INIT_PROBE_ARM_INDEX = 0,
    KAGE_MENU_INIT_PROBE_DISARM_INDEX = KAGE_MENU_INIT_PROBE_COUNT - 1
};

enum {
    KAGE_MENU_RENDER_PROBE_ARM_INDEX = 0,
    KAGE_MENU_RENDER_PROBE_DISARM_INDEX = KAGE_MENU_RENDER_PROBE_COUNT - 1
};

enum {
    KAGE_GL_TRACE_ACTIVE_TEXTURE = 0,
    KAGE_GL_TRACE_BIND_FRAMEBUFFER,
    KAGE_GL_TRACE_BIND_TEXTURE,
    KAGE_GL_TRACE_CLEAR,
    KAGE_GL_TRACE_CLEAR_COLOR,
    KAGE_GL_TRACE_DISABLE_VERTEX_ATTRIB,
    KAGE_GL_TRACE_DRAW_ELEMENTS,
    KAGE_GL_TRACE_ENABLE_VERTEX_ATTRIB,
    KAGE_GL_TRACE_FRAMEBUFFER_TEXTURE,
    KAGE_GL_TRACE_TEX_IMAGE,
    KAGE_GL_TRACE_TEX_SUB_IMAGE,
    KAGE_GL_TRACE_UNIFORM_MATRIX4,
    KAGE_GL_TRACE_USE_PROGRAM,
    KAGE_GL_TRACE_VERTEX_ATTRIB_POINTER,
    KAGE_GL_TRACE_VIEWPORT,
    KAGE_GL_TRACE_COUNT
};

static const char *const kage_gl_trace_names[KAGE_GL_TRACE_COUNT] = {
    "glActiveTexture", "glBindFramebuffer", "glBindTexture", "glClear",
    "glClearColor", "glDisableVertexAttribArray", "glDrawElements",
    "glEnableVertexAttribArray", "glFramebufferTexture2D", "glTexImage2D",
    "glTexSubImage2D", "glUniformMatrix4fv", "glUseProgram",
    "glVertexAttribPointer", "glViewport"
};

static const uint32_t
kage_menu_init_probe_sites[KAGE_MENU_INIT_PROBE_COUNT] = {
    0x004b44a4u, 0x004e04a5u, 0x004e0511u, 0x00007198u,
    0x0000719eu, 0x000071acu, 0x004e0516u, 0x004e0590u,
    0x004e05e6u, 0x004b44b7u, 0x004b44f7u, 0x004b0085u,
    0x004afc33u, 0x004afce3u, 0x004afd3du, 0x004afe79u,
    0x004afe89u, 0x004b01f0u, 0x004b01f6u, 0x0047a25fu,
    0x0047a284u, 0x0047a292u, 0x0047a2dcu, 0x004b01fbu,
    0x004b0571u, 0x004b05e6u
};

typedef struct kage_menu_render_probe {
    uint32_t site;
    const char *name;
} kage_menu_render_probe;

static const kage_menu_render_probe
kage_menu_render_probes[KAGE_MENU_RENDER_PROBE_COUNT] = {
    {0x004b07e6u, "state1 MenuManager Render call"},
    {0x004e19d0u, "MenuManager Render entry"},
    {0x004e1ab9u, "pre-submenu image draw call"},
    {0x004e1abcu, "pre-submenu image draw returned"},
    {0x004e1b45u, "inlined Title complete / Challenge setup"},
    {0x004a16e0u, "Challenge fixed-item loop begin"},
    {0x004a1c24u, "Challenge fixed-item loop complete"},
    {0x004a1d8bu, "Challenge cleanup complete"},
    {0x004e1b50u, "Challenge render returned"},
    {0x004e1b5bu, "Daily render returned"},
    {0x004e1b66u, "Collection render returned"},
    {0x004e1b71u, "Bestiary render returned"},
    {0x004e1b7cu, "Stats render returned"},
    {0x004e1b87u, "Options render returned"},
    {0x004e1b92u, "Character render returned"},
    {0x004e1b9du, "Game render returned"},
    {0x004e1ba8u, "SpecialSeed render returned"},
    {0x004e1bb3u, "Controller render returned"},
    {0x004e1bbeu, "KeyConfig render returned"},
    {0x004e1bc9u, "Cutscenes render returned"},
    {0x004e1bd4u, "Custom render returned"},
    {0x004e1bdfu, "Mods render returned"},
    {0x004e1beau, "Save render returned"},
    {0x004e1bf5u, "Daily leaderboard render returned"},
    {0x004e235fu, "MenuManager Render return"},
    {0x004b07ebu, "state1 MenuManager Render returned"}
};

static const kage_menu_render_probe
kage_menu_character_probes[KAGE_MENU_CHARACTER_PROBE_COUNT] = {
    {0x004a5570u, "Character Render entry"},
    {0x004a5650u, "header AnimationStates complete"},
    {0x004a575du, "first portrait layers complete"},
    {0x004a59d2u, "early portrait bundle complete"},
    {0x004a5a25u, "character-menu helper call"},
    {0x004a5befu, "random-detail path joined"},
    {0x004a6008u, "supplemental portrait block complete"},
    {0x004a623bu, "navigation AnimationStates complete"},
    {0x004a644du, "selection logic complete / font block"},
    {0x004a6519u, "first font draw returned"},
    {0x004a65d8u, "second font draw returned"},
    {0x004a669du, "character-info text complete"},
    {0x004a672bu, "lower AnimationStates complete"},
    {0x004a6820u, "heading draw and availability query returned"},
    {0x004a6aa7u, "coop-modifier block complete"},
    {0x004a6b65u, "tooltip source string returned"},
    {0x004a6bbfu, "tooltip lowercase pass complete"},
    {0x004a6bfdu, "first tooltip width measured"},
    {0x004a6c7fu, "first tooltip line drawn"},
    {0x004a6cd9u, "second tooltip width measured"},
    {0x004a6d58u, "second tooltip line drawn"},
    {0x004a6d8eu, "Character cleanup join"},
    {0x004a6dabu, "Character Render ret"},
    {0x004a6db0u, "character-menu helper entry"},
    {0x004a6e08u, "initial character draw complete"},
    {0x004a6e1bu, "character-wheel helper call"},
    {0x004a6e20u, "character-wheel helper returned"},
    {0x004a6f4bu, "optional character-ten font block complete"},
    {0x004a7019u, "helper transition state ready"},
    {0x004a706au, "helper AnimationStates complete"},
    {0x004a707au, "character-menu helper ret"},
    {0x004a7080u, "character-wheel helper entry"},
    {0x004a75edu, "character-wheel TLS slow path"},
    {0x004a75f7u, "character-wheel TLS header returned"},
    {0x004a70a7u, "character-wheel TLS cache ready"},
    {0x004a7110u, "character-wheel fixed loop"},
    {0x004a7180u, "character-wheel fixed loop complete"},
    {0x004a71ecu, "character-wheel map fill complete"},
    {0x004a71f5u, "character-wheel index helper returned"},
    {0x004a7230u, "character-wheel dynamic loop"},
    {0x004a733eu, "character-wheel entry prelude complete"},
    {0x004a735eu, "character-wheel entry render phase"},
    {0x004a75aeu, "character-wheel entry draw complete"},
    {0x004a75e4u, "character-wheel dynamic loop complete"},
    {0x004a75eau, "character-wheel helper ret"}
};

typedef HGLRC (WINAPI *kage_wgl_create_context_fn)(HDC);
typedef BOOL  (WINAPI *kage_wgl_delete_context_fn)(HGLRC);
typedef BOOL  (WINAPI *kage_wgl_make_current_fn)(HDC, HGLRC);
typedef HGLRC (WINAPI *kage_wgl_get_current_context_fn)(void);
typedef PROC  (WINAPI *kage_wgl_get_proc_address_fn)(LPCSTR);
typedef HGLRC (WINAPI *kage_wgl_create_context_attribs_fn)(HDC, HGLRC,
                                                           const int *);
typedef BOOL  (WINAPI *kage_wgl_swap_interval_fn)(int);

typedef const unsigned char *(APIENTRY *kage_gl_get_string_fn)(unsigned int);
typedef unsigned int (APIENTRY *kage_gl_get_error_fn)(void);
typedef void (APIENTRY *kage_gl_viewport_fn)(int, int, int, int);
typedef void (APIENTRY *kage_gl_clear_color_fn)(float, float, float, float);
typedef void (APIENTRY *kage_gl_clear_fn)(unsigned int);
typedef void (APIENTRY *kage_gl_finish_fn)(void);
typedef void (APIENTRY *kage_gl_get_boolean_v_fn)(unsigned int,
                                                   unsigned char *);
typedef void (APIENTRY *kage_gl_get_float_v_fn)(unsigned int, float *);
typedef void (APIENTRY *kage_gl_get_integer_v_fn)(unsigned int, int *);
typedef unsigned char (APIENTRY *kage_gl_is_enabled_fn)(unsigned int);
typedef void (APIENTRY *kage_gl_enable_fn)(unsigned int);
typedef void (APIENTRY *kage_gl_disable_fn)(unsigned int);
typedef void (APIENTRY *kage_gl_color_mask_fn)(unsigned char, unsigned char,
                                                unsigned char, unsigned char);
typedef void (APIENTRY *kage_gl_draw_buffer_fn)(unsigned int);
typedef void (APIENTRY *kage_gl_read_buffer_fn)(unsigned int);
typedef void (APIENTRY *kage_gl_pixel_store_i_fn)(unsigned int, int);
typedef void (APIENTRY *kage_gl_read_pixels_fn)(int, int, int, int,
                                                unsigned int, unsigned int,
                                                void *);

typedef struct kage_pc_api {
    HMODULE user32;
    HMODULE gdi32;
    HMODULE opengl32;

    ATOM    (WINAPI *RegisterClassExA_)(const WNDCLASSEXA *);
    BOOL    (WINAPI *UnregisterClassA_)(LPCSTR, HINSTANCE);
    HWND    (WINAPI *CreateWindowExA_)(DWORD, LPCSTR, LPCSTR, DWORD,
                                       int, int, int, int, HWND, HMENU,
                                       HINSTANCE, LPVOID);
    BOOL    (WINAPI *DestroyWindow_)(HWND);
    LRESULT (WINAPI *DefWindowProcA_)(HWND, UINT, WPARAM, LPARAM);
    HDC     (WINAPI *GetDC_)(HWND);
    int     (WINAPI *ReleaseDC_)(HWND, HDC);
    BOOL    (WINAPI *ShowWindow_)(HWND, int);
    BOOL    (WINAPI *UpdateWindow_)(HWND);
    BOOL    (WINAPI *PeekMessageA_)(LPMSG, HWND, UINT, UINT, UINT);
    BOOL    (WINAPI *TranslateMessage_)(const MSG *);
    LRESULT (WINAPI *DispatchMessageA_)(const MSG *);
    BOOL    (WINAPI *EnumDisplaySettingsA_)(LPCSTR, DWORD, DEVMODEA *);

    int  (WINAPI *ChoosePixelFormat_)(HDC, const PIXELFORMATDESCRIPTOR *);
    int  (WINAPI *DescribePixelFormat_)(HDC, int, UINT,
                                        LPPIXELFORMATDESCRIPTOR);
    BOOL (WINAPI *SetPixelFormat_)(HDC, int,
                                   const PIXELFORMATDESCRIPTOR *);
    BOOL (WINAPI *SwapBuffers_)(HDC);

    kage_wgl_create_context_fn       wglCreateContext_;
    kage_wgl_delete_context_fn       wglDeleteContext_;
    kage_wgl_make_current_fn         wglMakeCurrent_;
    kage_wgl_get_current_context_fn  wglGetCurrentContext_;
    kage_wgl_get_proc_address_fn     wglGetProcAddress_;
    kage_wgl_swap_interval_fn        wglSwapIntervalEXT_;

    kage_gl_get_string_fn  glGetString_;
    kage_gl_get_error_fn   glGetError_;
    kage_gl_viewport_fn    glViewport_;
    kage_gl_clear_color_fn glClearColor_;
    kage_gl_clear_fn       glClear_;
    kage_gl_finish_fn      glFinish_;
    kage_gl_get_boolean_v_fn glGetBooleanv_;
    kage_gl_get_float_v_fn   glGetFloatv_;
    kage_gl_get_integer_v_fn glGetIntegerv_;
    kage_gl_is_enabled_fn    glIsEnabled_;
    kage_gl_enable_fn        glEnable_;
    kage_gl_disable_fn       glDisable_;
    kage_gl_color_mask_fn    glColorMask_;
    kage_gl_draw_buffer_fn   glDrawBuffer_;
    kage_gl_read_buffer_fn glReadBuffer_;
    kage_gl_pixel_store_i_fn glPixelStorei_;
    kage_gl_read_pixels_fn glReadPixels_;
} kage_pc_api;

typedef struct kage_pc_state {
    int mode;
    int ready;
    int accelerated;
    int vsync_enabled;
    int class_owned;
    int close_requested;
    uint32_t width;
    uint32_t height;
    unsigned clear_count;
    unsigned present_count;
    unsigned stage_counts[KAGE_STAGE_COUNT];
    unsigned menu_init_probe_counts[KAGE_MENU_INIT_PROBE_COUNT];
    int menu_init_probe_armed;
    int menu_init_probe_completed;
    int menu_render_probe_armed;
    int menu_render_probe_completed;
    unsigned menu_character_probe_counts[KAGE_MENU_CHARACTER_PROBE_COUNT];
    unsigned menu_character_wheel_call_count;
    unsigned menu_character_wheel_fixed_iterations;
    unsigned menu_character_wheel_dynamic_iterations;
    int menu_character_probe_armed;
    int menu_character_probe_completed;
    unsigned gl_trace_counts[KAGE_GL_TRACE_COUNT];
    unsigned vsync_change_count;
    int capture_requested;
    unsigned capture_target_present;
    int capture_arm_attempted;
    int capture_armed;
    int capture_present_attempted;
    int capture_finished;
    int clock_ready;
    LARGE_INTEGER clock_frequency;
    LARGE_INTEGER clock_origin;
    LONGLONG clock_last;
    HINSTANCE instance;
    HWND window;
    HDC dc;
    HGLRC context;
    uint8_t keyboard[KAGE_PC_KEY_COUNT];
    int input_trace_enabled;
    int gameplay_trace_enabled;
    int console_trace_enabled;
    unsigned keyboard_generation;
    unsigned keyboard_consumed_generation;
    unsigned console_command_sequence;
    HGLOBAL command_clipboard;
    DWORD command_clipboard_thread;
    uint32_t command_transport_sequence;
    uint32_t command_text_size;
    int command_pending;
    int command_open;
    int command_data_issued;
    int command_lock_seen;
    int command_unlock_seen;
    uint8_t probe_rgba[4];
    char gl_version[128];
    char error[256];
} kage_pc_state;

static const char kage_window_class[] = "repentogxm-kage-wgl";
static kage_pc_api api;
static kage_pc_state state;

static void set_error_text(const char *text)
{
    if (!text)
        text = "unknown KAGE PC backend error";
    _snprintf_s(state.error, sizeof state.error, _TRUNCATE, "%s", text);
}

static void set_win32_error(const char *stage)
{
    DWORD code = GetLastError();
    _snprintf_s(state.error, sizeof state.error, _TRUNCATE,
                "KAGE PC backend: %s failed (Win32 %lu)", stage,
                (unsigned long)code);
}

static void reset_clock(void)
{
    state.clock_ready = 0;
    state.clock_frequency.QuadPart = 0;
    state.clock_origin.QuadPart = 0;
    state.clock_last = 0;
}

static int initialize_clock(void)
{
    reset_clock();
    if (!QueryPerformanceFrequency(&state.clock_frequency)) {
        set_win32_error("QueryPerformanceFrequency");
        return 0;
    }
    if (state.clock_frequency.QuadPart <= 0) {
        set_error_text("KAGE PC backend: QPC frequency is not positive");
        return 0;
    }
    if (!QueryPerformanceCounter(&state.clock_origin)) {
        set_win32_error("QueryPerformanceCounter origin");
        return 0;
    }
    state.clock_ready = 1;
    return 1;
}

static FARPROC load_required(HMODULE module, const char *name)
{
    FARPROC proc = module ? GetProcAddress(module, name) : NULL;
    if (!proc) {
        _snprintf_s(state.error, sizeof state.error, _TRUNCATE,
                    "KAGE PC backend: missing %s", name);
    }
    return proc;
}

#define LOAD_REQUIRED(module, field, type, symbol)                     \
    do {                                                               \
        (field) = (type)load_required((module), (symbol));             \
        if (!(field))                                                   \
            goto fail;                                                  \
    } while (0)

static int valid_wgl_extension(PROC proc)
{
    uintptr_t value = (uintptr_t)proc;
    return proc && value != 1u && value != 2u && value != 3u &&
           value != ~(uintptr_t)0;
}

typedef struct kage_pc_key_map {
    uint16_t scancode;
    uint16_t key;
} kage_pc_key_map;

/* The game callback consumes GLFW key tokens, not Win32 virtual-key values.
 * Keep that numeric contract without constructing a GLFW window or calling
 * any guest callback from WndProc.  This is the GLFW 3.3 Win32 physical-key
 * table expressed as inert data; extended scancodes have bit 0x100 set.
 * https://github.com/glfw/glfw/blob/3.3.8/src/win32_init.c */
static const kage_pc_key_map kage_pc_key_map_table[] = {
    {0x00bu, '0'}, {0x002u, '1'}, {0x003u, '2'}, {0x004u, '3'},
    {0x005u, '4'}, {0x006u, '5'}, {0x007u, '6'}, {0x008u, '7'},
    {0x009u, '8'}, {0x00au, '9'},
    {0x01eu, 'A'}, {0x030u, 'B'}, {0x02eu, 'C'}, {0x020u, 'D'},
    {0x012u, 'E'}, {0x021u, 'F'}, {0x022u, 'G'}, {0x023u, 'H'},
    {0x017u, 'I'}, {0x024u, 'J'}, {0x025u, 'K'}, {0x026u, 'L'},
    {0x032u, 'M'}, {0x031u, 'N'}, {0x018u, 'O'}, {0x019u, 'P'},
    {0x010u, 'Q'}, {0x013u, 'R'}, {0x01fu, 'S'}, {0x014u, 'T'},
    {0x016u, 'U'}, {0x02fu, 'V'}, {0x011u, 'W'}, {0x02du, 'X'},
    {0x015u, 'Y'}, {0x02cu, 'Z'},
    {0x028u, 39u},  {0x02bu, 92u},  {0x033u, 44u},
    {0x00du, 61u},  {0x029u, 96u},  {0x01au, 91u},
    {0x00cu, 45u},  {0x034u, 46u},  {0x01bu, 93u},
    {0x027u, 59u},  {0x035u, 47u},  {0x056u, 162u},
    {0x00eu, 259u}, {0x153u, 261u}, {0x14fu, 269u},
    {0x01cu, 257u}, {0x001u, 256u}, {0x147u, 268u},
    {0x152u, 260u}, {0x15du, 348u}, {0x151u, 267u},
    {0x149u, 266u}, {0x045u, 284u}, {0x039u, 32u},
    {0x00fu, 258u}, {0x03au, 280u}, {0x145u, 282u},
    {0x046u, 281u},
    {0x03bu, 290u}, {0x03cu, 291u}, {0x03du, 292u},
    {0x03eu, 293u}, {0x03fu, 294u}, {0x040u, 295u},
    {0x041u, 296u}, {0x042u, 297u}, {0x043u, 298u},
    {0x044u, 299u}, {0x057u, 300u}, {0x058u, 301u},
    {0x064u, 302u}, {0x065u, 303u}, {0x066u, 304u},
    {0x067u, 305u}, {0x068u, 306u}, {0x069u, 307u},
    {0x06au, 308u}, {0x06bu, 309u}, {0x06cu, 310u},
    {0x06du, 311u}, {0x06eu, 312u}, {0x076u, 313u},
    {0x038u, 342u}, {0x01du, 341u}, {0x02au, 340u},
    {0x15bu, 343u}, {0x137u, 283u}, {0x138u, 346u},
    {0x11du, 345u}, {0x036u, 344u}, {0x15cu, 347u},
    {0x150u, 264u}, {0x14bu, 263u}, {0x14du, 262u},
    {0x148u, 265u},
    {0x052u, 320u}, {0x04fu, 321u}, {0x050u, 322u},
    {0x051u, 323u}, {0x04bu, 324u}, {0x04cu, 325u},
    {0x04du, 326u}, {0x047u, 327u}, {0x048u, 328u},
    {0x049u, 329u}, {0x04eu, 334u}, {0x053u, 330u},
    {0x135u, 331u}, {0x11cu, 335u}, {0x059u, 336u},
    {0x037u, 332u}, {0x04au, 333u}
};

static uint16_t kage_pc_scancode_from_lparam(LPARAM lparam)
{
    const uintptr_t bits = (uintptr_t)lparam;
    const uint16_t extended = (bits & ((uintptr_t)1u << 24)) ? 0x100u : 0u;
    return (uint16_t)(((bits >> 16) & 0xffu) | extended);
}

static int kage_pc_key_from_virtual_key(WPARAM virtual_key, LPARAM lparam)
{
    const uint16_t scancode = kage_pc_scancode_from_lparam(lparam);
    const int extended = !!(scancode & 0x100u);
    size_t index;

    for (index = 0; index < sizeof kage_pc_key_map_table /
                                sizeof kage_pc_key_map_table[0]; ++index) {
        if (kage_pc_key_map_table[index].scancode == scancode)
            return (int)kage_pc_key_map_table[index].key;
    }

    /* Synthetic messages are permitted to omit a hardware scancode.  Keep a
     * small layout-independent fallback for the controls needed by the menu. */
    if ((virtual_key >= (WPARAM)'A' && virtual_key <= (WPARAM)'Z') ||
        (virtual_key >= (WPARAM)'0' && virtual_key <= (WPARAM)'9'))
        return (int)virtual_key;
    switch (virtual_key) {
    case VK_SPACE:  return 32;
    case VK_ESCAPE: return 256;
    case VK_RETURN: return extended ? 335 : 257;
    case VK_LEFT:   return 263;
    case VK_RIGHT:  return 262;
    case VK_DOWN:   return 264;
    case VK_UP:     return 265;
    default:        return -1;
    }
}

static int kage_pc_console_trace_environment_requested(void)
{
    char value[2];
    const DWORD length = GetEnvironmentVariableA(
        "REPENTOGXM_PC_CONSOLE_TRACE", value, (DWORD)sizeof value);
    return length == 1u && value[0] == '1';
}

static uint32_t kage_pc_load_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static int kage_pc_command_text_valid(const uint8_t *text, uint32_t size)
{
    uint32_t index;

    /* At least one printable command byte, LF and NUL.  `size` includes NUL;
     * KAGE_PC_COMMAND_TEXT_MAX is the maximum pre-NUL payload size. */
    if (!text || size < 3u || size > KAGE_PC_COMMAND_TEXT_MAX + 1u ||
        text[size - 1u] != 0u || text[size - 2u] != (uint8_t)'\n')
        return 0;
    for (index = 0u; index + 1u < size; ++index) {
        const uint8_t value = text[index];
        if (value == (uint8_t)'\n') {
            if (index == 0u || text[index - 1u] == (uint8_t)'\n')
                return 0;
        } else if (value < 0x20u || value > 0x7eu) {
            /* This also rejects every embedded NUL. */
            return 0;
        }
    }
    return 1;
}

static int kage_pc_virtual_clipboard_release(void)
{
    HGLOBAL handle = state.command_clipboard;

    if (handle && GlobalFree(handle) != NULL) {
        set_win32_error("GlobalFree private command clipboard");
        return 0;
    }
    state.command_clipboard = NULL;
    state.command_clipboard_thread = 0u;
    state.command_transport_sequence = 0u;
    state.command_text_size = 0u;
    state.command_pending = 0;
    state.command_open = 0;
    state.command_data_issued = 0;
    state.command_lock_seen = 0;
    state.command_unlock_seen = 0;
    return 1;
}

static LRESULT kage_pc_command_copydata(HWND window, WPARAM source,
                                        LPARAM parameter)
{
    const COPYDATASTRUCT *copy = (const COPYDATASTRUCT *)(uintptr_t)parameter;
    const uint8_t *packet;
    const uint8_t *text;
    HGLOBAL fresh;
    HGLOBAL released;
    void *destination;
    DWORD unlock_error;
    DWORD release_error;
    uint32_t version;
    uint32_t target_pid;
    uint32_t sequence;
    uint32_t text_size;

    /* This is a local automation transport, not an authentication boundary.
     * A same-integrity process able to forge this message can already forge
     * keyboard messages.  Exact target/sequence/grammar validation prevents
     * accidental cross-worker delivery and stale overwrite. */
    if (!state.ready || !state.console_trace_enabled ||
        window != state.window || source != 0u || !copy ||
        copy->dwData != (ULONG_PTR)KAGE_PC_COMMAND_COPYDATA_MAGIC ||
        copy->cbData < KAGE_PC_COMMAND_COPYDATA_HEADER_SIZE ||
        !copy->lpData || state.command_pending || state.command_open)
        return FALSE;

    packet = (const uint8_t *)copy->lpData;
    version = kage_pc_load_le32(packet + 0u);
    target_pid = kage_pc_load_le32(packet + 4u);
    sequence = kage_pc_load_le32(packet + 8u);
    text_size = kage_pc_load_le32(packet + 12u);
    if (version != KAGE_PC_COMMAND_COPYDATA_VERSION ||
        target_pid != GetCurrentProcessId() || !sequence ||
        state.command_transport_sequence == UINT32_MAX ||
        sequence != state.command_transport_sequence + 1u ||
        text_size < 3u || text_size > KAGE_PC_COMMAND_TEXT_MAX + 1u ||
        copy->cbData != KAGE_PC_COMMAND_COPYDATA_HEADER_SIZE + text_size)
        return FALSE;
    text = packet + KAGE_PC_COMMAND_COPYDATA_HEADER_SIZE;
    if (!kage_pc_command_text_valid(text, text_size))
        return FALSE;

    /* Allocate and populate the replacement completely before touching the
     * retained old handle.  A rejected packet therefore leaves the previous
     * transport sequence and payload intact. */
    fresh = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)text_size);
    if (!fresh) {
        set_win32_error("GlobalAlloc private command clipboard");
        return FALSE;
    }
    destination = GlobalLock(fresh);
    if (!destination) {
        release_error = GetLastError();
        GlobalFree(fresh);
        SetLastError(release_error);
        set_win32_error("GlobalLock private command clipboard");
        return FALSE;
    }
    memcpy(destination, text, text_size);
    SetLastError(ERROR_SUCCESS);
    if (!GlobalUnlock(fresh) && (unlock_error = GetLastError()) != ERROR_SUCCESS) {
        GlobalFree(fresh);
        SetLastError(unlock_error);
        set_win32_error("GlobalUnlock private command clipboard");
        return FALSE;
    }

    if (state.command_clipboard) {
        SetLastError(ERROR_SUCCESS);
        released = GlobalFree(state.command_clipboard);
        if (released != NULL) {
            release_error = GetLastError();
            GlobalFree(fresh);
            SetLastError(release_error);
            set_win32_error("GlobalFree replaced command clipboard");
            return FALSE;
        }
    }
    state.command_clipboard = fresh;
    state.command_clipboard_thread = GetCurrentThreadId();
    state.command_transport_sequence = sequence;
    state.command_text_size = text_size;
    state.command_pending = 1;
    state.command_open = 0;
    state.command_data_issued = 0;
    state.command_lock_seen = 0;
    state.command_unlock_seen = 0;
    return TRUE;
}

static int kage_pc_input_trace_environment_requested(void)
{
    char value[2];
    const DWORD length = GetEnvironmentVariableA(
        "REPENTOGXM_PC_INPUT_TRACE", value, (DWORD)sizeof value);
    return length == 1u && value[0] == '1';
}

static int kage_pc_gameplay_trace_environment_requested(void)
{
    char value[2];
    const DWORD length = GetEnvironmentVariableA(
        "REPENTOGXM_PC_GAMEPLAY_TRACE", value, (DWORD)sizeof value);
    return length == 1u && value[0] == '1';
}

static int kage_pc_read_guest_u32(uint32_t address, uint32_t *value)
{
    SIZE_T copied = 0u;

    if (!address || !value ||
        !ReadProcessMemory(GetCurrentProcess(),
                           (const void *)(uintptr_t)address,
                           value, sizeof *value, &copied) ||
        copied != sizeof *value)
        return 0;
    return 1;
}

static int kage_pc_add_guest_offset(uint32_t base, uint32_t offset,
                                    uint32_t *address)
{
    if (!address || base > UINT32_MAX - offset)
        return 0;
    *address = base + offset;
    return 1;
}

static int kage_pc_sample_player(uint32_t *player, float *x, float *y,
                                 uint32_t *room, uint32_t *dimension)
{
    uint32_t game;
    uint32_t room_address;
    uint32_t dimension_address;
    uint32_t manager;
    uint32_t begin_address;
    uint32_t end_address;
    uint32_t begin;
    uint32_t end;
    uint32_t entity_type_address;
    uint32_t entity_type;
    uint32_t vptr;
    uint32_t x_address;
    uint32_t y_address;
    uint32_t x_bits;
    uint32_t y_bits;

    if (!player || !x || !y || !room || !dimension ||
        !kage_pc_read_guest_u32(KAGE_GAME_GLOBAL_POINTER, &game) ||
        !game ||
        !kage_pc_add_guest_offset(game, KAGE_GAME_ROOM_INDEX,
                                  &room_address) ||
        !kage_pc_add_guest_offset(game, KAGE_GAME_DIMENSION,
                                  &dimension_address) ||
        !kage_pc_read_guest_u32(room_address, room) ||
        !kage_pc_read_guest_u32(dimension_address, dimension) ||
        !kage_pc_add_guest_offset(game, KAGE_GAME_PLAYER_MANAGER, &manager) ||
        !kage_pc_add_guest_offset(manager, KAGE_PLAYER_VECTOR_BEGIN,
                                  &begin_address) ||
        !kage_pc_add_guest_offset(manager, KAGE_PLAYER_VECTOR_END,
                                  &end_address) ||
        !kage_pc_read_guest_u32(begin_address, &begin) ||
        !kage_pc_read_guest_u32(end_address, &end) ||
        (begin & 3u) || (end & 3u) || end < begin || end - begin < 4u ||
        end - begin > 8u * sizeof(uint32_t) ||
        !kage_pc_read_guest_u32(begin, player) || !*player ||
        !kage_pc_read_guest_u32(*player, &vptr) ||
        vptr != KAGE_ENTITY_PLAYER_VPTR ||
        !kage_pc_add_guest_offset(*player, KAGE_ENTITY_TYPE_OFFSET,
                                  &entity_type_address) ||
        !kage_pc_read_guest_u32(entity_type_address, &entity_type) ||
        entity_type != 1u ||
        !kage_pc_add_guest_offset(*player, KAGE_PLAYER_POSITION_X,
                                  &x_address) ||
        !kage_pc_add_guest_offset(*player, KAGE_PLAYER_POSITION_Y,
                                  &y_address) ||
        !kage_pc_read_guest_u32(x_address, &x_bits) ||
        !kage_pc_read_guest_u32(y_address, &y_bits) ||
        (x_bits & 0x7f800000u) == 0x7f800000u ||
        (y_bits & 0x7f800000u) == 0x7f800000u)
        return 0;
    memcpy(x, &x_bits, sizeof *x);
    memcpy(y, &y_bits, sizeof *y);
    return 1;
}

static void kage_pc_note_gameplay_sample(unsigned present)
{
    uint32_t player;
    uint32_t room;
    uint32_t dimension;
    float x;
    float y;

    if (!state.gameplay_trace_enabled || present < 720u ||
        present % 30u != 0u)
        return;
    if (!kage_pc_sample_player(&player, &x, &y, &room, &dimension)) {
        fprintf(stderr,
                "PC KAGE gameplay sample : Present %u unavailable "
                "key_d=%u key_s=%u\n",
                present, (unsigned)state.keyboard['D'],
                (unsigned)state.keyboard['S']);
    } else {
        fprintf(stderr,
                "PC KAGE gameplay sample : Present %u player=%08lx "
                "room=%lu dimension=%lu x=%.3f y=%.3f key_d=%u key_s=%u\n",
                present, (unsigned long)player, (unsigned long)room,
                (unsigned long)dimension, (double)x, (double)y,
                (unsigned)state.keyboard['D'],
                (unsigned)state.keyboard['S']);
    }
    fflush(stderr);
}

static void kage_pc_keyboard_reset(void)
{
    memset(state.keyboard, 0, sizeof state.keyboard);
    state.keyboard_generation = 0u;
    state.keyboard_consumed_generation = 0u;
}

static void kage_pc_keyboard_advance_generation(void)
{
    state.keyboard_generation++;
    if (!state.keyboard_generation) {
        state.keyboard_generation = 1u;
        state.keyboard_consumed_generation = 0u;
    }
}

static void kage_pc_keyboard_clear(void)
{
    size_t index;
    int changed = 0;

    for (index = 0; index < sizeof state.keyboard; ++index) {
        if (state.keyboard[index]) {
            changed = 1;
            break;
        }
    }
    memset(state.keyboard, 0, sizeof state.keyboard);
    if (changed)
        kage_pc_keyboard_advance_generation();
}

static void kage_pc_keyboard_message(UINT message, WPARAM wparam,
                                     LPARAM lparam)
{
    const int pressed = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const int key = kage_pc_key_from_virtual_key(wparam, lparam);
    const uint8_t next = (uint8_t)(pressed ? 1u : 0u);
    int changed = 0;

    if (key >= 0 && key < (int)KAGE_PC_KEY_COUNT &&
        state.keyboard[key] != next) {
        state.keyboard[key] = next;
        changed = 1;
    }
    if (!pressed && wparam == VK_SHIFT) {
        /* Windows can omit one release when both Shift keys were held. */
        changed |= state.keyboard[340] != 0u || state.keyboard[344] != 0u;
        state.keyboard[340] = 0u;
        state.keyboard[344] = 0u;
    }
    if (!changed)
        return;

    kage_pc_keyboard_advance_generation();
    if (state.input_trace_enabled) {
        fprintf(stderr,
                "PC KAGE input key : vk=0x%04lx scancode=0x%03x "
                "key=%d down=%d generation=%u\n",
                (unsigned long)wparam,
                (unsigned)kage_pc_scancode_from_lparam(lparam), key,
                pressed, state.keyboard_generation);
        fflush(stderr);
    }
}

static LRESULT CALLBACK kage_window_proc(HWND window, UINT message,
                                         WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_COPYDATA:
        return kage_pc_command_copydata(window, wparam, lparam);
    case WM_CLOSE:
        state.close_requested = 1;
        return 0;
    case WM_KILLFOCUS:
        kage_pc_keyboard_clear();
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
        kage_pc_keyboard_message(message, wparam, lparam);
        break;
    default:
        break;
    }
    if (api.DefWindowProcA_)
        return api.DefWindowProcA_(window, message, wparam, lparam);
    return 0;
}

static void unload_api(void)
{
    HMODULE user32 = api.user32;
    HMODULE gdi32 = api.gdi32;
    HMODULE opengl32 = api.opengl32;
    memset(&api, 0, sizeof api);
    if (opengl32)
        FreeLibrary(opengl32);
    if (gdi32)
        FreeLibrary(gdi32);
    if (user32)
        FreeLibrary(user32);
}

static void release_resources(void)
{
    if (state.context && api.wglMakeCurrent_)
        api.wglMakeCurrent_(NULL, NULL);
    if (state.context && api.wglDeleteContext_)
        api.wglDeleteContext_(state.context);
    state.context = NULL;
    if (state.dc && state.window && api.ReleaseDC_)
        api.ReleaseDC_(state.window, state.dc);
    state.dc = NULL;
    if (state.window && api.DestroyWindow_)
        api.DestroyWindow_(state.window);
    state.window = NULL;
    if (state.class_owned && api.UnregisterClassA_)
        api.UnregisterClassA_(kage_window_class, state.instance);
    state.class_owned = 0;
    unload_api();
    state.ready = 0;
    state.vsync_enabled = 0;
    state.input_trace_enabled = 0;
    state.gameplay_trace_enabled = 0;
    state.console_trace_enabled = 0;
    state.console_command_sequence = 0u;
    (void)kage_pc_virtual_clipboard_release();
    kage_pc_keyboard_reset();
}

static int load_api(void)
{
    api.user32 = LoadLibraryA("user32.dll");
    api.gdi32 = LoadLibraryA("gdi32.dll");
    api.opengl32 = LoadLibraryA("opengl32.dll");
    if (!api.user32 || !api.gdi32 || !api.opengl32) {
        set_win32_error("LoadLibrary Win32/OpenGL");
        goto fail;
    }

    LOAD_REQUIRED(api.user32, api.RegisterClassExA_,
                  ATOM (WINAPI *)(const WNDCLASSEXA *), "RegisterClassExA");
    LOAD_REQUIRED(api.user32, api.UnregisterClassA_,
                  BOOL (WINAPI *)(LPCSTR, HINSTANCE), "UnregisterClassA");
    LOAD_REQUIRED(api.user32, api.CreateWindowExA_,
                  HWND (WINAPI *)(DWORD, LPCSTR, LPCSTR, DWORD, int, int,
                                  int, int, HWND, HMENU, HINSTANCE, LPVOID),
                  "CreateWindowExA");
    LOAD_REQUIRED(api.user32, api.DestroyWindow_,
                  BOOL (WINAPI *)(HWND), "DestroyWindow");
    LOAD_REQUIRED(api.user32, api.DefWindowProcA_,
                  LRESULT (WINAPI *)(HWND, UINT, WPARAM, LPARAM),
                  "DefWindowProcA");
    LOAD_REQUIRED(api.user32, api.GetDC_, HDC (WINAPI *)(HWND), "GetDC");
    LOAD_REQUIRED(api.user32, api.ReleaseDC_,
                  int (WINAPI *)(HWND, HDC), "ReleaseDC");
    LOAD_REQUIRED(api.user32, api.ShowWindow_,
                  BOOL (WINAPI *)(HWND, int), "ShowWindow");
    LOAD_REQUIRED(api.user32, api.UpdateWindow_,
                  BOOL (WINAPI *)(HWND), "UpdateWindow");
    LOAD_REQUIRED(api.user32, api.PeekMessageA_,
                  BOOL (WINAPI *)(LPMSG, HWND, UINT, UINT, UINT),
                  "PeekMessageA");
    LOAD_REQUIRED(api.user32, api.TranslateMessage_,
                  BOOL (WINAPI *)(const MSG *), "TranslateMessage");
    LOAD_REQUIRED(api.user32, api.DispatchMessageA_,
                  LRESULT (WINAPI *)(const MSG *), "DispatchMessageA");
    LOAD_REQUIRED(api.user32, api.EnumDisplaySettingsA_,
                  BOOL (WINAPI *)(LPCSTR, DWORD, DEVMODEA *),
                  "EnumDisplaySettingsA");

    LOAD_REQUIRED(api.gdi32, api.ChoosePixelFormat_,
                  int (WINAPI *)(HDC, const PIXELFORMATDESCRIPTOR *),
                  "ChoosePixelFormat");
    LOAD_REQUIRED(api.gdi32, api.DescribePixelFormat_,
                  int (WINAPI *)(HDC, int, UINT, LPPIXELFORMATDESCRIPTOR),
                  "DescribePixelFormat");
    LOAD_REQUIRED(api.gdi32, api.SetPixelFormat_,
                  BOOL (WINAPI *)(HDC, int, const PIXELFORMATDESCRIPTOR *),
                  "SetPixelFormat");
    LOAD_REQUIRED(api.gdi32, api.SwapBuffers_,
                  BOOL (WINAPI *)(HDC), "SwapBuffers");

    LOAD_REQUIRED(api.opengl32, api.wglCreateContext_,
                  kage_wgl_create_context_fn, "wglCreateContext");
    LOAD_REQUIRED(api.opengl32, api.wglDeleteContext_,
                  kage_wgl_delete_context_fn, "wglDeleteContext");
    LOAD_REQUIRED(api.opengl32, api.wglMakeCurrent_,
                  kage_wgl_make_current_fn, "wglMakeCurrent");
    LOAD_REQUIRED(api.opengl32, api.wglGetCurrentContext_,
                  kage_wgl_get_current_context_fn, "wglGetCurrentContext");
    LOAD_REQUIRED(api.opengl32, api.wglGetProcAddress_,
                  kage_wgl_get_proc_address_fn, "wglGetProcAddress");

    LOAD_REQUIRED(api.opengl32, api.glGetString_,
                  kage_gl_get_string_fn, "glGetString");
    LOAD_REQUIRED(api.opengl32, api.glGetError_,
                  kage_gl_get_error_fn, "glGetError");
    LOAD_REQUIRED(api.opengl32, api.glViewport_,
                  kage_gl_viewport_fn, "glViewport");
    LOAD_REQUIRED(api.opengl32, api.glClearColor_,
                  kage_gl_clear_color_fn, "glClearColor");
    LOAD_REQUIRED(api.opengl32, api.glClear_,
                  kage_gl_clear_fn, "glClear");
    LOAD_REQUIRED(api.opengl32, api.glFinish_,
                  kage_gl_finish_fn, "glFinish");
    LOAD_REQUIRED(api.opengl32, api.glGetBooleanv_,
                  kage_gl_get_boolean_v_fn, "glGetBooleanv");
    LOAD_REQUIRED(api.opengl32, api.glGetFloatv_,
                  kage_gl_get_float_v_fn, "glGetFloatv");
    LOAD_REQUIRED(api.opengl32, api.glGetIntegerv_,
                  kage_gl_get_integer_v_fn, "glGetIntegerv");
    LOAD_REQUIRED(api.opengl32, api.glIsEnabled_,
                  kage_gl_is_enabled_fn, "glIsEnabled");
    LOAD_REQUIRED(api.opengl32, api.glEnable_,
                  kage_gl_enable_fn, "glEnable");
    LOAD_REQUIRED(api.opengl32, api.glDisable_,
                  kage_gl_disable_fn, "glDisable");
    LOAD_REQUIRED(api.opengl32, api.glColorMask_,
                  kage_gl_color_mask_fn, "glColorMask");
    LOAD_REQUIRED(api.opengl32, api.glDrawBuffer_,
                  kage_gl_draw_buffer_fn, "glDrawBuffer");
    LOAD_REQUIRED(api.opengl32, api.glReadBuffer_,
                  kage_gl_read_buffer_fn, "glReadBuffer");
    LOAD_REQUIRED(api.opengl32, api.glPixelStorei_,
                  kage_gl_pixel_store_i_fn, "glPixelStorei");
    LOAD_REQUIRED(api.opengl32, api.glReadPixels_,
                  kage_gl_read_pixels_fn, "glReadPixels");
    return 1;

fail:
    unload_api();
    return 0;
}

static int probe_component(uint8_t actual, unsigned expected)
{
    return actual + 2u >= expected && actual <= expected + 2u;
}

static int capture_environment_requested(void)
{
    char value[2];
    DWORD length = GetEnvironmentVariableA("REPENTOGXM_PC_CAPTURE", value,
                                           (DWORD)sizeof value);
    return length == 1u && value[0] == '1';
}

static int capture_target_from_environment(unsigned *target, char *reason,
                                           size_t reason_size)
{
    char value[32];
    DWORD length;
    DWORD error;
    unsigned parsed = 0u;
    DWORD index;

    if (!target || !reason || !reason_size)
        return 0;
    SetLastError(ERROR_SUCCESS);
    length = GetEnvironmentVariableA("REPENTOGXM_PC_CAPTURE_PRESENT", value,
                                     (DWORD)sizeof value);
    if (!length) {
        error = GetLastError();
        if (error == ERROR_ENVVAR_NOT_FOUND) {
            *target = 1u;
            return 1;
        }
        _snprintf_s(reason, reason_size, _TRUNCATE,
                    "could not read REPENTOGXM_PC_CAPTURE_PRESENT (Win32 %lu)",
                    (unsigned long)error);
        return 0;
    }
    if (length >= (DWORD)sizeof value) {
        _snprintf_s(reason, reason_size, _TRUNCATE,
                    "REPENTOGXM_PC_CAPTURE_PRESENT is too long");
        return 0;
    }
    for (index = 0u; index < length; ++index) {
        unsigned digit;
        if (value[index] < '0' || value[index] > '9') {
            _snprintf_s(reason, reason_size, _TRUNCATE,
                        "REPENTOGXM_PC_CAPTURE_PRESENT must be a positive decimal integer");
            return 0;
        }
        digit = (unsigned)(value[index] - '0');
        if (parsed > (UINT_MAX - digit) / 10u) {
            _snprintf_s(reason, reason_size, _TRUNCATE,
                        "REPENTOGXM_PC_CAPTURE_PRESENT overflowed unsigned int");
            return 0;
        }
        parsed = parsed * 10u + digit;
    }
    if (!parsed) {
        _snprintf_s(reason, reason_size, _TRUNCATE,
                    "REPENTOGXM_PC_CAPTURE_PRESENT must be at least 1");
        return 0;
    }
    *target = parsed;
    return 1;
}

static void capture_remove_artifacts(void)
{
    DeleteFileA(KAGE_PC_CAPTURE_PPM_TEMP);
    DeleteFileA(KAGE_PC_CAPTURE_PPM);
}

static int capture_path_is_absent(const char *path, char *reason,
                                  size_t reason_size)
{
    DWORD attributes = GetFileAttributesA(path);
    DWORD error;
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        _snprintf_s(reason, reason_size, _TRUNCATE,
                    "could not remove stale artefact %s", path);
        return 0;
    }
    error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
        return 1;
    _snprintf_s(reason, reason_size, _TRUNCATE,
                "could not verify stale artefact %s was removed (Win32 %lu)",
                path, (unsigned long)error);
    return 0;
}

static void capture_fail(const char *reason)
{
    state.capture_armed = 0;
    state.capture_finished = 1;
    capture_remove_artifacts();
    fprintf(stderr, "PC KAGE capture FAILED: %s\n",
            reason ? reason : "unknown diagnostic error");
}

static int capture_gl_error(const char *stage)
{
    unsigned int error = api.glGetError_();
    char reason[160];
    if (!error)
        return 0;
    _snprintf_s(reason, sizeof reason, _TRUNCATE,
                "%s reported GL error 0x%x", stage, error);
    capture_fail(reason);
    return 1;
}

static int capture_drain_stale_gl_errors(const char *stage)
{
    unsigned int error = 0u;
    unsigned int attempts;
    char reason[192];

    /* The translated renderer can leave an error flag behind before this
     * opt-in observer runs.  Attribute every such flag explicitly, but keep
     * the drain bounded so a lost/broken context can never hide as success. */
    for (attempts = 0u; attempts < KAGE_CAPTURE_STALE_ERROR_LIMIT;
         ++attempts) {
        error = api.glGetError_();
        if (!error) {
            fprintf(stderr,
                    "PC KAGE capture GL drain : stage=%s stale_count=%u\n",
                    stage, attempts);
            return 0;
        }
        fprintf(stderr,
                "PC KAGE capture stale GL : stage=%s index=%u error=0x%x\n",
                stage, attempts, error);
    }

    _snprintf_s(reason, sizeof reason, _TRUNCATE,
                "%s retained GL errors after %u drains (last 0x%x)",
                stage, KAGE_CAPTURE_STALE_ERROR_LIMIT, error);
    capture_fail(reason);
    return 1;
}

static void restore_capability(unsigned int capability, int enabled)
{
    if (enabled)
        api.glEnable_(capability);
    else
        api.glDisable_(capability);
}

static void capture_arm_at_render_entry(void)
{
    int draw_framebuffer = -1;
    int draw_buffer = 0;
    int scissor_enabled;
    int dither_enabled;
    int srgb_enabled;
    unsigned char color_mask[4];
    float clear_color[4];
    unsigned int operation_error;
    unsigned int restore_error;
    char reason[192];

    state.capture_arm_attempted = 1;
    if (!state.ready || !state.context || !state.dc) {
        capture_fail("target Render entry arrived without a ready context");
        return;
    }
    if (!api.wglGetCurrentContext_ ||
        api.wglGetCurrentContext_() != state.context) {
        capture_fail("target Render entry did not own the KAGE GL context");
        return;
    }
    if (state.width != KAGE_CAPTURE_WIDTH ||
        state.height != KAGE_CAPTURE_HEIGHT) {
        _snprintf_s(reason, sizeof reason, _TRUNCATE,
                    "expected a %ux%u framebuffer, got %ux%u",
                    KAGE_CAPTURE_WIDTH, KAGE_CAPTURE_HEIGHT,
                    state.width, state.height);
        capture_fail(reason);
        return;
    }
    if (capture_drain_stale_gl_errors("pre-sentinel state"))
        return;

    api.glGetIntegerv_(KAGE_GL_DRAW_FRAMEBUFFER_BINDING, &draw_framebuffer);
    api.glGetIntegerv_(KAGE_GL_DRAW_BUFFER, &draw_buffer);
    api.glGetBooleanv_(KAGE_GL_COLOR_WRITEMASK, color_mask);
    api.glGetFloatv_(KAGE_GL_COLOR_CLEAR_VALUE, clear_color);
    scissor_enabled = !!api.glIsEnabled_(KAGE_GL_SCISSOR_TEST);
    dither_enabled = !!api.glIsEnabled_(KAGE_GL_DITHER);
    srgb_enabled = !!api.glIsEnabled_(KAGE_GL_FRAMEBUFFER_SRGB);
    if (capture_gl_error("sentinel state query"))
        return;
    if (draw_framebuffer != 0) {
        _snprintf_s(reason, sizeof reason, _TRUNCATE,
                    "target Render entry draw FBO was %d, expected 0",
                    draw_framebuffer);
        capture_fail(reason);
        return;
    }

    api.glDrawBuffer_(KAGE_GL_BACK);
    api.glDisable_(KAGE_GL_SCISSOR_TEST);
    api.glDisable_(KAGE_GL_DITHER);
    api.glDisable_(KAGE_GL_FRAMEBUFFER_SRGB);
    api.glColorMask_(1, 1, 1, 1);
    api.glClearColor_(1.0f, 0.0f, 1.0f, 1.0f);
    api.glClear_(KAGE_GL_COLOR_BUFFER_BIT);
    operation_error = api.glGetError_();

    api.glClearColor_(clear_color[0], clear_color[1],
                      clear_color[2], clear_color[3]);
    api.glColorMask_(color_mask[0], color_mask[1],
                     color_mask[2], color_mask[3]);
    restore_capability(KAGE_GL_FRAMEBUFFER_SRGB, srgb_enabled);
    restore_capability(KAGE_GL_DITHER, dither_enabled);
    restore_capability(KAGE_GL_SCISSOR_TEST, scissor_enabled);
    api.glDrawBuffer_((unsigned int)draw_buffer);
    restore_error = api.glGetError_();

    if (operation_error) {
        _snprintf_s(reason, sizeof reason, _TRUNCATE,
                    "magenta sentinel clear reported GL error 0x%x",
                    operation_error);
        capture_fail(reason);
        return;
    }
    if (restore_error) {
        _snprintf_s(reason, sizeof reason, _TRUNCATE,
                    "sentinel state restore reported GL error 0x%x",
                    restore_error);
        capture_fail(reason);
        return;
    }

    state.capture_armed = 1;
    memset(state.gl_trace_counts, 0, sizeof state.gl_trace_counts);
    fprintf(stderr,
            "PC KAGE capture armed     : magenta GL_BACK for game Present %u\n",
            state.capture_target_present);
}

static uint8_t *capture_read_first_present(unsigned long *different_pixels)
{
    int read_framebuffer = -1;
    int pack_buffer = -1;
    int read_buffer = 0;
    int pack_alignment = 0;
    int pack_row_length = 0;
    int pack_skip_rows = 0;
    int pack_skip_pixels = 0;
    int pack_swap_bytes = 0;
    int pack_lsb_first = 0;
    unsigned int operation_error;
    unsigned int restore_error;
    SIZE_T byte_count = (SIZE_T)KAGE_CAPTURE_WIDTH *
                        (SIZE_T)KAGE_CAPTURE_HEIGHT *
                        (SIZE_T)KAGE_CAPTURE_CHANNELS;
    uint8_t *pixels;
    unsigned long different = 0;
    unsigned long pixel_count = KAGE_CAPTURE_WIDTH * KAGE_CAPTURE_HEIGHT;
    unsigned long index;
    unsigned trace_index;
    char reason[192];

    if (!different_pixels) {
        capture_fail("internal null changed-pixel output");
        return NULL;
    }
    *different_pixels = 0;
    if (!state.capture_armed) {
        capture_fail("target game Present arrived before the Render sentinel");
        return NULL;
    }
    if (!api.wglGetCurrentContext_ ||
        api.wglGetCurrentContext_() != state.context) {
        capture_fail("target game Present did not own the KAGE GL context");
        return NULL;
    }
    for (trace_index = 0u; trace_index < KAGE_GL_TRACE_COUNT;
         ++trace_index) {
        fprintf(stderr, "PC KAGE guest GL calls : call=%s count=%u\n",
                kage_gl_trace_names[trace_index],
                state.gl_trace_counts[trace_index]);
    }
    if (capture_drain_stale_gl_errors("pre-readback state"))
        return NULL;

    api.glGetIntegerv_(KAGE_GL_READ_FRAMEBUFFER_BINDING, &read_framebuffer);
    api.glGetIntegerv_(KAGE_GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
    api.glGetIntegerv_(KAGE_GL_READ_BUFFER, &read_buffer);
    api.glGetIntegerv_(KAGE_GL_PACK_ALIGNMENT, &pack_alignment);
    api.glGetIntegerv_(KAGE_GL_PACK_ROW_LENGTH, &pack_row_length);
    api.glGetIntegerv_(KAGE_GL_PACK_SKIP_ROWS, &pack_skip_rows);
    api.glGetIntegerv_(KAGE_GL_PACK_SKIP_PIXELS, &pack_skip_pixels);
    api.glGetIntegerv_(KAGE_GL_PACK_SWAP_BYTES, &pack_swap_bytes);
    api.glGetIntegerv_(KAGE_GL_PACK_LSB_FIRST, &pack_lsb_first);
    if (capture_gl_error("readback state query"))
        return NULL;
    if (read_framebuffer != 0) {
        _snprintf_s(reason, sizeof reason, _TRUNCATE,
                    "target game Present read FBO was %d, expected 0",
                    read_framebuffer);
        capture_fail(reason);
        return NULL;
    }
    if (pack_buffer != 0) {
        _snprintf_s(reason, sizeof reason, _TRUNCATE,
                    "target game Present pack PBO was %d, expected 0",
                    pack_buffer);
        capture_fail(reason);
        return NULL;
    }

    pixels = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, byte_count);
    if (!pixels) {
        capture_fail("could not allocate the 960x540 RGBA readback");
        return NULL;
    }

    api.glReadBuffer_(KAGE_GL_BACK);
    api.glPixelStorei_(KAGE_GL_PACK_ALIGNMENT, 1);
    api.glPixelStorei_(KAGE_GL_PACK_ROW_LENGTH, 0);
    api.glPixelStorei_(KAGE_GL_PACK_SKIP_ROWS, 0);
    api.glPixelStorei_(KAGE_GL_PACK_SKIP_PIXELS, 0);
    api.glPixelStorei_(KAGE_GL_PACK_SWAP_BYTES, 0);
    api.glPixelStorei_(KAGE_GL_PACK_LSB_FIRST, 0);
    api.glReadPixels_(0, 0, (int)KAGE_CAPTURE_WIDTH,
                      (int)KAGE_CAPTURE_HEIGHT, KAGE_GL_RGBA,
                      KAGE_GL_UNSIGNED_BYTE, pixels);
    operation_error = api.glGetError_();

    api.glPixelStorei_(KAGE_GL_PACK_LSB_FIRST, pack_lsb_first);
    api.glPixelStorei_(KAGE_GL_PACK_SWAP_BYTES, pack_swap_bytes);
    api.glPixelStorei_(KAGE_GL_PACK_SKIP_PIXELS, pack_skip_pixels);
    api.glPixelStorei_(KAGE_GL_PACK_SKIP_ROWS, pack_skip_rows);
    api.glPixelStorei_(KAGE_GL_PACK_ROW_LENGTH, pack_row_length);
    api.glPixelStorei_(KAGE_GL_PACK_ALIGNMENT, pack_alignment);
    api.glReadBuffer_((unsigned int)read_buffer);
    restore_error = api.glGetError_();

    if (operation_error) {
        HeapFree(GetProcessHeap(), 0, pixels);
        _snprintf_s(reason, sizeof reason, _TRUNCATE,
                    "960x540 GL_BACK readback reported GL error 0x%x",
                    operation_error);
        capture_fail(reason);
        return NULL;
    }
    if (restore_error) {
        HeapFree(GetProcessHeap(), 0, pixels);
        _snprintf_s(reason, sizeof reason, _TRUNCATE,
                    "readback state restore reported GL error 0x%x",
                    restore_error);
        capture_fail(reason);
        return NULL;
    }

    for (index = 0; index < pixel_count; ++index) {
        const uint8_t *pixel = pixels + index * KAGE_CAPTURE_CHANNELS;
        if (pixel[0] != 255u || pixel[1] != 0u ||
            pixel[2] != 255u || pixel[3] != 255u)
            different++;
    }
    *different_pixels = different;
    return pixels;
}

static int capture_write_all(HANDLE file, const void *data, DWORD size,
                             char *reason, size_t reason_size)
{
    DWORD written = 0;
    if (!WriteFile(file, data, size, &written, NULL)) {
        DWORD error = GetLastError();
        _snprintf_s(reason, reason_size, _TRUNCATE,
                    "writing %s failed (Win32 %lu)",
                    KAGE_PC_CAPTURE_PPM_TEMP, (unsigned long)error);
        return 0;
    }
    if (written != size) {
        _snprintf_s(reason, reason_size, _TRUNCATE,
                    "writing %s was short (%lu/%lu bytes)",
                    KAGE_PC_CAPTURE_PPM_TEMP, (unsigned long)written,
                    (unsigned long)size);
        return 0;
    }
    return 1;
}

static int capture_write_ppm_atomic(const uint8_t *pixels,
                                    char *reason, size_t reason_size)
{
    HANDLE file = INVALID_HANDLE_VALUE;
    uint8_t *row = NULL;
    char header[64];
    int header_size;
    unsigned int output_y;
    int ok = 0;

    if (!pixels) {
        _snprintf_s(reason, reason_size, _TRUNCATE,
                    "internal null image at PPM write");
        return 0;
    }
    DeleteFileA(KAGE_PC_CAPTURE_PPM_TEMP);
    file = CreateFileA(KAGE_PC_CAPTURE_PPM_TEMP, GENERIC_WRITE, 0, NULL,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        _snprintf_s(reason, reason_size, _TRUNCATE,
                    "creating %s failed (Win32 %lu)",
                    KAGE_PC_CAPTURE_PPM_TEMP, (unsigned long)error);
        goto done;
    }
    row = (uint8_t *)HeapAlloc(GetProcessHeap(), 0,
                               (SIZE_T)KAGE_CAPTURE_WIDTH * 3u);
    if (!row) {
        _snprintf_s(reason, reason_size, _TRUNCATE,
                    "could not allocate one PPM output row");
        goto done;
    }
    header_size = _snprintf_s(header, sizeof header, _TRUNCATE,
                              "P6\n%u %u\n255\n", KAGE_CAPTURE_WIDTH,
                              KAGE_CAPTURE_HEIGHT);
    if (header_size <= 0 ||
        !capture_write_all(file, header, (DWORD)header_size,
                           reason, reason_size))
        goto done;

    for (output_y = 0; output_y < KAGE_CAPTURE_HEIGHT; ++output_y) {
        unsigned int source_y = KAGE_CAPTURE_HEIGHT - 1u - output_y;
        const uint8_t *source = pixels +
            (SIZE_T)source_y * KAGE_CAPTURE_WIDTH * KAGE_CAPTURE_CHANNELS;
        unsigned int x;
        for (x = 0; x < KAGE_CAPTURE_WIDTH; ++x) {
            row[x * 3u + 0u] = source[x * KAGE_CAPTURE_CHANNELS + 0u];
            row[x * 3u + 1u] = source[x * KAGE_CAPTURE_CHANNELS + 1u];
            row[x * 3u + 2u] = source[x * KAGE_CAPTURE_CHANNELS + 2u];
        }
        if (!capture_write_all(file, row, KAGE_CAPTURE_WIDTH * 3u,
                               reason, reason_size))
            goto done;
    }
    if (!FlushFileBuffers(file)) {
        DWORD error = GetLastError();
        _snprintf_s(reason, reason_size, _TRUNCATE,
                    "flushing %s failed (Win32 %lu)",
                    KAGE_PC_CAPTURE_PPM_TEMP, (unsigned long)error);
        goto done;
    }
    if (!CloseHandle(file)) {
        DWORD error = GetLastError();
        file = INVALID_HANDLE_VALUE;
        _snprintf_s(reason, reason_size, _TRUNCATE,
                    "closing %s failed (Win32 %lu)",
                    KAGE_PC_CAPTURE_PPM_TEMP, (unsigned long)error);
        goto done;
    }
    file = INVALID_HANDLE_VALUE;
    if (!MoveFileExA(KAGE_PC_CAPTURE_PPM_TEMP, KAGE_PC_CAPTURE_PPM,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD error = GetLastError();
        _snprintf_s(reason, reason_size, _TRUNCATE,
                    "publishing %s failed (Win32 %lu)",
                    KAGE_PC_CAPTURE_PPM, (unsigned long)error);
        goto done;
    }
    ok = 1;

done:
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    if (row)
        HeapFree(GetProcessHeap(), 0, row);
    if (!ok)
        DeleteFileA(KAGE_PC_CAPTURE_PPM_TEMP);
    return ok;
}

static int prove_clear_and_present(void)
{
    unsigned int error;
    unsigned int stale = 0u;
    unsigned int attempts;
    int x = (int)(state.width / 2u);
    int y = (int)(state.height / 2u);

    /* Drain pre-existing driver errors, but never let a lost context turn the
     * smoke proof into an unbounded wait. */
    for (attempts = 0u; attempts < 16u; ++attempts) {
        stale = api.glGetError_();
        if (!stale)
            break;
    }
    if (stale) {
        _snprintf_s(state.error, sizeof state.error, _TRUNCATE,
                    "KAGE PC backend: persistent pre-clear GL error 0x%x",
                    stale);
        return 0;
    }
    api.glViewport_(0, 0, (int)state.width, (int)state.height);
    api.glReadBuffer_(KAGE_GL_BACK);
    api.glClearColor_(0.125f, 0.25f, 0.5f, 1.0f);
    api.glClear_(KAGE_GL_COLOR_BUFFER_BIT);
    api.glFinish_();
    memset(state.probe_rgba, 0, sizeof state.probe_rgba);
    api.glReadPixels_(x, y, 1, 1, KAGE_GL_RGBA, KAGE_GL_UNSIGNED_BYTE,
                      state.probe_rgba);
    error = api.glGetError_();
    if (error != 0u) {
        _snprintf_s(state.error, sizeof state.error, _TRUNCATE,
                    "KAGE PC backend: GL clear/readback error 0x%x", error);
        return 0;
    }
    if (!probe_component(state.probe_rgba[0], 32u) ||
        !probe_component(state.probe_rgba[1], 64u) ||
        !probe_component(state.probe_rgba[2], 128u)) {
        _snprintf_s(state.error, sizeof state.error, _TRUNCATE,
                    "KAGE PC backend: clear readback was %u,%u,%u,%u",
                    state.probe_rgba[0], state.probe_rgba[1],
                    state.probe_rgba[2], state.probe_rgba[3]);
        return 0;
    }
    state.clear_count++;
    if (!api.SwapBuffers_(state.dc)) {
        set_win32_error("initial SwapBuffers");
        return 0;
    }
    state.present_count++;
    return 1;
}

int kage_pc_backend_set_mode(int mode)
{
    if (mode != KAGE_PC_BACKEND_DISABLED &&
        mode != KAGE_PC_BACKEND_HIDDEN &&
        mode != KAGE_PC_BACKEND_VISIBLE) {
        set_error_text("KAGE PC backend: invalid explicit mode");
        return 0;
    }
    if (state.mode != mode) {
        if (state.ready)
            kage_pc_backend_shutdown();
        else {
            reset_clock();
            state.input_trace_enabled = 0;
            state.gameplay_trace_enabled = 0;
            state.console_trace_enabled = 0;
            kage_pc_keyboard_reset();
        }
    }
    state.mode = mode;
    return 1;
}

int kage_pc_backend_mode(void)
{
    return state.mode;
}

int kage_pc_backend_initialize(uint32_t width, uint32_t height)
{
    WNDCLASSEXA window_class;
    PIXELFORMATDESCRIPTOR request;
    PIXELFORMATDESCRIPTOR chosen;
    const unsigned char *version;
    DWORD style;
    int pixel_format;
    char capture_reason[192];

    if (state.mode == KAGE_PC_BACKEND_DISABLED) {
        set_error_text("KAGE PC backend: explicit mode is disabled");
        return 0;
    }
    if (!width || !height || width > KAGE_MAX_DIMENSION ||
        height > KAGE_MAX_DIMENSION) {
        set_error_text("KAGE PC backend: invalid framebuffer dimensions");
        return 0;
    }
    if (state.ready) {
        if (state.width == width && state.height == height)
            return 1;
        set_error_text("KAGE PC backend: framebuffer size changed while active");
        return 0;
    }

    if (!kage_pc_virtual_clipboard_release())
        return 0;
    state.error[0] = '\0';
    state.gl_version[0] = '\0';
    state.probe_rgba[0] = state.probe_rgba[1] = 0;
    state.probe_rgba[2] = state.probe_rgba[3] = 0;
    state.clear_count = 0;
    state.present_count = 0;
    memset(state.stage_counts, 0, sizeof state.stage_counts);
    memset(state.menu_init_probe_counts, 0,
           sizeof state.menu_init_probe_counts);
    state.menu_init_probe_armed = 0;
    state.menu_init_probe_completed = 0;
    state.menu_render_probe_armed = 0;
    state.menu_render_probe_completed = 0;
    memset(state.menu_character_probe_counts, 0,
           sizeof state.menu_character_probe_counts);
    state.menu_character_wheel_call_count = 0u;
    state.menu_character_wheel_fixed_iterations = 0u;
    state.menu_character_wheel_dynamic_iterations = 0u;
    state.menu_character_probe_armed = 0;
    state.menu_character_probe_completed = 0;
    memset(state.gl_trace_counts, 0, sizeof state.gl_trace_counts);
    state.vsync_change_count = 0;
    state.capture_requested = capture_environment_requested();
    state.capture_target_present = 1u;
    state.capture_arm_attempted = 0;
    state.capture_armed = 0;
    state.capture_present_attempted = 0;
    state.capture_finished = 0;
    state.close_requested = 0;
    state.input_trace_enabled = 0;
    state.gameplay_trace_enabled = 0;
    state.console_trace_enabled = 0;
    state.console_command_sequence = 0u;
    kage_pc_keyboard_reset();
    state.accelerated = 0;
    state.width = width;
    state.height = height;
    state.instance = GetModuleHandleA(NULL);

    if (state.capture_requested) {
        capture_remove_artifacts();
        if (!capture_path_is_absent(KAGE_PC_CAPTURE_PPM_TEMP,
                                    capture_reason,
                                    sizeof capture_reason) ||
            !capture_path_is_absent(KAGE_PC_CAPTURE_PPM,
                                    capture_reason,
                                    sizeof capture_reason)) {
            capture_fail(capture_reason);
        } else if (!capture_target_from_environment(
                       &state.capture_target_present,
                       capture_reason, sizeof capture_reason)) {
            capture_fail(capture_reason);
        } else {
            fprintf(stderr,
                    "PC KAGE capture requested : game Present %u\n",
                    state.capture_target_present);
        }
    }

    /* GLFW establishes its timer origin during glfwInit.  The explicit PC
     * boundary replaces that lifecycle, so establish one checked origin here
     * before native graphics setup can consume appreciable wall time. */
    if (!initialize_clock())
        return 0;
    if (!load_api()) {
        reset_clock();
        return 0;
    }
    state.input_trace_enabled =
        kage_pc_input_trace_environment_requested();
    state.gameplay_trace_enabled =
        kage_pc_gameplay_trace_environment_requested();
    state.console_trace_enabled =
        kage_pc_console_trace_environment_requested();

    memset(&window_class, 0, sizeof window_class);
    window_class.cbSize = sizeof window_class;
    window_class.style = CS_OWNDC;
    window_class.lpfnWndProc = kage_window_proc;
    window_class.hInstance = state.instance;
    window_class.lpszClassName = kage_window_class;
    if (!api.RegisterClassExA_(&window_class)) {
        set_win32_error("RegisterClassExA");
        goto fail;
    }
    state.class_owned = 1;

    style = WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
    style |= state.mode == KAGE_PC_BACKEND_VISIBLE
        ? WS_OVERLAPPEDWINDOW : WS_POPUP;
    state.window = api.CreateWindowExA_(
        0, kage_window_class, "repentogxm KAGE PC backend", style,
        CW_USEDEFAULT, CW_USEDEFAULT, (int)width, (int)height,
        NULL, NULL, state.instance, NULL);
    if (!state.window) {
        set_win32_error("CreateWindowExA");
        goto fail;
    }
    state.dc = api.GetDC_(state.window);
    if (!state.dc) {
        set_win32_error("GetDC");
        goto fail;
    }

    memset(&request, 0, sizeof request);
    request.nSize = sizeof request;
    request.nVersion = 1;
    request.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL |
                      PFD_DOUBLEBUFFER;
    request.iPixelType = PFD_TYPE_RGBA;
    request.cColorBits = 24;
    request.cAlphaBits = 8;
    request.cDepthBits = 24;
    request.cStencilBits = 8;
    request.iLayerType = PFD_MAIN_PLANE;
    pixel_format = api.ChoosePixelFormat_(state.dc, &request);
    if (!pixel_format) {
        set_win32_error("ChoosePixelFormat");
        goto fail;
    }
    memset(&chosen, 0, sizeof chosen);
    if (!api.DescribePixelFormat_(state.dc, pixel_format, sizeof chosen,
                                  &chosen)) {
        set_win32_error("DescribePixelFormat");
        goto fail;
    }
    if (!(chosen.dwFlags & PFD_DRAW_TO_WINDOW) ||
        !(chosen.dwFlags & PFD_SUPPORT_OPENGL) ||
        !(chosen.dwFlags & PFD_DOUBLEBUFFER) ||
        chosen.iPixelType != PFD_TYPE_RGBA) {
        set_error_text("KAGE PC backend: chosen pixel format lacks RGBA double buffering");
        goto fail;
    }
    state.accelerated = !(chosen.dwFlags & PFD_GENERIC_FORMAT) ||
                        !!(chosen.dwFlags & PFD_GENERIC_ACCELERATED);
    if (!api.SetPixelFormat_(state.dc, pixel_format, &chosen)) {
        set_win32_error("SetPixelFormat");
        goto fail;
    }

    state.context = api.wglCreateContext_(state.dc);
    if (!state.context) {
        set_win32_error("wglCreateContext");
        goto fail;
    }
    if (!api.wglMakeCurrent_(state.dc, state.context)) {
        set_win32_error("wglMakeCurrent");
        goto fail;
    }

    api.wglSwapIntervalEXT_ = (kage_wgl_swap_interval_fn)
        api.wglGetProcAddress_("wglSwapIntervalEXT");
    if (!valid_wgl_extension((PROC)api.wglSwapIntervalEXT_))
        api.wglSwapIntervalEXT_ = NULL;
    state.vsync_enabled = 0;
    if (api.wglSwapIntervalEXT_ && !api.wglSwapIntervalEXT_(0)) {
        set_error_text("KAGE PC backend: wglSwapIntervalEXT(0) failed");
        goto fail;
    }

    version = api.glGetString_(KAGE_GL_VERSION);
    if (!version || !version[0]) {
        set_error_text("KAGE PC backend: current context has no GL_VERSION");
        goto fail;
    }
    _snprintf_s(state.gl_version, sizeof state.gl_version, _TRUNCATE,
                "%s", (const char *)version);
    if (!prove_clear_and_present())
        goto fail;

    if (state.mode == KAGE_PC_BACKEND_VISIBLE) {
        api.ShowWindow_(state.window, SW_SHOW);
        api.UpdateWindow_(state.window);
    }
    state.ready = 1;
    return 1;

fail:
    release_resources();
    reset_clock();
    return 0;
}

void kage_pc_backend_shutdown(void)
{
    int mode = state.mode;
    release_resources();
    state.width = 0;
    state.height = 0;
    memset(state.stage_counts, 0, sizeof state.stage_counts);
    memset(state.menu_init_probe_counts, 0,
           sizeof state.menu_init_probe_counts);
    state.menu_init_probe_armed = 0;
    state.menu_init_probe_completed = 0;
    state.menu_render_probe_armed = 0;
    state.menu_render_probe_completed = 0;
    memset(state.menu_character_probe_counts, 0,
           sizeof state.menu_character_probe_counts);
    state.menu_character_wheel_call_count = 0u;
    state.menu_character_wheel_fixed_iterations = 0u;
    state.menu_character_wheel_dynamic_iterations = 0u;
    state.menu_character_probe_armed = 0;
    state.menu_character_probe_completed = 0;
    state.close_requested = 0;
    state.input_trace_enabled = 0;
    state.gameplay_trace_enabled = 0;
    state.console_trace_enabled = 0;
    kage_pc_keyboard_reset();
    reset_clock();
    state.mode = mode;
}

static int should_log_count(unsigned count)
{
    if (count == 0u)
        return 0;
    if (count >= 1u && count <= 4u)
        return 1;
    if (count <= 64u && (count & (count - 1u)) == 0u)
        return 1;
    return count % 120u == 0u;
}

static int should_log_stage_count(unsigned count)
{
    if (count == 0u)
        return 0;
    if (count <= 64u)
        return 1;
    return count % 120u == 0u;
}

int kage_pc_backend_present(void)
{
    MSG message;
    uint8_t *capture_pixels = NULL;
    unsigned long different_pixels = 0;
    char capture_reason[192];
    if (!state.ready || !state.context || !state.dc) {
        set_error_text("KAGE PC backend: present without a context");
        return 0;
    }
    if (!api.wglMakeCurrent_(state.dc, state.context)) {
        set_win32_error("wglMakeCurrent during present");
        return 0;
    }
    while (api.PeekMessageA_(&message, NULL, 0, 0, PM_REMOVE)) {
        api.TranslateMessage_(&message);
        api.DispatchMessageA_(&message);
    }
    if (state.close_requested) {
        set_error_text("KAGE PC backend: window close requested");
        return 0;
    }
    if (state.capture_requested && !state.capture_finished &&
        !state.capture_present_attempted &&
        state.present_count == state.capture_target_present) {
        state.capture_present_attempted = 1;
        capture_pixels = capture_read_first_present(&different_pixels);
    }
    if (!api.SwapBuffers_(state.dc)) {
        set_win32_error("SwapBuffers");
        if (capture_pixels) {
            HeapFree(GetProcessHeap(), 0, capture_pixels);
            capture_fail(state.error);
        }
        return 0;
    }
    state.present_count++;
    if (capture_pixels) {
        if (!capture_write_ppm_atomic(capture_pixels, capture_reason,
                                      sizeof capture_reason)) {
            HeapFree(GetProcessHeap(), 0, capture_pixels);
            capture_fail(capture_reason);
        } else {
            HeapFree(GetProcessHeap(), 0, capture_pixels);
            state.capture_armed = 0;
            state.capture_finished = 1;
            fprintf(stderr,
                    "PC KAGE capture complete  : Present %u, %lu/%u pixels differ; %s\n",
                    state.capture_target_present,
                    different_pixels,
                    KAGE_CAPTURE_WIDTH * KAGE_CAPTURE_HEIGHT,
                    KAGE_PC_CAPTURE_PPM);
        }
    }
    if (should_log_count(state.present_count - 1u))
        fprintf(stderr, "PC KAGE game Present calls : %u\n",
                state.present_count - 1u);
    return 1;
}

static void note_stage(unsigned stage, const char *name)
{
    unsigned game_present_count = state.present_count
        ? state.present_count - 1u : 0u;
    state.stage_counts[stage]++;
    if (should_log_stage_count(state.stage_counts[stage]))
        fprintf(stderr,
                "PC KAGE stage %s: %u (game Present calls: %u)\n",
                name, state.stage_counts[stage], game_present_count);
}

static int menu_render_probe_elapsed(double *seconds)
{
    LARGE_INTEGER now;

    if (!seconds || !state.clock_ready ||
        state.clock_frequency.QuadPart <= 0 ||
        !QueryPerformanceCounter(&now) ||
        now.QuadPart < state.clock_origin.QuadPart)
        return 0;
    *seconds = (double)(now.QuadPart - state.clock_origin.QuadPart) /
        (double)state.clock_frequency.QuadPart;
    return 1;
}

void kage_pc_backend_note_loop_head(void)
{ note_stage(KAGE_STAGE_LOOP_HEAD, "loop head"); }
void kage_pc_backend_note_service_entry(void) {}
void kage_pc_backend_note_update_entry(void)
{ note_stage(KAGE_STAGE_UPDATE_ENTRY, "Update entry"); }
void kage_pc_backend_audio_cooperative_poll(struct CPU *cpu) { (void)cpu; }
void kage_pc_backend_note_render_entry(void)
{
    kage_pc_note_gameplay_sample(state.present_count);
    if (state.capture_requested && !state.capture_finished &&
        !state.capture_arm_attempted &&
        state.present_count == state.capture_target_present)
        capture_arm_at_render_entry();
    note_stage(KAGE_STAGE_RENDER_ENTRY, "Render entry");
}
void kage_pc_backend_note_render_return(void)
{
    note_stage(KAGE_STAGE_RENDER_RETURN, "Render return");
    if (state.menu_character_probe_armed) {
        fprintf(stderr,
                "PC KAGE menu-character probe : main Render return fallback\n");
        fflush(stderr);
        state.menu_character_probe_armed = 0;
        state.menu_character_probe_completed = 1;
    }
    if (state.menu_render_probe_armed) {
        fprintf(stderr,
                "PC KAGE menu-render probe : main Render return fallback\n");
        fflush(stderr);
        state.menu_render_probe_armed = 0;
        state.menu_render_probe_completed = 1;
    }
}

void kage_pc_backend_note_limiter_entry(void) {}
void kage_pc_backend_note_limiter_exit(void) {}

void kage_pc_backend_note_menu_init(uint32_t site)
{
    unsigned index;
    unsigned count;

    if (state.menu_init_probe_completed)
        return;
    if (!state.menu_init_probe_armed) {
        if (site != kage_menu_init_probe_sites[
                        KAGE_MENU_INIT_PROBE_ARM_INDEX])
            return;
        state.menu_init_probe_armed = 1;
    }
    for (index = 0u; index < KAGE_MENU_INIT_PROBE_COUNT; ++index) {
        if (kage_menu_init_probe_sites[index] == site)
            break;
    }
    if (index == KAGE_MENU_INIT_PROBE_COUNT)
        return;
    count = ++state.menu_init_probe_counts[index];
    fprintf(stderr, "PC KAGE menu-init probe : site=%08lx count=%u\n",
            (unsigned long)site, count);
    fflush(stderr);
    if (index == KAGE_MENU_INIT_PROBE_DISARM_INDEX) {
        state.menu_init_probe_armed = 0;
        state.menu_init_probe_completed = 1;
    }
}

void kage_pc_backend_note_menu_render(uint32_t site)
{
    unsigned index;
    double elapsed;

    if (state.menu_render_probe_completed)
        return;
    if (!state.menu_render_probe_armed) {
        if (site != kage_menu_render_probes[
                        KAGE_MENU_RENDER_PROBE_ARM_INDEX].site)
            return;
        state.menu_render_probe_armed = 1;
    }
    for (index = 0u; index < KAGE_MENU_RENDER_PROBE_COUNT; ++index) {
        if (kage_menu_render_probes[index].site == site)
            break;
    }
    if (index == KAGE_MENU_RENDER_PROBE_COUNT)
        return;
    if (menu_render_probe_elapsed(&elapsed))
        fprintf(stderr,
                "PC KAGE menu-render probe : site=%08lx t=%.6f %s\n",
                (unsigned long)site, elapsed,
                kage_menu_render_probes[index].name);
    else
        fprintf(stderr,
                "PC KAGE menu-render probe : site=%08lx t=unavailable %s\n",
                (unsigned long)site, kage_menu_render_probes[index].name);
    fflush(stderr);
    if (site == 0x004e1b87u &&
        !state.menu_character_probe_completed) {
        memset(state.menu_character_probe_counts, 0,
               sizeof state.menu_character_probe_counts);
        state.menu_character_wheel_call_count = 0u;
        state.menu_character_wheel_fixed_iterations = 0u;
        state.menu_character_wheel_dynamic_iterations = 0u;
        state.menu_character_probe_armed = 1;
    } else if (site == 0x004e1b92u &&
               state.menu_character_probe_armed) {
        fprintf(stderr,
                "PC KAGE menu-character terminal : site=004e1b92 "
                "Character render returned\n");
        fflush(stderr);
        state.menu_character_probe_armed = 0;
        state.menu_character_probe_completed = 1;
    }
    if (index == KAGE_MENU_RENDER_PROBE_DISARM_INDEX) {
        state.menu_render_probe_armed = 0;
        state.menu_render_probe_completed = 1;
    }
}

static int should_log_menu_character_loop_count(unsigned count)
{
    if (count == 0u)
        return 0;
    if (count <= 4u)
        return 1;
    return (count & (count - 1u)) == 0u;
}

void kage_pc_backend_note_menu_character(uint32_t site)
{
    unsigned index;
    unsigned count;
    unsigned hit;
    uint32_t probe_site;
    int should_log;
    double elapsed;

    if (!state.menu_character_probe_armed ||
        state.menu_character_probe_completed)
        return;

    if (site == 0x004a7080u) {
        state.menu_character_wheel_call_count++;
        state.menu_character_wheel_fixed_iterations = 0u;
        state.menu_character_wheel_dynamic_iterations = 0u;
        for (index = 0u; index < KAGE_MENU_CHARACTER_PROBE_COUNT;
             ++index) {
            probe_site = kage_menu_character_probes[index].site;
            if (probe_site >= 0x004a7080u && probe_site < 0x004a7626u)
                state.menu_character_probe_counts[index] = 0u;
        }
    }

    for (index = 0u; index < KAGE_MENU_CHARACTER_PROBE_COUNT; ++index) {
        if (kage_menu_character_probes[index].site == site)
            break;
    }
    if (index == KAGE_MENU_CHARACTER_PROBE_COUNT)
        return;

    count = ++state.menu_character_probe_counts[index];
    hit = count;
    should_log = count == 1u;
    if (site == 0x004a7080u) {
        hit = state.menu_character_wheel_call_count;
        should_log = should_log_menu_character_loop_count(hit);
    } else if (site == 0x004a7110u) {
        hit = ++state.menu_character_wheel_fixed_iterations;
        should_log = should_log_menu_character_loop_count(hit);
    } else if (site == 0x004a7230u) {
        hit = ++state.menu_character_wheel_dynamic_iterations;
        should_log = should_log_menu_character_loop_count(hit);
    }
    if (!should_log)
        return;

    if (menu_render_probe_elapsed(&elapsed))
        fprintf(stderr,
                "PC KAGE menu-character probe : site=%08lx t=%.6f "
                "hit=%u wheel=%u fixed=%u dynamic=%u %s\n",
                (unsigned long)site, elapsed, hit,
                state.menu_character_wheel_call_count,
                state.menu_character_wheel_fixed_iterations,
                state.menu_character_wheel_dynamic_iterations,
                kage_menu_character_probes[index].name);
    else
        fprintf(stderr,
                "PC KAGE menu-character probe : site=%08lx t=unavailable "
                "hit=%u wheel=%u fixed=%u dynamic=%u %s\n",
                (unsigned long)site, hit,
                state.menu_character_wheel_call_count,
                state.menu_character_wheel_fixed_iterations,
                state.menu_character_wheel_dynamic_iterations,
                kage_menu_character_probes[index].name);
    fflush(stderr);
}

void kage_pc_backend_note_menu_character_wheel_snapshot(
    uint32_t this_ptr, uint32_t character_count,
    uint32_t scratch_begin, uint32_t scratch_end,
    uint32_t scratch_capacity, uint32_t guard)
{
    double elapsed;

    if (!state.menu_character_probe_armed ||
        state.menu_character_probe_completed)
        return;
    if (menu_render_probe_elapsed(&elapsed))
        fprintf(stderr,
                "PC KAGE menu-character snapshot : t=%.6f wheel=%u "
                "this=%08lx characters=%lu scratch=%08lx/%08lx/%08lx "
                "guard=%ld\n",
                elapsed, state.menu_character_wheel_call_count,
                (unsigned long)this_ptr, (unsigned long)character_count,
                (unsigned long)scratch_begin, (unsigned long)scratch_end,
                (unsigned long)scratch_capacity, (long)(int32_t)guard);
    else
        fprintf(stderr,
                "PC KAGE menu-character snapshot : t=unavailable wheel=%u "
                "this=%08lx characters=%lu scratch=%08lx/%08lx/%08lx "
                "guard=%ld\n",
                state.menu_character_wheel_call_count,
                (unsigned long)this_ptr, (unsigned long)character_count,
                (unsigned long)scratch_begin, (unsigned long)scratch_end,
                (unsigned long)scratch_capacity, (long)(int32_t)guard);
    fflush(stderr);
}

void kage_pc_backend_keyboard_snapshot(
    uint8_t keys[KAGE_PC_KEY_COUNT])
{
    unsigned down_keys = 0u;
    size_t index;

    if (!keys)
        return;
    memcpy(keys, state.keyboard, sizeof state.keyboard);
    if (!state.input_trace_enabled ||
        state.keyboard_consumed_generation == state.keyboard_generation)
        return;

    for (index = 0; index < sizeof state.keyboard; ++index)
        down_keys += state.keyboard[index] != 0u;
    state.keyboard_consumed_generation = state.keyboard_generation;
    fprintf(stderr,
            "PC KAGE input snapshot : generation=%u down_keys=%u\n",
            state.keyboard_consumed_generation, down_keys);
    fflush(stderr);
}

void kage_pc_backend_note_guest_gl_call(const char *name)
{
    unsigned int index;
    unsigned int trace_index;
    unsigned int error;
    unsigned int game_present_count;
    int current_program = -1;
    int draw_framebuffer = -1;
    int renderbuffer = -1;
    unsigned int query_error;
    int saw_error = 0;

    if (!state.capture_requested || state.capture_finished ||
        !state.ready || !api.glGetError_)
        return;
    if (name) {
        for (trace_index = 0u; trace_index < KAGE_GL_TRACE_COUNT;
             ++trace_index) {
            if (strcmp(name, kage_gl_trace_names[trace_index]) == 0) {
                state.gl_trace_counts[trace_index]++;
                break;
            }
        }
    }
    game_present_count = state.present_count ? state.present_count - 1u : 0u;
    for (index = 0u; index < KAGE_CAPTURE_STALE_ERROR_LIMIT; ++index) {
        error = api.glGetError_();
        if (!error)
            break;
        saw_error = 1;
        fprintf(stderr,
                "PC KAGE guest GL error : call=%s index=%u error=0x%x render=%u game_present=%u\n",
                name ? name : "?", index, error,
                state.stage_counts[KAGE_STAGE_RENDER_ENTRY],
                game_present_count);
    }
    if (!saw_error)
        return;
    if (index == KAGE_CAPTURE_STALE_ERROR_LIMIT) {
        fprintf(stderr,
                "PC KAGE guest GL error : call=%s remained nonzero after %u reads\n",
                name ? name : "?", KAGE_CAPTURE_STALE_ERROR_LIMIT);
        return;
    }
    api.glGetIntegerv_(KAGE_GL_CURRENT_PROGRAM, &current_program);
    api.glGetIntegerv_(KAGE_GL_DRAW_FRAMEBUFFER_BINDING, &draw_framebuffer);
    api.glGetIntegerv_(KAGE_GL_RENDERBUFFER_BINDING, &renderbuffer);
    query_error = api.glGetError_();
    fprintf(stderr,
            "PC KAGE guest GL state : call=%s program=%d draw_fbo=%d renderbuffer=%d query_error=0x%x\n",
            name ? name : "?", current_program, draw_framebuffer,
            renderbuffer, query_error);
}

int kage_pc_backend_time_seconds(double *seconds)
{
    LARGE_INTEGER now;
    LONGLONG elapsed;

    if (!seconds) {
        set_error_text("KAGE PC backend: null clock output");
        return 0;
    }
    if (state.mode == KAGE_PC_BACKEND_DISABLED || !state.clock_ready) {
        set_error_text("KAGE PC backend: clock requested before initialize");
        return 0;
    }
    if (!QueryPerformanceCounter(&now)) {
        set_win32_error("QueryPerformanceCounter sample");
        return 0;
    }
    if (now.QuadPart < state.clock_origin.QuadPart) {
        set_error_text("KAGE PC backend: QPC sample precedes origin");
        return 0;
    }
    elapsed = now.QuadPart - state.clock_origin.QuadPart;
    if (elapsed < state.clock_last) {
        set_error_text("KAGE PC backend: QPC moved backwards");
        return 0;
    }
    state.clock_last = elapsed;
    *seconds = (double)elapsed / (double)state.clock_frequency.QuadPart;
    return 1;
}

int kage_pc_backend_set_vsync(int enabled)
{
    enabled = !!enabled;
    if (!state.ready || !state.context || !state.dc) {
        set_error_text("KAGE PC backend: VSync requested before initialize");
        return 0;
    }
    if (!api.wglMakeCurrent_(state.dc, state.context)) {
        set_win32_error("wglMakeCurrent during VSync change");
        return 0;
    }
    if (!api.wglSwapIntervalEXT_) {
        set_error_text("KAGE PC backend: WGL_EXT_swap_control unavailable");
        return 0;
    }
    if (!api.wglSwapIntervalEXT_(enabled ? 1 : 0)) {
        set_error_text("KAGE PC backend: wglSwapIntervalEXT failed");
        return 0;
    }
    state.vsync_enabled = enabled;
    state.vsync_change_count++;
    return 1;
}

int kage_pc_backend_vsync_enabled(void) { return state.vsync_enabled; }

uint32_t kage_pc_backend_refresh_rate(void)
{
    DEVMODEA mode;
    if (!state.ready || !state.window || !api.EnumDisplaySettingsA_) {
        set_error_text("KAGE PC backend: refresh rate requested before initialize");
        return 0;
    }
    memset(&mode, 0, sizeof mode);
    mode.dmSize = (WORD)sizeof mode;
    if (!api.EnumDisplaySettingsA_(NULL, ENUM_CURRENT_SETTINGS, &mode)) {
        set_win32_error("EnumDisplaySettingsA current mode");
        return 0;
    }
    if (mode.dmDisplayFrequency <= 1u) {
        _snprintf_s(state.error, sizeof state.error, _TRUNCATE,
                    "KAGE PC backend: invalid display refresh %lu Hz",
                    (unsigned long)mode.dmDisplayFrequency);
        return 0;
    }
    return (uint32_t)mode.dmDisplayFrequency;
}

unsigned kage_pc_backend_vsync_change_count(void)
{
    return state.vsync_change_count;
}

int kage_pc_backend_ready(void) { return state.ready; }
uint32_t kage_pc_backend_width(void) { return state.width; }
uint32_t kage_pc_backend_height(void) { return state.height; }
const char *kage_pc_backend_last_error(void)
{
    return state.error[0] ? state.error : "KAGE PC backend: no error";
}
const char *kage_pc_backend_gl_version(void) { return state.gl_version; }
unsigned kage_pc_backend_clear_count(void) { return state.clear_count; }
unsigned kage_pc_backend_present_count(void) { return state.present_count; }
int kage_pc_backend_accelerated(void) { return state.accelerated; }
void kage_pc_backend_probe_rgba(uint8_t rgba[4])
{
    if (rgba)
        memcpy(rgba, state.probe_rgba, sizeof state.probe_rgba);
}

int kage_pc_backend_console_trace_enabled(void)
{
    return state.console_trace_enabled;
}

static int kage_pc_virtual_clipboard_same_thread(void)
{
    return state.command_clipboard_thread != 0u &&
           state.command_clipboard_thread == GetCurrentThreadId();
}

int kage_pc_backend_virtual_clipboard_open(uintptr_t owner, uint32_t *result)
{
    if (!result)
        return 0;
    /* The explicit no-GLFW getter always passes HWND=NULL.  Once its trace
     * gate is enabled, even an unarmed/stale Ctrl+V is private: return FALSE
     * instead of exposing the user's real clipboard.  Non-null native owners
     * retain byte-for-byte native fallthrough. */
    if (!state.console_trace_enabled || owner != (uintptr_t)0)
        return 0;
    *result = 0u;
    if (!state.ready || !state.command_pending || state.command_open ||
        !state.command_clipboard || !kage_pc_virtual_clipboard_same_thread())
        return 1;
    state.command_open = 1;
    state.command_data_issued = 0;
    state.command_lock_seen = 0;
    state.command_unlock_seen = 0;
    *result = 1u;
    return 1;
}

int kage_pc_backend_virtual_clipboard_get_data(uint32_t format,
                                               uintptr_t *result)
{
    if (!result || !state.command_open ||
        !kage_pc_virtual_clipboard_same_thread())
        return 0;
    /* Never query USER32 while a virtual OpenClipboard transaction is live.
     * The original getter asks only for CF_TEXT; another format is an exact
     * handled miss and cannot see system clipboard data. */
    *result = (uintptr_t)0;
    if (format == CF_TEXT && state.command_pending &&
        state.command_clipboard) {
        state.command_data_issued = 1;
        *result = (uintptr_t)state.command_clipboard;
    }
    return 1;
}

int kage_pc_backend_virtual_global_lock(uintptr_t handle, uintptr_t *result)
{
    void *pointer;

    if (!result || !state.command_open || !state.command_data_issued ||
        !kage_pc_virtual_clipboard_same_thread() ||
        handle != (uintptr_t)state.command_clipboard)
        return 0;
    pointer = GlobalLock(state.command_clipboard);
    *result = (uintptr_t)pointer;
    if (pointer)
        state.command_lock_seen = 1;
    return 1;
}

int kage_pc_backend_virtual_global_unlock(uintptr_t handle, uint32_t *result)
{
    BOOL unlocked;

    if (!result || !state.command_open || !state.command_data_issued ||
        !state.command_lock_seen || !kage_pc_virtual_clipboard_same_thread() ||
        handle != (uintptr_t)state.command_clipboard)
        return 0;
    SetLastError(ERROR_SUCCESS);
    unlocked = GlobalUnlock(state.command_clipboard);
    *result = (uint32_t)unlocked;
    if (unlocked || GetLastError() == ERROR_SUCCESS)
        state.command_unlock_seen = 1;
    return 1;
}

int kage_pc_backend_virtual_clipboard_close(uint32_t *result)
{
    const int consumed = state.command_data_issued &&
                         state.command_lock_seen &&
                         state.command_unlock_seen;

    if (!result || !state.command_open ||
        !kage_pc_virtual_clipboard_same_thread())
        return 0;
    *result = 1u;
    state.command_open = 0;
    state.command_data_issued = 0;
    state.command_lock_seen = 0;
    state.command_unlock_seen = 0;
    if (consumed)
        state.command_pending = 0;
    return 1;
}

int kage_pc_backend_note_console_command(uint32_t guest_stack)
{
    static const char hex_digits[] = "0123456789abcdef";
    uint8_t command[KAGE_PC_CONSOLE_COMMAND_MAX];
    char hex[KAGE_PC_CONSOLE_COMMAND_MAX * 2u + 1u];
    uint32_t argument_address;
    uint32_t string_address;
    uint32_t size_address;
    uint32_t capacity_address;
    uint32_t data_address;
    uint32_t size;
    uint32_t capacity;
    SIZE_T copied = 0u;
    uint32_t index;
    unsigned sequence;

    /* The translated entry hook is unconditional for explicit PC-KAGE, but
     * ordinary runs must retain their old logging and cost profile. */
    if (!state.console_trace_enabled)
        return 1;
    if (!state.ready) {
        set_error_text("KAGE PC console trace before backend ready");
        return 0;
    }
    if (!kage_pc_add_guest_offset(guest_stack, 4u, &argument_address) ||
        !kage_pc_read_guest_u32(argument_address, &string_address) ||
        !string_address ||
        !kage_pc_add_guest_offset(string_address, 0x10u, &size_address) ||
        !kage_pc_add_guest_offset(string_address, 0x14u,
                                  &capacity_address) ||
        !kage_pc_read_guest_u32(size_address, &size) ||
        !kage_pc_read_guest_u32(capacity_address, &capacity)) {
        set_error_text("KAGE PC console trace could not read std::string");
        return 0;
    }
    if (!size || size > KAGE_PC_CONSOLE_COMMAND_MAX || size > capacity) {
        set_error_text("KAGE PC console trace rejected std::string bounds");
        return 0;
    }
    if (capacity < 16u) {
        data_address = string_address;
    } else if (!kage_pc_read_guest_u32(string_address, &data_address) ||
               !data_address) {
        set_error_text("KAGE PC console trace could not read string storage");
        return 0;
    }
    if (data_address > UINT32_MAX - (size - 1u) ||
        !ReadProcessMemory(GetCurrentProcess(),
                           (const void *)(uintptr_t)data_address,
                           command, (SIZE_T)size, &copied) ||
        copied != (SIZE_T)size) {
        set_error_text("KAGE PC console trace could not read command bytes");
        return 0;
    }
    for (index = 0u; index < size; ++index) {
        if (command[index] < 0x20u || command[index] > 0x7eu) {
            set_error_text("KAGE PC console trace rejected non-printable byte");
            return 0;
        }
        hex[index * 2u] = hex_digits[command[index] >> 4];
        hex[index * 2u + 1u] = hex_digits[command[index] & 0x0fu];
    }
    hex[size * 2u] = '\0';
    if (state.console_command_sequence == UINT_MAX) {
        set_error_text("KAGE PC console trace sequence overflow");
        return 0;
    }
    sequence = ++state.console_command_sequence;
    if (fprintf(stderr,
                "PC KAGE console command : sequence=%u length=%lu hex=%s\n",
                sequence, (unsigned long)size, hex) < 0 ||
        fflush(stderr) != 0) {
        set_error_text("KAGE PC console trace write failed");
        return 0;
    }
    return 1;
}

uintptr_t kage_pc_backend_gl_proc(const char *name)
{
    PROC extension;
    FARPROC core;

    if (!state.ready || !state.context || !state.dc ||
        !api.wglGetCurrentContext_ || !api.wglMakeCurrent_ ||
        !api.wglGetProcAddress_ ||
        !api.opengl32 || !name || !name[0])
        return (uintptr_t)0;
    if (api.wglGetCurrentContext_() != state.context &&
        !api.wglMakeCurrent_(state.dc, state.context)) {
        set_win32_error("wglMakeCurrent during GL resolve");
        return (uintptr_t)0;
    }

    /* Microsoft documents that 1.1/core exports live in opengl32.dll while
     * extensions come from wglGetProcAddress.  Some ICDs return the sentinel
     * values 1, 2, 3, or -1 for a miss; never turn those into callable code. */
    extension = api.wglGetProcAddress_(name);
    if (valid_wgl_extension(extension))
        return (uintptr_t)extension;
    core = GetProcAddress(api.opengl32, name);
    return core ? (uintptr_t)core : (uintptr_t)0;
}

#else

static int mode;
static const char unsupported[] =
    "KAGE PC backend: WGL is unavailable on this target";

int kage_pc_backend_set_mode(int requested)
{
    if (requested < KAGE_PC_BACKEND_DISABLED ||
        requested > KAGE_PC_BACKEND_VISIBLE)
        return 0;
    mode = requested;
    return 1;
}
int kage_pc_backend_mode(void) { return mode; }
int kage_pc_backend_initialize(uint32_t width, uint32_t height)
{ (void)width; (void)height; return 0; }
void kage_pc_backend_shutdown(void) {}
int kage_pc_backend_present(void) { return 0; }
void kage_pc_backend_note_loop_head(void) {}
void kage_pc_backend_note_service_entry(void) {}
void kage_pc_backend_note_update_entry(void) {}
void kage_pc_backend_audio_cooperative_poll(struct CPU *cpu) { (void)cpu; }
void kage_pc_backend_note_render_entry(void) {}
void kage_pc_backend_note_render_return(void) {}
void kage_pc_backend_note_limiter_entry(void) {}
void kage_pc_backend_note_limiter_exit(void) {}
void kage_pc_backend_note_menu_init(uint32_t site) { (void)site; }
void kage_pc_backend_note_menu_render(uint32_t site) { (void)site; }
void kage_pc_backend_note_menu_character(uint32_t site) { (void)site; }
void kage_pc_backend_note_menu_character_wheel_snapshot(
    uint32_t this_ptr, uint32_t character_count,
    uint32_t scratch_begin, uint32_t scratch_end,
    uint32_t scratch_capacity, uint32_t guard)
{
    (void)this_ptr; (void)character_count;
    (void)scratch_begin; (void)scratch_end; (void)scratch_capacity;
    (void)guard;
}
void kage_pc_backend_keyboard_snapshot(
    uint8_t keys[KAGE_PC_KEY_COUNT])
{
    if (keys)
        memset(keys, 0, KAGE_PC_KEY_COUNT);
}
int kage_pc_backend_console_trace_enabled(void) { return 0; }
int kage_pc_backend_note_console_command(uint32_t guest_stack)
{ (void)guest_stack; return 1; }
int kage_pc_backend_virtual_clipboard_open(
    uintptr_t owner, uint32_t *result)
{ (void)owner; (void)result; return 0; }
int kage_pc_backend_virtual_clipboard_get_data(
    uint32_t format, uintptr_t *result)
{ (void)format; (void)result; return 0; }
int kage_pc_backend_virtual_global_lock(
    uintptr_t handle, uintptr_t *result)
{ (void)handle; (void)result; return 0; }
int kage_pc_backend_virtual_global_unlock(
    uintptr_t handle, uint32_t *result)
{ (void)handle; (void)result; return 0; }
int kage_pc_backend_virtual_clipboard_close(uint32_t *result)
{ (void)result; return 0; }
void kage_pc_backend_note_guest_gl_call(const char *name) { (void)name; }
int kage_pc_backend_time_seconds(double *seconds)
{ (void)seconds; return 0; }
int kage_pc_backend_set_vsync(int enabled) { (void)enabled; return 0; }
int kage_pc_backend_vsync_enabled(void) { return 0; }
uint32_t kage_pc_backend_refresh_rate(void) { return 0; }
unsigned kage_pc_backend_vsync_change_count(void) { return 0; }
int kage_pc_backend_ready(void) { return 0; }
uint32_t kage_pc_backend_width(void) { return 0; }
uint32_t kage_pc_backend_height(void) { return 0; }
const char *kage_pc_backend_last_error(void) { return unsupported; }
const char *kage_pc_backend_gl_version(void) { return ""; }
unsigned kage_pc_backend_clear_count(void) { return 0; }
unsigned kage_pc_backend_present_count(void) { return 0; }
int kage_pc_backend_accelerated(void) { return 0; }
void kage_pc_backend_probe_rgba(uint8_t rgba[4])
{ if (rgba) memset(rgba, 0, 4); }
uintptr_t kage_pc_backend_gl_proc(const char *name)
{ (void)name; return (uintptr_t)0; }

#endif
