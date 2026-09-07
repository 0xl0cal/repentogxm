#include "host_vita_native_png_lz_neon.h"

int np_lz_test(uint8_t *output, const uint8_t *source,
               uint32_t length, uint32_t distance)
{
    return isaac_np_lz_match_try(output, source, length, distance);
}

#if ISAAC_VITA_NATIVE_PNG_LZ_D4_NEON
/* Fifth argument deliberately exercises the target AAPCS stack boundary. */
int np_lz_d4_test(uint8_t *output, const uint8_t *source,
                  uint32_t length, uint32_t distance, uint32_t history)
{
    return isaac_np_lz_d4_try(output, source, length, distance, history);
}
#endif
