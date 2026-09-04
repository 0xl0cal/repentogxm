/* Prefix for the one vendored TU that is inflate_codes itself (infcodes.c,
 * compiled through host_vita_native_inflate_zlib114_codes.c).
 *
 * On top of the iz_ namespace, the two leaves inflate_codes calls are routed
 * through the seam's counting hooks (host_vita_native_inflate_hooks.c), which
 * forward to the iz_ leaves unchanged.  The translated body reaches the same
 * two leaves through direct translated calls; the counts are what the seam
 * replays into the coverage census after a native call.
 */
#ifndef IZLIB114_PREFIX_CODES_H
#define IZLIB114_PREFIX_CODES_H

#include "izlib114_prefix.h"

#undef inflate_fast
#undef inflate_flush
#define inflate_fast   isaac_ni_hook_inflate_fast
#define inflate_flush  isaac_ni_hook_inflate_flush

#endif /* IZLIB114_PREFIX_CODES_H */
