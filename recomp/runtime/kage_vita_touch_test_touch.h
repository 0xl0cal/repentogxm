#ifndef KAGE_VITA_TOUCH_TEST_TOUCH_H
#define KAGE_VITA_TOUCH_TEST_TOUCH_H

#include <stdint.h>

typedef int16_t SceInt16;
typedef uint8_t SceUInt8;
typedef uint16_t SceUInt16;
typedef uint32_t SceUInt32;
typedef uint64_t SceUInt64;

#define SCE_TOUCH_MAX_REPORT 8

typedef enum SceTouchPortType {
    SCE_TOUCH_PORT_FRONT = 0,
    SCE_TOUCH_PORT_BACK = 1,
    SCE_TOUCH_PORT_MAX_NUM = 2
} SceTouchPortType;

typedef enum SceTouchSamplingState {
    SCE_TOUCH_SAMPLING_STATE_STOP = 0,
    SCE_TOUCH_SAMPLING_STATE_START = 1
} SceTouchSamplingState;

typedef struct SceTouchPanelInfo {
    SceInt16 minAaX;
    SceInt16 minAaY;
    SceInt16 maxAaX;
    SceInt16 maxAaY;
    SceInt16 minDispX;
    SceInt16 minDispY;
    SceInt16 maxDispX;
    SceInt16 maxDispY;
    SceUInt8 minForce;
    SceUInt8 maxForce;
    SceUInt8 reserved[30];
} SceTouchPanelInfo;

typedef struct SceTouchReport {
    SceUInt8 id;
    SceUInt8 force;
    SceInt16 x;
    SceInt16 y;
    SceUInt8 reserved[8];
    SceUInt16 info;
} SceTouchReport;

typedef struct SceTouchData {
    SceUInt64 timeStamp;
    SceUInt32 status;
    SceUInt32 reportNum;
    SceTouchReport report[SCE_TOUCH_MAX_REPORT];
} SceTouchData;

int sceTouchGetPanelInfo(SceUInt32 port, SceTouchPanelInfo *panel_info);
int sceTouchPeek(SceUInt32 port, SceTouchData *data, SceUInt32 buffers);
int sceTouchSetSamplingState(SceUInt32 port,
                             SceTouchSamplingState state);

_Static_assert(sizeof(SceTouchPanelInfo) == 0x30U,
               "fake SceTouchPanelInfo layout changed");
_Static_assert(sizeof(SceTouchReport) == 0x10U,
               "fake SceTouchReport layout changed");
_Static_assert(sizeof(SceTouchData) == 0x90U,
               "fake SceTouchData layout changed");

#endif
