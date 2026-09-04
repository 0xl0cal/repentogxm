#ifndef HOST_VITA_SAVE_CHECKSUM_GUEST_H
#define HOST_VITA_SAVE_CHECKSUM_GUEST_H

#include "guest.h"

/* Entry-only fast path for frozen sub_0025b340.  HANDLED consumes the guest
 * return word and both stdcall arguments (RET 8).  REJECTED leaves CPU, stack,
 * checksum state and lazy table untouched for the translated body. */
int isaac_vita_save_checksum_guest_try(CPU *__restrict c);

#endif
