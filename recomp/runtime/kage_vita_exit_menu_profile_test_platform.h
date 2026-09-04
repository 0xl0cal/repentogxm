#ifndef KAGE_VITA_EXIT_MENU_PROFILE_TEST_PLATFORM_H
#define KAGE_VITA_EXIT_MENU_PROFILE_TEST_PLATFORM_H

#include <stdint.h>

int      sceKernelGetThreadId(void);
uint64_t sceKernelGetProcessTimeWide(void);
int      sceClibPrintf(const char *format, ...);

#endif
