/* ARM-only fixture: include the real decoder to exercise its dispatch and
 * scalar tails, not a reimplementation of the new block helper. Linked with
 * --gc-sections so unused decoder/guest paths need no libc or Vita runtime. */
#define ISAAC_VITA_NATIVE_PNG_ORACLE 1
#include "../../runtime/host_vita_native_png.c"

void isaac_np_test_rgba_filter(uint8_t *row, const uint8_t *prev,
                              uint32_t rowbytes, uint32_t filter)
{
    switch (filter) {
    case 1U: np_unfilter_sub(row, rowbytes, 4U); break;
    case 3U: np_unfilter_avg(row, prev, rowbytes, 4U); break;
    case 4U: np_unfilter_paeth(row, prev, rowbytes, 4U); break;
    default: break;
    }
}
