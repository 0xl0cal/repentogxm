#ifndef HOST_VITA_PNG_UNFILTER_GUEST_H
#define HOST_VITA_PNG_UNFILTER_GUEST_H

#include "guest.h"

/* Entry-only fast path for frozen png_read_filter_row at RVA 0x005c6bf0.
 * HANDLED consumes exactly the guest return word; FALLBACK leaves the CPU,
 * guest stack and row bytes untouched so the translated body can run. */
int isaac_vita_png_unfilter_guest_try(CPU *__restrict c);

#endif
