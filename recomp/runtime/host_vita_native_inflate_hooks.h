/* Counting hooks between the vendored zlib 1.1.4 inflate_codes and its leaves.
 *
 * infcodes.c is compiled with izlib114_prefix_codes.h, which routes its two
 * calls (inflate_fast, inflate_flush) through the hooks below; infutil.c is
 * compiled with izlib114_prefix_util.h, which routes zmemcpy here; the seam
 * swaps the guest check function for isaac_ni_hook_adler32.  Each hook counts
 * and forwards to the identical iz_ leaf, so the seam can replay the exact
 * census (coverage, import notes, translated-call notes) the translated body
 * would have produced.  No guest.h dependency: the host oracles link it too.
 */
#ifndef ISAAC_HOST_VITA_NATIVE_INFLATE_HOOKS_H
#define ISAAC_HOST_VITA_NATIVE_INFLATE_HOOKS_H

#include <stdint.h>

typedef struct isaac_ni_hook_counters {
    uint32_t fast_calls;
    uint32_t flush_calls;
    uint32_t memcpy_calls;
    uint32_t adler_calls;
} isaac_ni_hook_counters;

extern isaac_ni_hook_counters isaac_ni_hooks;

/* Prototypes in zlib's own types live in host_vita_native_inflate_hooks.c;
 * the seam only needs the adler hook's address, declared with the exact
 * check_func shape (uLong, const Bytef *, uInt) == (unsigned long,
 * const unsigned char *, unsigned int) on every ILP32 target. */
unsigned long isaac_ni_hook_adler32(unsigned long adler,
                                    const unsigned char *buf,
                                    unsigned int len);

/* zmemcpy's shape (Bytef *, const Bytef *, uInt); infutil.c reaches it through
 * its prefix header, the guest oracle's test double calls it directly. */
void isaac_ni_hook_zmemcpy(unsigned char *dest, const unsigned char *source,
                           unsigned int len);

#endif /* ISAAC_HOST_VITA_NATIVE_INFLATE_HOOKS_H */
