/* Counting hooks for the vendored zlib 1.1.4 inflate_codes leaves.  See
 * host_vita_native_inflate_hooks.h.  Compiled with -I vendor/zlib114 and
 * -include izlib114_prefix.h so the zlib names below resolve to the iz_ leaves. */
#include "host_vita_native_inflate_hooks.h"

#include <string.h>

#include "zutil.h"
#include "infblock.h"
#include "inftrees.h"
#include "infcodes.h"
#include "infutil.h"
#include "inffast.h"

isaac_ni_hook_counters isaac_ni_hooks;

int isaac_ni_hook_inflate_fast(uInt bl, uInt bd, inflate_huft *tl,
                               inflate_huft *td, inflate_blocks_statef *s,
                               z_streamp z)
{
    ++isaac_ni_hooks.fast_calls;
    return inflate_fast(bl, bd, tl, td, s, z);
}

int isaac_ni_hook_inflate_flush(inflate_blocks_statef *s, z_streamp z, int r)
{
    ++isaac_ni_hooks.flush_calls;
    return inflate_flush(s, z, r);
}

/* zmemcpy of infutil.c (NO_MEMCPY): zlib 1.1.4 calls it once per copy
 * segment, zero-length segments included; the PE does the same through its
 * memcpy import. */
void isaac_ni_hook_zmemcpy(Bytef *dest, const Bytef *source, uInt len)
{
    ++isaac_ni_hooks.memcpy_calls;
    if (len != 0U)
        memcpy(dest, source, len);
}

unsigned long isaac_ni_hook_adler32(unsigned long adler,
                                    const unsigned char *buf,
                                    unsigned int len)
{
    ++isaac_ni_hooks.adler_calls;
    return adler32(adler, buf, len);
}
