#ifndef HOST_VITA_SAVE_READER_DIRECT_H
#define HOST_VITA_SAVE_READER_DIRECT_H

#include "guest.h"

/* HANDLED dispatches an exact canonical/relocated Save Reader target through
 * its generated owner.  REJECTED leaves CPU and stack untouched so the site
 * can call the original guest_call path. */
int isaac_vita_save_reader_direct_try(
    CPU *__restrict c, uint32_t target);

#endif
