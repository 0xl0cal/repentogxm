#ifndef ISAAC_HOST_VITA_XINPUT_H
#define ISAAC_HOST_VITA_XINPUT_H

#include <stddef.h>
#include <stdint.h>

#include "guest.h"
#include "kage_vita_input.h"

/* Frozen input executable.  Every RVA below was censused against this exact
 * unpacked PE, not inferred from a Windows SDK or a neighbouring build. */
#define ISAAC_VITA_XINPUT_PE_SIZE UINT32_C(8650240)
#define ISAAC_VITA_XINPUT_PE_SHA256 \
    "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"

#define ISAAC_VITA_XINPUT_INIT_ROOT_RVA          0x00569830U
#define ISAAC_VITA_XINPUT_INIT_BYPASS_SITE_RVA   0x005698d3U
#define ISAAC_VITA_XINPUT_INIT_BYPASS_TARGET_RVA 0x00569a03U
#define ISAAC_VITA_XINPUT_INITIALIZED_RVA        0x00803040U
#define ISAAC_VITA_XINPUT_AVAILABLE_RVA          0x008007dfU
#define ISAAC_VITA_DINPUT_CHANGED_RVA            0x008007deU

#define ISAAC_VITA_XINPUT_LOAD_LIBRARY_NAME \
    "KERNEL32.dll!LoadLibraryA"
#define ISAAC_VITA_XINPUT_LOAD_LIBRARY_IAT_RVA   0x00606124U
#define ISAAC_VITA_XINPUT_14_NAME                 "XInput1_4.dll"
#define ISAAC_VITA_XINPUT_14_NAME_RVA             0x00765334U
#define ISAAC_VITA_XINPUT_14_CALL_RVA             0x005698dfU
#define ISAAC_VITA_XINPUT_14_RETURN_RVA           0x005698e6U
#define ISAAC_VITA_XINPUT_13_NAME                 "XInput1_3.dll"
#define ISAAC_VITA_XINPUT_13_NAME_RVA             0x00765268U
#define ISAAC_VITA_XINPUT_13_CALL_RVA             0x005698ecU
#define ISAAC_VITA_XINPUT_13_RETURN_RVA           0x005698f3U
#define ISAAC_VITA_XINPUT_13_BIN_NAME             "bin\\XInput1_3.dll"
#define ISAAC_VITA_XINPUT_13_BIN_NAME_RVA         0x00765278U
#define ISAAC_VITA_XINPUT_13_BIN_CALL_RVA         0x005698f9U
#define ISAAC_VITA_XINPUT_13_BIN_RETURN_RVA       0x00569900U
#define ISAAC_VITA_XINPUT_MODULE_ALIAS_COUNT      3U

#define ISAAC_VITA_XINPUT_GET_PROC_NAME \
    "KERNEL32.dll!GetProcAddress"
#define ISAAC_VITA_XINPUT_GET_PROC_IAT_RVA        0x00606118U
#define ISAAC_VITA_XINPUT_GET_STATE_EX_ORDINAL    100U
#define ISAAC_VITA_XINPUT_GET_STATE_EX_CALL_RVA   0x00569934U
#define ISAAC_VITA_XINPUT_GET_STATE_EX_RETURN_RVA 0x0056993aU
#define ISAAC_VITA_XINPUT_GET_STATE_EX_CACHE_RVA  0x00803028U
#define ISAAC_VITA_XINPUT_GET_STATE_NAME          "XInputGetState"
#define ISAAC_VITA_XINPUT_GET_STATE_NAME_RVA      0x007652e8U
#define ISAAC_VITA_XINPUT_GET_STATE_CALL_RVA      0x00569945U
#define ISAAC_VITA_XINPUT_GET_STATE_RETURN_RVA    0x0056994bU
#define ISAAC_VITA_XINPUT_GET_STATE_CACHE_RVA     0x00803020U
#define ISAAC_VITA_XINPUT_GET_CAPS_NAME           "XInputGetCapabilities"
#define ISAAC_VITA_XINPUT_GET_CAPS_NAME_RVA       0x007651f8U
#define ISAAC_VITA_XINPUT_GET_CAPS_CALL_RVA       0x00569956U
#define ISAAC_VITA_XINPUT_GET_CAPS_RETURN_RVA     0x0056995cU
#define ISAAC_VITA_XINPUT_GET_CAPS_CACHE_RVA      0x00803018U
#define ISAAC_VITA_XINPUT_RESOLVED_PROC_COUNT     3U

#define ISAAC_VITA_XINPUT_CAPS_ROOT_RVA           0x0059d3b0U
#define ISAAC_VITA_XINPUT_CAPS_CALL_RVA           0x0059d4a8U
#define ISAAC_VITA_XINPUT_CAPS_RETURN_RVA         0x0059d4aeU
#define ISAAC_VITA_XINPUT_DETECT_WORKER_SEAM_RVA  0x005699edU
#define ISAAC_VITA_XINPUT_DETECT_WORKER_CALL_RVA  0x00569a00U
#define ISAAC_VITA_XINPUT_DETECT_WORKER_NEXT_RVA  0x00569a03U
#define ISAAC_VITA_XINPUT_DETECT_WORKER_RVA       0x00569e00U
#define ISAAC_VITA_XINPUT_DETECT_WORKER_RELOC_RVA 0x005699fcU
#define ISAAC_VITA_KAGE_THREAD_START_ROOT_RVA     0x00598a50U
#define ISAAC_VITA_BEGINTHREADEX_IAT_RVA          0x006065b0U
#define ISAAC_VITA_BEGINTHREADEX_RETURN_RVA       0x00598b5cU
#define ISAAC_VITA_XINPUT_POLL_ROOT_RVA           0x0059d720U
#define ISAAC_VITA_XINPUT_STATE_EX_CALL_RVA       0x0059d7aeU
#define ISAAC_VITA_XINPUT_STATE_EX_RETURN_RVA     0x0059d7b0U
#define ISAAC_VITA_XINPUT_STATE_CALL_RVA          0x0059d829U
#define ISAAC_VITA_XINPUT_STATE_RETURN_RVA        0x0059d82fU

/* Exact default DeviceController binding table and the six relevant rows.
 * The keyboard table independently pins action 8/9/10/11/12/13 to bomb,
 * active, pill/card, drop, pause and map.  These are guest binding IDs after
 * KAGE's libstem normalization, not raw XInput bit positions. */
#define ISAAC_VITA_XINPUT_BINDINGS_TABLE_RVA      0x007aaa20U
#define ISAAC_VITA_XINPUT_BINDINGS_TABLE_SIZE     0x00000120U
#define ISAAC_VITA_XINPUT_ACTION_BOMB             8U
#define ISAAC_VITA_XINPUT_ACTION_ACTIVE           9U
#define ISAAC_VITA_XINPUT_ACTION_PILL_CARD        10U
#define ISAAC_VITA_XINPUT_ACTION_DROP             11U
#define ISAAC_VITA_XINPUT_ACTION_PAUSE            12U
#define ISAAC_VITA_XINPUT_ACTION_MAP              13U
#define ISAAC_VITA_XINPUT_BINDING_LEFT_SHOULDER   8U
#define ISAAC_VITA_XINPUT_BINDING_LEFT_TRIGGER    9U
#define ISAAC_VITA_XINPUT_BINDING_RIGHT_SHOULDER  11U
#define ISAAC_VITA_XINPUT_BINDING_RIGHT_TRIGGER   12U
#define ISAAC_VITA_XINPUT_BINDING_BACK            14U
#define ISAAC_VITA_XINPUT_BINDING_START           15U

/* Synthetic handles never overlap the relocated PE, generated 0x7e...... GL
 * calls, or the established 0x7f000001..3 module family. */
#define ISAAC_VITA_XINPUT_MODULE_TOKEN \
    UINT32_C(0x7f000004)
#define ISAAC_VITA_XINPUT_GET_STATE_EX_TOKEN \
    UINT32_C(0x7d200001)
#define ISAAC_VITA_XINPUT_GET_STATE_TOKEN \
    UINT32_C(0x7d200002)
#define ISAAC_VITA_XINPUT_GET_CAPS_TOKEN \
    UINT32_C(0x7d200003)

#define ISAAC_VITA_XINPUT_FREE_LIBRARY_NAME \
    "KERNEL32.dll!FreeLibrary"
#define ISAAC_VITA_XINPUT_FREE_LIBRARY_IAT_RVA    0x00606104U
#define ISAAC_VITA_XINPUT_PROCEDURE_NAME_MAX      31U
#define ISAAC_VITA_XINPUT_SHARED_IMPORT_COUNT     3U

enum isaac_vita_xinput_status {
    ISAAC_VITA_XINPUT_ERROR_SUCCESS = 0U,
    ISAAC_VITA_XINPUT_ERROR_BAD_ARGUMENTS = 160U,
    ISAAC_VITA_XINPUT_ERROR_DEVICE_NOT_CONNECTED = 1167U
};

enum isaac_vita_xinput_button {
    ISAAC_VITA_XINPUT_DPAD_UP        = 0x0001U,
    ISAAC_VITA_XINPUT_DPAD_DOWN      = 0x0002U,
    ISAAC_VITA_XINPUT_DPAD_LEFT      = 0x0004U,
    ISAAC_VITA_XINPUT_DPAD_RIGHT     = 0x0008U,
    ISAAC_VITA_XINPUT_START          = 0x0010U,
    ISAAC_VITA_XINPUT_BACK           = 0x0020U,
    ISAAC_VITA_XINPUT_LEFT_THUMB     = 0x0040U,
    ISAAC_VITA_XINPUT_RIGHT_THUMB    = 0x0080U,
    ISAAC_VITA_XINPUT_LEFT_SHOULDER  = 0x0100U,
    ISAAC_VITA_XINPUT_RIGHT_SHOULDER = 0x0200U,
    ISAAC_VITA_XINPUT_GUIDE          = 0x0400U,
    ISAAC_VITA_XINPUT_A              = 0x1000U,
    ISAAC_VITA_XINPUT_B              = 0x2000U,
    ISAAC_VITA_XINPUT_X              = 0x4000U,
    ISAAC_VITA_XINPUT_Y              = 0x8000U
};

#define ISAAC_VITA_XINPUT_DEVTYPE_GAMEPAD        0x01U
#define ISAAC_VITA_XINPUT_DEVSUBTYPE_GAMEPAD     0x01U
#define ISAAC_VITA_XINPUT_FLAG_GAMEPAD           0x00000001U

typedef struct isaac_vita_xinput_gamepad {
    uint16_t wButtons;
    uint8_t bLeftTrigger;
    uint8_t bRightTrigger;
    int16_t sThumbLX;
    int16_t sThumbLY;
    int16_t sThumbRX;
    int16_t sThumbRY;
} isaac_vita_xinput_gamepad;

typedef struct isaac_vita_xinput_state {
    uint32_t dwPacketNumber;
    isaac_vita_xinput_gamepad Gamepad;
} isaac_vita_xinput_state;

/* Undocumented ordinal 100 layout copied from the exact libstem source used
 * by this PE.  Its caller copies only bytes 4..15, but the local is 20 bytes
 * and the reserved DWORD is always written deterministically. */
typedef struct isaac_vita_xinput_state_ex {
    uint32_t dwPacketNumber;
    isaac_vita_xinput_gamepad Gamepad;
    uint32_t dwPaddingReserved;
} isaac_vita_xinput_state_ex;

typedef struct isaac_vita_xinput_vibration {
    uint16_t wLeftMotorSpeed;
    uint16_t wRightMotorSpeed;
} isaac_vita_xinput_vibration;

typedef struct isaac_vita_xinput_capabilities {
    uint8_t Type;
    uint8_t SubType;
    uint16_t Flags;
    isaac_vita_xinput_gamepad Gamepad;
    isaac_vita_xinput_vibration Vibration;
} isaac_vita_xinput_capabilities;

_Static_assert(sizeof(isaac_vita_xinput_gamepad) == 12U,
               "XINPUT_GAMEPAD must be 12 bytes");
_Static_assert(sizeof(isaac_vita_xinput_state) == 16U,
               "XINPUT_STATE must be 16 bytes");
_Static_assert(sizeof(isaac_vita_xinput_state_ex) == 20U,
               "libstem XINPUT_STATE_EX must be 20 bytes");
_Static_assert(sizeof(isaac_vita_xinput_capabilities) == 20U,
               "XINPUT_CAPABILITIES must be 20 bytes");
_Static_assert(offsetof(isaac_vita_xinput_state, Gamepad) == 4U &&
               offsetof(isaac_vita_xinput_capabilities, Gamepad) == 4U &&
               offsetof(isaac_vita_xinput_capabilities, Vibration) == 16U,
               "XInput structure field offsets changed");

/* Pure policy helpers make module/name resolution and the complete mapping
 * executable on a host without fabricating 32-bit guest pointers. */
uint32_t isaac_vita_xinput_module_for_name(const char *name);
uint32_t isaac_vita_xinput_token_for_name(const char *name);
uint32_t isaac_vita_xinput_token_for_ordinal(uint32_t ordinal);
int16_t isaac_vita_xinput_axis(uint8_t value, int inverted);
void isaac_vita_xinput_build_state(
         const kage_vita_gamepad_snapshot *snapshot,
         isaac_vita_xinput_state *state);
void isaac_vita_xinput_build_capabilities(
         isaac_vita_xinput_capabilities *capabilities);

/* Common KERNEL32 names remain delegated unless the argument identifies our
 * exact XInput aliases/token.  A claimed dynamic call is counted before its
 * handler, including a loud null-output or out-of-contract fault. */
int isaac_vita_xinput_import(CPU *__restrict c, const char *name);
int isaac_vita_xinput_import_counted(CPU *__restrict c, const char *name,
                                     unsigned *call_count);
int isaac_vita_xinput_dynamic(CPU *__restrict c, uint32_t token);
int isaac_vita_xinput_dynamic_counted(CPU *__restrict c, uint32_t token,
                                      unsigned *call_count);

#endif
