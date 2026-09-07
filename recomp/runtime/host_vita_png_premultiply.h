#ifndef HOST_VITA_PNG_PREMULTIPLY_H
#define HOST_VITA_PNG_PREMULTIPLY_H
#include <stdint.h>

typedef struct IsaacVitaPngPremultiply {
    uint32_t base, bytes, width, rows, stride;
    const uint8_t *table;
} IsaacVitaPngPremultiply;

/* Only the scratch owner calls this, with its lock held and all extents
 * validated. No calls, allocation, guest state or texture operations. */
void isaac_vita_png_premultiply_rows(const IsaacVitaPngPremultiply *p);
#endif
