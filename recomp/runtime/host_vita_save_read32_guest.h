#ifndef HOST_VITA_SAVE_READ32_GUEST_H
#define HOST_VITA_SAVE_READ32_GUEST_H

#include "guest.h"

/* Entry-only fusion for frozen sub_0052ea90.  HANDLED runs the existing
 * generated Save Reader and checksum owners and consumes RET 4.  REJECTED
 * leaves CPU and guest memory untouched for the translated wrapper. */
int isaac_vita_save_read32_guest_try(CPU *__restrict c);

#endif
