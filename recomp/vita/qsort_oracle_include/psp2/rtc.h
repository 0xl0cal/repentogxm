#ifndef ISAAC_QSORT_ORACLE_PSP2_RTC_H
#define ISAAC_QSORT_ORACLE_PSP2_RTC_H

/* Host-only declarations needed to compile the complete production CRT
 * translation unit.  The focused executable retains no RTC function after
 * --gc-sections.  Layouts and signatures mirror VitaSDK's public headers so
 * this shim cannot hide a compile-time source mismatch. */
#include <stdint.h>

typedef uint64_t SceUInt64;

typedef struct SceRtcTick {
    SceUInt64 tick;
} SceRtcTick;

typedef struct SceDateTime {
    unsigned short year;
    unsigned short month;
    unsigned short day;
    unsigned short hour;
    unsigned short minute;
    unsigned short second;
    unsigned int microsecond;
} SceDateTime;

unsigned int sceRtcGetTickResolution(void);
int sceRtcConvertUtcToLocalTime(const SceRtcTick *utc,
                                SceRtcTick *local_time);
int sceRtcSetTime64_t(SceDateTime *time, SceUInt64 value);
int sceRtcSetTick(SceDateTime *time, const SceRtcTick *tick);
int sceRtcGetTick(const SceDateTime *time, SceRtcTick *tick);

#endif
