#ifndef HOST_VITA_NATIVE_PNG_LIBDEFLATE_H
#define HOST_VITA_NATIVE_PNG_LIBDEFLATE_H
#include <stdint.h>

/* Private PNG probe. The typed decoder context is local stack storage, bounded
 * at 12 KiB. No heap allocation, retained pointers or mutable shared context.
 * Failure may modify output, so only private, uncommitted output is allowed. */
int isaac_np_libdeflate_try(const uint8_t *input, uint32_t input_bytes,
                          uint8_t *output, uint32_t output_bytes);
#endif
