#ifndef ISAAC_MANAGER_PNG_H
#define ISAAC_MANAGER_PNG_H

/*
 * Minimal PNG decoder for the manager's game skin (theme=game):
 * exactly what the game's menu sheets and font pages
 * use - bit depth 8, colour type 6 (RGBA) or 2 (RGB), no interlace - and
 * nothing else.  The zlib stream goes through the runtime tinfl
 * (manager_tinfl.c, non-wrapping output, adler32 checked); all five row
 * filters; ancillary chunks skipped, chunk CRCs not checked.  Anything
 * outside that subset, or any structural damage, returns an error status;
 * the decoder never reads outside the input or writes outside the output.
 *
 * Output: RGBA8, rows top-down, pitch width * 4 bytes.  Byte order R, G, B,
 * A, i.e. one little-endian word per pixel reads 0xAABBGGRR, which is the
 * manager canvas' SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 word.  RGB images get
 * alpha 255.  No gamma or colour-profile chunk is applied (Pillow does not
 * apply them either).
 *
 * Memory: the caller owns everything.  The output buffer doubles as the
 * inflate target, so it must hold manager_png_buffer_bytes(info) bytes
 * (the inflated rows are width * channels + 1 bytes each; for RGBA that is
 * height bytes more than the image).  Scratch holds the tinfl state.
 */

#include <stddef.h>
#include <stdint.h>

#include "host_vita_archive_miniz_native.h"

#define MANAGER_PNG_MAX_DIMENSION 4096u
/* tinfl state plus alignment slack. */
#define MANAGER_PNG_SCRATCH_BYTES (ISAAC_VITA_ARCHIVE_MINIZ_STATE_BYTES + 16u)

enum manager_png_status {
    MANAGER_PNG_OK = 0,
    MANAGER_PNG_E_ARGUMENT = -1,
    MANAGER_PNG_E_SIGNATURE = -2,   /* not a PNG signature */
    MANAGER_PNG_E_CHUNK = -3,       /* chunk crosses the end, IHDR not first
                                       or not 13 bytes, IDATs missing or not
                                       consecutive, no IEND */
    MANAGER_PNG_E_UNSUPPORTED = -4, /* bit depth != 8, colour type not 2/6,
                                       interlaced, compression/filter method
                                       != 0, tRNS, unknown critical chunk */
    MANAGER_PNG_E_DIMENSIONS = -5,  /* 0 or > MANAGER_PNG_MAX_DIMENSION */
    MANAGER_PNG_E_CAPACITY = -6,    /* output or scratch too small */
    MANAGER_PNG_E_ZLIB = -7,        /* bad zlib header, corrupt or truncated
                                       stream, reserved symbol, adler32 */
    MANAGER_PNG_E_DATA = -8,        /* stream longer or shorter than rows */
    MANAGER_PNG_E_FILTER = -9       /* row filter type > 4 */
};

struct manager_png_info {
    uint32_t width;
    uint32_t height;
    uint8_t bit_depth;
    uint8_t color_type;
    uint8_t interlace;
    uint8_t channels; /* 3 or 4 once validated */
};

/* Walks every chunk and validates IHDR; fills info.  Same verdicts as
 * manager_png_decode() short of inflating. */
int manager_png_probe(const uint8_t *png, size_t size,
                      struct manager_png_info *info);

/* Output buffer bytes manager_png_decode() needs: max(width * height * 4,
 * height * (width * channels + 1)).  0 for an unvalidated info. */
size_t manager_png_buffer_bytes(const struct manager_png_info *info);

/* Decodes into rgba (capacity >= manager_png_buffer_bytes()).  On success
 * rgba[0 .. width * height * 4) is the image; the bytes after it are
 * scratch.  info may be NULL. */
int manager_png_decode(const uint8_t *png, size_t size, uint8_t *rgba,
                       size_t rgba_capacity, void *scratch,
                       size_t scratch_size, struct manager_png_info *info);

/* Stable short name of a status ("ok", "signature", "chunk", ...). */
const char *manager_png_status_text(int status);

#endif
