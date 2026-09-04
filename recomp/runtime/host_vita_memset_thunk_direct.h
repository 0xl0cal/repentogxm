#ifndef ISAAC_HOST_VITA_MEMSET_THUNK_DIRECT_H
#define ISAAC_HOST_VITA_MEMSET_THUNK_DIRECT_H

#include "guest.h"

/* HANDLED enters the already-registered memset import by its frozen dense
 * binding.  REJECTED leaves CPU, guest stack and all counters untouched so
 * the generated thunk can execute its original guest_call. */
int isaac_vita_memset_thunk_direct_try(
    CPU *__restrict c, uint32_t target);

#endif
