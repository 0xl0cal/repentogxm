#ifndef HOST_VITA_SAVE_READER_GUEST_H
#define HOST_VITA_SAVE_READER_GUEST_H

#include "guest.h"

/* Entry-only fast path for frozen sub_0025ba60.  HANDLED consumes the guest
 * return word and all three stdcall arguments (RET 0x0c).  REJECTED leaves
 * CPU, stack, reader state and destination bytes untouched. */
int isaac_vita_save_reader_guest_try(CPU *__restrict c);

#endif
