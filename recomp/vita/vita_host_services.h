#ifndef ISAAC_VITA_HOST_SERVICES_H
#define ISAAC_VITA_HOST_SERVICES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int isaac_vita_get_win32_filetime(uint64_t *filetime);
uint32_t isaac_vita_get_thread_id(void);
uint32_t isaac_vita_get_process_id(void);
uint64_t isaac_vita_get_process_time(void);

#ifdef __cplusplus
}
#endif

#endif
