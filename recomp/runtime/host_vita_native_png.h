#ifndef ISAAC_HOST_VITA_NATIVE_PNG_H
#define ISAAC_HOST_VITA_NATIVE_PNG_H

#include <stddef.h>
#include <stdint.h>

/* Native whole-image PNG decode for the frozen KAGE ImagePng loader
 * (ISAAC_VITA_NATIVE_PNG).
 *
 * SEAM.  ImagePng::load_png_data (sub_005a0e10, guest_0173.c) is the game's
 * own code: it reads the 8-byte signature through the KAGE stream, creates
 * the libpng 1.0.2 read/info structs, runs png_read_info, rejects any depth
 * other than 8, calls png_set_gamma(screen 2.2, file gAMA or 1/2.2), then
 * png_read_update_info, allocates its power-of-two texel buffer, builds the
 * row pointer table and runs the inlined png_read_image loop:
 *
 *     for (pass = 0; pass < png_set_interlace_handling(); ++pass)
 *         for (y = 0; y < png_ptr->height; ++y)
 *             png_read_row(png_ptr, rows[y], NULL);       <- 0x5a139d
 *
 * followed by the game's premultiply / palette expansion, SetTexelData,
 * png_read_end and png_destroy_read_struct.  None of that is touched.  The
 * expensive part is png_read_row (sub_005b1500, guest_0175.c): the
 * translated zlib 1.1.4 inflate, the row unfilter and the memcpy into the
 * texel buffer.  It is the only libpng entry the loop calls, its sole caller
 * is in another translation unit, and it is wrapped at link time with GNU ld
 * --wrap=sub_005b1500: __wrap_sub_005b1500 below receives every call,
 * __real_sub_005b1500 is the untouched translated body used for fallbacks.
 *
 * CONVENTION (read off the translated prologue/epilogue at 0x5b1500):
 * `mov esi, ecx` = png_structp png_ptr, `mov [ebp-8], edx` = png_bytep row,
 * [ebp+8] = dsp_row is never read (the game passes NULL; the LTCG caller
 * pushes a dead register), plain `ret`; the caller cleans with `add esp, 4`.
 * eax/ecx/edx are clobbered by the translated body and unused by the caller
 * afterwards; ebx/esi/edi/ebp are preserved because the wrapper never
 * writes them.  The wrapper performs exactly the epilogue's `ret`: it pops
 * the return word and leaves the argument word for the caller.
 *
 * MODEL.  On the first png_read_row of an image (row_number == 0) the
 * wrapper validates the frozen png_struct (layout below) and, when the
 * image is 8-bit, non-interlaced, of colour type 0/2/3/4/6 and the only
 * active transformation is at most PNG_GAMMA, reads every IDAT chunk from
 * the guest stream through the same vtable slot the translated default read
 * callback (0x5c74c0) uses, inflates them with the in-tree public-domain
 * tinfl (miniz v1.15, host_vita_archive_miniz_native.c), unfilters the rows
 * with the PNG filter equations, applies the guest's own gamma_table exactly
 * as png_do_gamma does, and keeps the result in a host scratch buffer.  Each
 * png_read_row call then copies one row into the guest row pointer and
 * mirrors the translated bookkeeping (row_number, row_info, and after the
 * last row: idat_size = 0, crc = CRC of the last IDAT chunk, PNG_AFTER_IDAT,
 * PNG_FLAG_ZLIB_FINISHED, zstream avail_out = 0, row_buf/prev_row holding
 * the last row) so png_read_end and png_destroy_read_struct run unchanged.
 * The stream is left where the translated loop leaves it: after the data of
 * the last IDAT chunk, before its CRC, which png_read_end verifies itself.
 *
 * EXACTNESS.  The decoded bytes are the PNG's own channel bytes: the game
 * applies no expand/strip/filler/BGR/swap transform (png_read_update_info is
 * called right after png_set_gamma), so the translated png_read_row output
 * for a supported image is inflate + unfilter + (gamma_table map on the
 * colour channels when PNG_GAMMA is set).  DEFLATE decoding is
 * deterministic for valid streams; the CRC and Adler-32 are checked exactly
 * as libpng/zlib check them.  Any deviation from the well-formed case -- a
 * CRC mismatch, a non-IDAT chunk before the zlib stream ends, the stream
 * ending before or after the last row, trailing bytes in an IDAT chunk,
 * a filter byte above 4, a short read, an allocation failure -- rewinds the
 * stream to the position at entry and runs the translated body for every
 * row of that image, so the game observes exactly its original error path.
 *
 * VERIFY (ISAAC_VITA_NATIVE_PNG_VERIFY).  The native decode runs first, the
 * stream is rewound, the translated body serves every row and the wrapper
 * compares each row and the final png_struct/stream state against the native
 * result, logging bounded `KAGE VITA NATIVE PNG VERIFY` lines; the guest
 * always receives the translated bytes. */

/* Frozen RVAs (8,650,240-byte Repentance PE, image base GUEST_IMAGE_BASE).
 * Return words pushed by generated call sites are plain RVAs; code pointers
 * stored in guest memory (read_data_fn) carry GUEST_IMAGE_BASE. */
#define ISAAC_NP_READ_ROW_RVA           0x005b1500U
#define ISAAC_NP_READ_ROW_SITE_RVA      0x005a13a2U /* return word pushed by the loop */
#define ISAAC_NP_DEFAULT_READ_FN_RVA    0x005c74c0U /* KAGE png_default_read_data */
#define ISAAC_NP_SITE_STREAM_READ_RVA   0x005c74d7U /* its stream->read return word */
#define ISAAC_NP_LOADER_RVA             0x005a0e10U

/* KAGE stream vtable slots (same class the OGG path uses). */
#define ISAAC_NP_STREAM_VT_TELL         0x08U
#define ISAAC_NP_STREAM_VT_SEEK         0x0cU
#define ISAAC_NP_STREAM_VT_READ         0x14U

/* Frozen libpng 1.0.2 png_struct layout (MSVC x86, 64-byte jmp_buf). */
#define ISAAC_NP_PNG_READ_DATA_FN       0x50U
#define ISAAC_NP_PNG_IO_PTR             0x5cU
#define ISAAC_NP_PNG_MODE               0x60U
#define ISAAC_NP_PNG_FLAGS              0x64U
#define ISAAC_NP_PNG_TRANSFORMATIONS    0x68U
#define ISAAC_NP_PNG_Z_NEXT_IN          0x6cU
#define ISAAC_NP_PNG_Z_AVAIL_IN         0x70U
#define ISAAC_NP_PNG_Z_TOTAL_IN         0x74U
#define ISAAC_NP_PNG_Z_NEXT_OUT         0x78U
#define ISAAC_NP_PNG_Z_AVAIL_OUT        0x7cU
#define ISAAC_NP_PNG_Z_TOTAL_OUT        0x80U
#define ISAAC_NP_PNG_ZBUF               0xa4U
#define ISAAC_NP_PNG_ZBUF_SIZE          0xa8U
#define ISAAC_NP_PNG_WIDTH              0xc0U
#define ISAAC_NP_PNG_HEIGHT             0xc4U
#define ISAAC_NP_PNG_NUM_ROWS           0xc8U
#define ISAAC_NP_PNG_ROWBYTES           0xd0U
#define ISAAC_NP_PNG_IROWBYTES          0xd4U
#define ISAAC_NP_PNG_IWIDTH             0xd8U
#define ISAAC_NP_PNG_ROW_NUMBER         0xdcU
#define ISAAC_NP_PNG_PREV_ROW           0xe0U
#define ISAAC_NP_PNG_ROW_BUF            0xe4U
#define ISAAC_NP_PNG_RI_WIDTH           0xf8U
#define ISAAC_NP_PNG_RI_ROWBYTES        0xfcU
#define ISAAC_NP_PNG_RI_COLOR_TYPE      0x100U
#define ISAAC_NP_PNG_RI_BIT_DEPTH       0x101U
#define ISAAC_NP_PNG_RI_CHANNELS        0x102U
#define ISAAC_NP_PNG_RI_PIXEL_DEPTH     0x103U
#define ISAAC_NP_PNG_IDAT_SIZE          0x104U
#define ISAAC_NP_PNG_CRC                0x108U
#define ISAAC_NP_PNG_CHUNK_NAME         0x114U
#define ISAAC_NP_PNG_INTERLACED         0x11bU
#define ISAAC_NP_PNG_PASS               0x11cU
#define ISAAC_NP_PNG_COLOR_TYPE         0x11eU
#define ISAAC_NP_PNG_BIT_DEPTH          0x11fU
#define ISAAC_NP_PNG_PIXEL_DEPTH        0x121U
#define ISAAC_NP_PNG_CHANNELS           0x122U
#define ISAAC_NP_PNG_GAMMA_TABLE        0x15cU
#define ISAAC_NP_PNG_READ_ROW_FN        0x190U

/* libpng 1.0.2 mode / flags / transformation bits used here. */
#define ISAAC_NP_MODE_HAVE_IDAT         0x04U
#define ISAAC_NP_MODE_AFTER_IDAT        0x08U
#define ISAAC_NP_MODE_HAVE_IEND         0x10U
#define ISAAC_NP_FLAG_ZLIB_FINISHED     0x20U
#define ISAAC_NP_FLAG_ROW_INIT          0x40U
#define ISAAC_NP_FLAG_CRC_MASK          0xf00U
#define ISAAC_NP_TRANSFORM_INTERLACE    0x0002U
#define ISAAC_NP_TRANSFORM_GAMMA        0x2000U

/* Geometry. */
#define ISAAC_NP_MAX_DIMENSION          0x4000U       /* the game's own bound */
#ifndef ISAAC_NP_SCRATCH_MAX_BYTES
#define ISAAC_NP_SCRATCH_MAX_BYTES      (32U * 1024U * 1024U)
#endif
#ifndef ISAAC_NP_SCRATCH_RETAIN_BYTES
#define ISAAC_NP_SCRATCH_RETAIN_BYTES   (4U * 1024U * 1024U)
#endif
#ifndef ISAAC_NP_STAGING_BYTES
#define ISAAC_NP_STAGING_BYTES          (64U * 1024U)
#endif
#define ISAAC_NP_TINFL_STATE_BYTES      0x2af0U

/* Dedicated scratch reserve (ISAAC_VITA_NATIVE_PNG_RESERVE_MB, a CMake cache
 * variable).  One USER_RW memblock taken in entry_vita.c before the guest's
 * first KAGE call runs vglInit*, which hands every USER_RW byte above
 * KAGE_VITA_RAM_THRESHOLD (16 MiB without audio) to the vitaGL RAM pool.
 * The texel scratch (0xbd6000), the ANM2 scratch (0x2a2000), the async-save
 * arena and the room-entry block are all requested after that init and
 * already share what is left of the window, so the per-image block this
 * decoder used to request at the first row was refused on device for every
 * large sheet (perf:wf-flags-v5-png: 1024x2048 at 13.6 s, then 1024x1024,
 * 960x800 and 1024x880 with `reason=memory`, alloc_refused in STATS) while
 * a 1 MiB block from a 512x512 sheet kept serving the small ones.  A sheet
 * needs height * (rowbytes + 1) bytes; the 64 KiB slack covers the one
 * filter byte per row up to the game's 0x4000 bound, so N MiB holds every
 * sheet with height * rowbytes <= N MiB: 16 -> 2048x2048 RGBA (or
 * 4096x1024), 12 -> 1536x2048 RGBA, 8 -> 1024x2048 RGBA.  Images above the
 * reserve still try a per-image block (the previous behaviour) and
 * otherwise fall back to the translated decoder; 0 disables the reserve. */
#ifndef ISAAC_VITA_NATIVE_PNG_RESERVE_MB
#define ISAAC_VITA_NATIVE_PNG_RESERVE_MB 16
#endif
#if ISAAC_VITA_NATIVE_PNG_RESERVE_MB > 0
#define ISAAC_NP_RESERVE_BYTES \
    ((uint32_t)ISAAC_VITA_NATIVE_PNG_RESERVE_MB * 1024U * 1024U + 64U * 1024U)
#else
#define ISAAC_NP_RESERVE_BYTES          0U
#endif

/* Vita eboot only.  Idempotent; returns 1 when the reserve is ready, 0 when
 * it is disabled or was refused (the decoder then keeps per-image blocks). */
int isaac_vita_native_png_reserve(void);

/* Receipt bounds. */
#define ISAAC_NP_RECEIPT_IMAGES_DENSE   24U
#define ISAAC_NP_FALLBACK_LINES_DENSE   32U
#define ISAAC_NP_STATS_EVERY            128U

/* Core decoder (pure host code; the oracle drives it with a file reader). */
enum isaac_np_status {
    ISAAC_NP_OK = 0,
    ISAAC_NP_REJECT_READ,       /* short read from the stream */
    ISAAC_NP_REJECT_CRC,        /* IDAT CRC mismatch */
    ISAAC_NP_REJECT_NOT_IDAT,   /* non-IDAT chunk before the stream ended */
    ISAAC_NP_REJECT_LENGTH,     /* chunk length above 0x7fffffff */
    ISAAC_NP_REJECT_INFLATE,    /* corrupt deflate stream or Adler-32 */
    ISAAC_NP_REJECT_EXTRA,      /* data after the end of the zlib stream */
    ISAAC_NP_REJECT_TRUNCATED,  /* zlib stream ended before the last row */
    ISAAC_NP_REJECT_FILTER,     /* filter byte above 4 */
    ISAAC_NP_REJECT_PARAM,
    ISAAC_NP_STATUS_COUNT
};

typedef struct isaac_np_params {
    uint32_t width;
    uint32_t height;
    uint32_t channels;          /* 1..4, 8 bits each */
    uint32_t color_type;        /* 0, 2, 3, 4, 6 */
    uint32_t rowbytes;          /* width * channels */
    uint32_t first_remaining;   /* png_ptr->idat_size at entry */
    uint32_t crc_entry;         /* png_ptr->crc at entry (covers "IDAT") */
    const uint8_t *gamma_table; /* 256 entries, NULL when PNG_GAMMA is off */
} isaac_np_params;

/* Returns the bytes actually read; fewer than requested is an error. */
typedef uint32_t (*isaac_np_read_fn)(void *ctx, uint8_t *dst, uint32_t n);

typedef struct isaac_np_result {
    int status;
    uint32_t chunks;            /* IDAT chunks consumed, including the entry one */
    uint32_t idat_bytes;        /* compressed payload bytes */
    uint32_t stream_bytes;      /* bytes read from the stream */
    uint32_t crc_final;         /* running CRC of the last IDAT chunk */
    uint32_t last_filter;       /* filter byte of the last row */
    uint32_t filters[5];        /* rows per filter type */
    uint32_t io_us;             /* time inside the read callback */
} isaac_np_result;

typedef struct isaac_np_work {
    uint8_t *raw;               /* height * (rowbytes + 1) */
    uint8_t *staging;           /* staging_bytes */
    uint32_t staging_bytes;
    void *tinfl_state;          /* ISAAC_NP_TINFL_STATE_BYTES, 4-aligned */
    uint8_t *last_row_pre_gamma;/* rowbytes; the last row before the gamma map */
    uint64_t (*now_us)(void);   /* optional clock for io_us */
} isaac_np_work;

/* Decode every IDAT chunk of one image.  On ISAAC_NP_OK, work->raw holds
 * height rows of (filter byte, rowbytes unfiltered and gamma-mapped bytes).
 * Any other status leaves the caller to rewind the stream. */
int isaac_np_decode(const isaac_np_params *params, isaac_np_read_fn read,
                    void *ctx, isaac_np_work *work, isaac_np_result *out);

const char *isaac_np_status_name(int status);
uint32_t isaac_np_crc32(uint32_t crc, const void *data, uint32_t bytes);
uint32_t isaac_np_channels_for(uint32_t color_type);

#endif
