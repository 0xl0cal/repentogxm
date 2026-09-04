#ifndef HOST_VITA_PNG_UNFILTER_NATIVE_H
#define HOST_VITA_PNG_UNFILTER_NATIVE_H

#include <stddef.h>
#include <stdint.h>

/* This experiment is default-off.  Its pinned generated entry shim may opt in;
 * normal builds continue through the exact translated png_read_filter_row. */
#ifndef ISAAC_VITA_PNG_NATIVE_UNFILTER
#define ISAAC_VITA_PNG_NATIVE_UNFILTER 0
#endif

enum {
    ISAAC_VITA_PNG_ROW_INFO_BYTES = 12,
    ISAAC_VITA_PNG_UNFILTER_FALLBACK = 0,
    ISAAC_VITA_PNG_UNFILTER_HANDLED = 1
};

/*
 * Try the five valid PNG filter types.  row_info is the frozen 32-bit guest
 * png_row_info byte image; only little-endian rowbytes at +4 and pixel_depth
 * at +11 are read.  Capacities are explicit so a future guest/host boundary
 * fails closed before native code touches either row.
 *
 * FALLBACK means that no byte was changed.  The caller must then execute the
 * frozen generated implementation, which also preserves its invalid-filter
 * error path.  HANDLED means the row now has the exact sequential PNG result.
 */
int isaac_vita_png_unfilter_try(
    const void *row_info,
    size_t row_info_capacity,
    uint8_t *row,
    size_t row_capacity,
    const uint8_t *previous_row,
    size_t previous_row_capacity,
    uint32_t filter_type);

#endif
