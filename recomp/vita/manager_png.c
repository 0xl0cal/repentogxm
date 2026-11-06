/*
 * PNG subset decoder for the manager's game skin; see manager_png.h.
 *
 * Sources: chunk layout and the IHDR fields per the PNG specification as
 * already used by the design's asset probe (">IIBB" at offset 16) and
 * recomp/runtime/host_vita_native_png.c; the inflate call pattern (zlib
 * header + more-input + non-wrapping, one call per IDAT slice) and the four
 * unfilter routines are copied from host_vita_native_png.c (isaac_np_decode
 * and np_unfilter_*, minus their NEON paths).
 */
#include "manager_png.h"

#include "manager_tinfl.h"

#include <string.h>

static const uint8_t manager_png_signature[8] = {
    0x89u, 'P', 'N', 'G', '\r', '\n', 0x1Au, '\n'};

struct png_layout {
    struct manager_png_info info;
    size_t first_idat; /* offset of the first IDAT chunk's length field */
    uint32_t idat_count;
};

static uint32_t png_be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static int png_type_is(const uint8_t *type, const char *name)
{
    return memcmp(type, name, 4) == 0;
}

static int png_parse_ihdr(const uint8_t *data, struct manager_png_info *info)
{
    uint8_t compression = data[10];
    uint8_t filter = data[11];

    info->width = png_be32(data);
    info->height = png_be32(data + 4);
    info->bit_depth = data[8];
    info->color_type = data[9];
    info->interlace = data[12];
    info->channels = 0u;
    if (info->width == 0u || info->height == 0u ||
        info->width > MANAGER_PNG_MAX_DIMENSION ||
        info->height > MANAGER_PNG_MAX_DIMENSION)
        return MANAGER_PNG_E_DIMENSIONS;
    if (info->bit_depth != 8u ||
        (info->color_type != 2u && info->color_type != 6u) ||
        compression != 0u || filter != 0u || info->interlace != 0u)
        return MANAGER_PNG_E_UNSUPPORTED;
    info->channels = info->color_type == 6u ? 4u : 3u;
    return MANAGER_PNG_OK;
}

/* Every chunk: 4-byte big-endian length, 4-byte type, data, 4-byte CRC. */
static int png_walk(const uint8_t *png, size_t size, struct png_layout *layout)
{
    size_t pos = sizeof(manager_png_signature);
    int seen_ihdr = 0;
    int idat_run = 0; /* 0 = none yet, 1 = inside the run, 2 = run ended */

    if (size < sizeof(manager_png_signature) ||
        memcmp(png, manager_png_signature, sizeof(manager_png_signature)) != 0)
        return MANAGER_PNG_E_SIGNATURE;
    for (;;) {
        const uint8_t *type;
        uint32_t length;
        int status;

        if (size - pos < 12u)
            return MANAGER_PNG_E_CHUNK;
        length = png_be32(png + pos);
        type = png + pos + 4;
        if (length > size - pos - 12u)
            return MANAGER_PNG_E_CHUNK;
        if (!seen_ihdr) {
            if (!png_type_is(type, "IHDR") || length != 13u)
                return MANAGER_PNG_E_CHUNK;
            status = png_parse_ihdr(png + pos + 8, &layout->info);
            if (status != MANAGER_PNG_OK)
                return status;
            seen_ihdr = 1;
        } else if (png_type_is(type, "IHDR")) {
            return MANAGER_PNG_E_CHUNK;
        } else if (png_type_is(type, "IDAT")) {
            if (idat_run == 2)
                return MANAGER_PNG_E_CHUNK;
            if (idat_run == 0) {
                layout->first_idat = pos;
                idat_run = 1;
            }
            ++layout->idat_count;
        } else {
            if (idat_run == 1)
                idat_run = 2;
            if (png_type_is(type, "IEND"))
                break;
            /* A colour key would need alpha synthesis: not in the subset. */
            if (png_type_is(type, "tRNS"))
                return MANAGER_PNG_E_UNSUPPORTED;
            /* PLTE is an optional suggested palette for colour types 2/6;
             * any other critical chunk (bit 5 of the first byte clear) is
             * unknown; ancillary chunks are skipped. */
            if (!png_type_is(type, "PLTE") && (type[0] & 0x20u) == 0u)
                return MANAGER_PNG_E_UNSUPPORTED;
        }
        pos += 12u + (size_t)length;
    }
    if (layout->idat_count == 0u)
        return MANAGER_PNG_E_CHUNK;
    return MANAGER_PNG_OK;
}

int manager_png_probe(const uint8_t *png, size_t size,
                      struct manager_png_info *info)
{
    struct png_layout layout;
    int status;

    if (png == NULL)
        return MANAGER_PNG_E_ARGUMENT;
    memset(&layout, 0, sizeof(layout));
    status = png_walk(png, size, &layout);
    if (info != NULL)
        *info = layout.info;
    return status;
}

size_t manager_png_buffer_bytes(const struct manager_png_info *info)
{
    uint64_t image;
    uint64_t raw;

    if (info == NULL || (info->channels != 3u && info->channels != 4u) ||
        info->width == 0u || info->height == 0u ||
        info->width > MANAGER_PNG_MAX_DIMENSION ||
        info->height > MANAGER_PNG_MAX_DIMENSION)
        return 0u;
    image = (uint64_t)info->width * info->height * 4u;
    raw = (uint64_t)info->height * ((uint64_t)info->width * info->channels + 1u);
    return (size_t)(image > raw ? image : raw);
}

/* np_unfilter_* of host_vita_native_png.c, scalar paths. */
static void png_unfilter_sub(uint8_t *row, uint32_t n, uint32_t bpp)
{
    uint32_t i;

    for (i = bpp; i < n; ++i)
        row[i] = (uint8_t)(row[i] + row[i - bpp]);
}

static void png_unfilter_up(uint8_t *row, const uint8_t *prev, uint32_t n)
{
    uint32_t i;

    for (i = 0u; i < n; ++i)
        row[i] = (uint8_t)(row[i] + prev[i]);
}

static void png_unfilter_avg(uint8_t *row, const uint8_t *prev, uint32_t n,
                             uint32_t bpp)
{
    uint32_t i = 0u;

    if (prev) {
        for (; i < bpp && i < n; ++i)
            row[i] = (uint8_t)(row[i] + (prev[i] >> 1));
        for (; i < n; ++i)
            row[i] = (uint8_t)(row[i] +
                               (((uint32_t)row[i - bpp] + prev[i]) >> 1));
    } else {
        for (i = bpp; i < n; ++i)
            row[i] = (uint8_t)(row[i] + (row[i - bpp] >> 1));
    }
}

static void png_unfilter_paeth(uint8_t *row, const uint8_t *prev, uint32_t n,
                               uint32_t bpp)
{
    uint32_t i = 0u;

    if (!prev) {
        /* above and upper-left are zero: the predictor is always `left` */
        png_unfilter_sub(row, n, bpp);
        return;
    }
    for (; i < bpp && i < n; ++i)
        row[i] = (uint8_t)(row[i] + prev[i]);
    for (; i < n; ++i) {
        int a = row[i - bpp];
        int b = prev[i];
        int c = prev[i - bpp];
        int p = a + b - c;
        int pa = p > a ? p - a : a - p;
        int pb = p > b ? p - b : b - p;
        int pc = p > c ? p - c : c - p;
        int pred = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
        row[i] = (uint8_t)(row[i] + pred);
    }
}

int manager_png_decode(const uint8_t *png, size_t size, uint8_t *rgba,
                       size_t rgba_capacity, void *scratch,
                       size_t scratch_size, struct manager_png_info *info)
{
    const uint32_t flags = (uint32_t)MANAGER_TINFL_PARSE_ZLIB_HEADER |
                           (uint32_t)MANAGER_TINFL_HAS_MORE_INPUT |
                           (uint32_t)MANAGER_TINFL_NON_WRAPPING_OUTPUT;
    struct png_layout layout;
    uintptr_t base;
    uintptr_t aligned;
    void *state;
    size_t need;
    uint8_t *raw;
    uint32_t width;
    uint32_t height;
    uint32_t channels;
    uint32_t row_bytes;
    uint32_t stride;
    uint32_t raw_size;
    uint32_t produced = 0u;
    uint32_t chunk;
    uint32_t y;
    size_t pos;
    int done = 0;
    int status;

    if (png == NULL || rgba == NULL || scratch == NULL)
        return MANAGER_PNG_E_ARGUMENT;
    memset(&layout, 0, sizeof(layout));
    status = png_walk(png, size, &layout);
    if (info != NULL)
        *info = layout.info;
    if (status != MANAGER_PNG_OK)
        return status;

    need = manager_png_buffer_bytes(&layout.info);
    if (need == 0u || rgba_capacity < need)
        return MANAGER_PNG_E_CAPACITY;
    base = (uintptr_t)scratch;
    aligned = (base + 7u) & ~(uintptr_t)7u;
    if (scratch_size < (size_t)(aligned - base) +
                           (size_t)ISAAC_VITA_ARCHIVE_MINIZ_STATE_BYTES)
        return MANAGER_PNG_E_CAPACITY;
    state = (void *)aligned;
    memset(state, 0, ISAAC_VITA_ARCHIVE_MINIZ_STATE_BYTES);

    width = layout.info.width;
    height = layout.info.height;
    channels = layout.info.channels;
    row_bytes = width * channels;
    stride = row_bytes + 1u;
    raw_size = height * stride;
    /* The inflated rows sit at the end of the minimum buffer, so the
     * top-down unfilter-in-place and the compaction to RGBA below never
     * overtake a byte they still have to read (for RGB the gap is
     * height * (width - 1) bytes, for RGBA the rows start at 0). */
    raw = rgba + (need - raw_size);

    /* isaac_np_decode: one tinfl call per slice of input until the IDAT is
     * consumed; DONE ends the stream, HAS_MORE_OUTPUT means more data than
     * rows.  Bytes after the zlib stream inside the last IDAT are ignored. */
    pos = layout.first_idat;
    for (chunk = 0u; chunk < layout.idat_count && !done; ++chunk) {
        uint32_t length = png_be32(png + pos);
        const uint8_t *input = png + pos + 8;
        uint32_t left = length;
        pos += 12u + (size_t)length;
        for (;;) {
            uint32_t in_size = left;
            uint32_t out_size = raw_size - produced;
            uint32_t reserved = 0u;
            int inflate = manager_tinfl_decompress(state, input, &in_size, raw,
                                                   raw + produced, &out_size,
                                                   flags, &reserved);
            input += in_size;
            left -= in_size;
            produced += out_size;
            if (reserved != 0u)
                return MANAGER_PNG_E_ZLIB;
            if (inflate == ISAAC_VITA_ARCHIVE_MINIZ_DONE) {
                done = 1;
                break;
            }
            if (inflate == ISAAC_VITA_ARCHIVE_MINIZ_NEEDS_MORE_INPUT) {
                if (left != 0u)
                    return MANAGER_PNG_E_ZLIB;
                break;
            }
            if (inflate == ISAAC_VITA_ARCHIVE_MINIZ_HAS_MORE_OUTPUT)
                return MANAGER_PNG_E_DATA;
            return MANAGER_PNG_E_ZLIB;
        }
    }
    if (!done)
        return MANAGER_PNG_E_ZLIB;
    if (produced != raw_size)
        return MANAGER_PNG_E_DATA;

    /* Unfilter in place; the filter byte stays in front of every row. */
    for (y = 0u; y < height; ++y) {
        uint8_t *row = raw + (size_t)y * stride;
        const uint8_t *prev = y ? row - stride + 1 : NULL;
        uint32_t filter = row[0];
        switch (filter) {
        case 0u: break;
        case 1u: png_unfilter_sub(row + 1, row_bytes, channels); break;
        case 2u: if (prev) png_unfilter_up(row + 1, prev, row_bytes); break;
        case 3u: png_unfilter_avg(row + 1, prev, row_bytes, channels); break;
        case 4u: png_unfilter_paeth(row + 1, prev, row_bytes, channels); break;
        default: return MANAGER_PNG_E_FILTER;
        }
    }

    /* Compact to width * 4 pitch, top-down (destination never ahead of the
     * source, see the layout note above). */
    for (y = 0u; y < height; ++y) {
        const uint8_t *source = raw + (size_t)y * stride + 1u;
        uint8_t *target = rgba + (size_t)y * width * 4u;
        if (channels == 4u) {
            memmove(target, source, row_bytes);
        } else {
            uint32_t x;
            for (x = 0u; x < width; ++x) {
                uint8_t r = source[3u * x];
                uint8_t g = source[3u * x + 1u];
                uint8_t b = source[3u * x + 2u];
                target[4u * x] = r;
                target[4u * x + 1u] = g;
                target[4u * x + 2u] = b;
                target[4u * x + 3u] = 0xFFu;
            }
        }
    }
    return MANAGER_PNG_OK;
}

const char *manager_png_status_text(int status)
{
    switch (status) {
    case MANAGER_PNG_OK: return "ok";
    case MANAGER_PNG_E_ARGUMENT: return "argument";
    case MANAGER_PNG_E_SIGNATURE: return "signature";
    case MANAGER_PNG_E_CHUNK: return "chunk";
    case MANAGER_PNG_E_UNSUPPORTED: return "unsupported";
    case MANAGER_PNG_E_DIMENSIONS: return "dimensions";
    case MANAGER_PNG_E_CAPACITY: return "capacity";
    case MANAGER_PNG_E_ZLIB: return "zlib";
    case MANAGER_PNG_E_DATA: return "data";
    case MANAGER_PNG_E_FILTER: return "filter";
    default: return "unknown";
    }
}
