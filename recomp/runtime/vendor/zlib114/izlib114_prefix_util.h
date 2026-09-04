/* Prefix for the vendored infutil.c (inflate_flush + inflate_mask).
 *
 * NO_MEMCPY makes zutil.h declare zmemcpy as an external function instead of
 * expanding it to memcpy, and that external is the seam's counting hook
 * (host_vita_native_inflate_hooks.c: one count, then memcpy).  The PE's
 * inflate_flush performs exactly one memcpy import call per copy segment,
 * including zero-length ones; the count feeds the seam's import census.
 */
#ifndef IZLIB114_PREFIX_UTIL_H
#define IZLIB114_PREFIX_UTIL_H

#include "izlib114_prefix.h"

#define NO_MEMCPY 1
#define zmemcpy isaac_ni_hook_zmemcpy

#endif /* IZLIB114_PREFIX_UTIL_H */
