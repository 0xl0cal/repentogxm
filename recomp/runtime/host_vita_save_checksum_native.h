#ifndef HOST_VITA_SAVE_CHECKSUM_NATIVE_H
#define HOST_VITA_SAVE_CHECKSUM_NATIVE_H

#include <stddef.h>
#include <stdint.h>

enum {
    ISAAC_VITA_SAVE_CHECKSUM_STATE_BYTES = 16U,
    ISAAC_VITA_SAVE_CHECKSUM_TABLE_WORDS = 256U,
    ISAAC_VITA_SAVE_CHECKSUM_TABLE_BYTES = 1024U
};

/* Exact native implementation of the two modes in frozen sub_0025b340.
 * State and table are byte-addressed so unaligned guest objects remain valid.
 * The caller has already rejected mode values other than 0/1 and a mode-0
 * partial-word count above three. */
void isaac_vita_save_checksum_native_feed(
    uint8_t *state, const uint8_t *data, size_t size, uint8_t *table);

#endif
