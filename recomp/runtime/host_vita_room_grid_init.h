#ifndef ISAAC_VITA_ROOM_GRID_INIT_H
#define ISAAC_VITA_ROOM_GRID_INIT_H

#include "guest.h"

/* Exact constructor 002c4260, at first fallthrough to 002c4330.  Initialize
 * only the first 447 rows; the last original row recreates final guest flags
 * and registers.  0: untouched fallback, 1: prefix done, -1: terminal fault. */
int isaac_vita_room_grid_init_try(CPU *__restrict c, uint32_t raw_allocation);

#endif
