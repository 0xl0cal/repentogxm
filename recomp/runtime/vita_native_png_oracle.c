/* Host oracle for host_vita_native_png.c (ISAAC_VITA_NATIVE_PNG).
 *
 * Drives the decoder core (isaac_np_decode) exactly as the Vita wrapper
 * does: the PNG is parsed up to the first IDAT chunk header the way the
 * translated png_read_info leaves the stream (idat_size = first length,
 * crc = CRC of the four type bytes), then every IDAT chunk is read through a
 * byte-source callback, inflated with the in-tree tinfl, unfiltered and
 * optionally gamma-mapped.  The unfiltered rows are written to a file for
 * the Python reference (recomp/test_vita_native_png.py) to compare, the
 * last row before the gamma map to a sibling file, and one status line is
 * printed:
 *
 *   status=<ok|read|crc|not-idat|length|inflate|extra|truncated|filter>
 *   width= height= ct= ch= chunks= idat= stream_bytes= end_offset= crc=
 *   filters=a/b/c/d/e
 *
 * Fault injection reproduces the wrapper's fallback triggers: --truncate N
 * limits the readable bytes (short read), --flip OFF xors one byte in the
 * file, --staging N shrinks the staging buffer so chunk boundaries fall in
 * the middle of inflate calls.  --gamma-invert / --gamma-pow G supply a
 * gamma table like png_ptr->gamma_table.
 *
 * Usage: vita-native-png-oracle [options] <in.png> <out.raw> */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_vita_native_png.h"

typedef struct reader {
    const uint8_t *data;
    uint32_t size;      /* readable bytes (after --truncate) */
    uint32_t pos;
} reader;

static uint32_t reader_read(void *ctx, uint8_t *dst, uint32_t n)
{
    reader *r = (reader *)ctx;
    uint32_t left = r->pos < r->size ? r->size - r->pos : 0U;
    uint32_t got = n < left ? n : left;
    memcpy(dst, r->data + r->pos, got);
    r->pos += got;
    return got;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint8_t *read_file(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 ||
            fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = (uint8_t *)malloc((size_t)n + 1U);
    if (!buf || fread(buf, 1U, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (uint32_t)n;
    return buf;
}

int main(int argc, char **argv)
{
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    const char *in_path = NULL;
    const char *out_path = NULL;
    uint32_t truncate_at = 0xffffffffU;
    uint32_t staging_bytes = ISAAC_NP_STAGING_BYTES;
    long flip_offset = -1;
    int gamma_mode = 0;
    double gamma_pow = 1.0;
    uint8_t gamma_table[256];
    uint8_t *file;
    uint32_t file_size;
    uint32_t pos;
    uint32_t width = 0U, height = 0U, bit_depth = 0U, color_type = 0U;
    uint32_t interlace = 0U;
    int have_ihdr = 0;
    isaac_np_params params;
    isaac_np_work work;
    isaac_np_result result;
    reader src;
    uint8_t *raw;
    uint8_t *last;
    uint32_t stride;
    uint64_t total;
    int i;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--truncate") && i + 1 < argc)
            truncate_at = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--flip") && i + 1 < argc)
            flip_offset = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--staging") && i + 1 < argc)
            staging_bytes = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--gamma-invert"))
            gamma_mode = 1;
        else if (!strcmp(argv[i], "--gamma-pow") && i + 1 < argc) {
            gamma_mode = 2;
            gamma_pow = strtod(argv[++i], NULL);
        } else if (!in_path)
            in_path = argv[i];
        else if (!out_path)
            out_path = argv[i];
        else {
            fprintf(stderr, "unexpected argument %s\n", argv[i]);
            return 2;
        }
    }
    if (!in_path || !out_path) {
        fprintf(stderr, "usage: %s [--truncate N] [--flip OFF] [--staging N] "
                "[--gamma-invert|--gamma-pow G] in.png out.raw\n", argv[0]);
        return 2;
    }
    file = read_file(in_path, &file_size);
    if (!file) {
        fprintf(stderr, "cannot read %s\n", in_path);
        return 2;
    }
    if (flip_offset >= 0 && (uint32_t)flip_offset < file_size)
        file[flip_offset] ^= 0x5aU;
    if (file_size < 8U || memcmp(file, sig, 8) != 0) {
        printf("status=unsupported reason=signature\n");
        return 0;
    }
    /* png_read_info: walk the chunks up to the first IDAT header. */
    pos = 8U;
    for (;;) {
        uint32_t length;
        const uint8_t *type;
        if (pos + 8U > file_size) {
            printf("status=unsupported reason=no-idat\n");
            return 0;
        }
        length = be32(file + pos);
        type = file + pos + 4U;
        if (!memcmp(type, "IHDR", 4) && length == 13U && pos + 8U + 13U <= file_size) {
            width = be32(file + pos + 8U);
            height = be32(file + pos + 12U);
            bit_depth = file[pos + 16U];
            color_type = file[pos + 17U];
            interlace = file[pos + 20U];
            have_ihdr = 1;
        }
        if (!memcmp(type, "IDAT", 4)) {
            pos += 8U;     /* data start: where the translated loop begins */
            params.first_remaining = length;
            params.crc_entry = isaac_np_crc32(0U, type, 4U);
            break;
        }
        pos += 12U + length;
    }
    if (!have_ihdr || bit_depth != 8U || interlace != 0U ||
            isaac_np_channels_for(color_type) == 0U ||
            width == 0U || height == 0U ||
            width > ISAAC_NP_MAX_DIMENSION || height > ISAAC_NP_MAX_DIMENSION) {
        printf("status=unsupported reason=shape depth=%u il=%u ct=%u %ux%u\n",
               (unsigned)bit_depth, (unsigned)interlace, (unsigned)color_type,
               (unsigned)width, (unsigned)height);
        return 0;
    }
    params.width = width;
    params.height = height;
    params.color_type = color_type;
    params.channels = isaac_np_channels_for(color_type);
    params.rowbytes = width * params.channels;
    params.gamma_table = NULL;
    if (gamma_mode) {
        for (i = 0; i < 256; ++i) {
            if (gamma_mode == 1)
                gamma_table[i] = (uint8_t)(255 - i);
            else
                gamma_table[i] = (uint8_t)(pow((double)i / 255.0, gamma_pow) *
                                           255.0 + 0.5);
        }
        params.gamma_table = gamma_table;
    }
    stride = params.rowbytes + 1U;
    total = (uint64_t)height * stride;
    raw = (uint8_t *)malloc((size_t)total);
    last = (uint8_t *)malloc(params.rowbytes);
    work.raw = raw;
    work.staging = (uint8_t *)malloc(staging_bytes);
    work.staging_bytes = staging_bytes;
    work.tinfl_state = malloc(ISAAC_NP_TINFL_STATE_BYTES);
    work.last_row_pre_gamma = last;
    work.now_us = NULL;
    if (!raw || !last || !work.staging || !work.tinfl_state) {
        fprintf(stderr, "out of memory\n");
        return 2;
    }
    /* A fresh allocation must not accidentally hide incomplete stream reset.
     * The production decoder, not the fixture allocator, owns initialization. */
    memset(work.tinfl_state, 0xa5, ISAAC_NP_TINFL_STATE_BYTES);
    src.data = file;
    src.size = truncate_at < file_size ? truncate_at : file_size;
    src.pos = pos;
    (void)isaac_np_decode(&params, reader_read, &src, &work, &result);
    printf("status=%s width=%u height=%u ct=%u ch=%u chunks=%u idat=%u "
           "stream_bytes=%u end_offset=%u crc=%08x filters=%u/%u/%u/%u/%u\n",
           isaac_np_status_name(result.status), (unsigned)width,
           (unsigned)height, (unsigned)color_type, (unsigned)params.channels,
           (unsigned)result.chunks, (unsigned)result.idat_bytes,
           (unsigned)result.stream_bytes, (unsigned)(pos + result.stream_bytes),
           (unsigned)result.crc_final, (unsigned)result.filters[0],
           (unsigned)result.filters[1], (unsigned)result.filters[2],
           (unsigned)result.filters[3], (unsigned)result.filters[4]);
    if (result.status == ISAAC_NP_OK) {
        FILE *out = fopen(out_path, "wb");
        char last_path[4096];
        FILE *lf;
        uint32_t y;
        if (!out) {
            fprintf(stderr, "cannot write %s\n", out_path);
            return 2;
        }
        for (y = 0U; y < height; ++y)
            fwrite(raw + (size_t)y * stride + 1U, 1U, params.rowbytes, out);
        fclose(out);
        snprintf(last_path, sizeof(last_path), "%s.last", out_path);
        lf = fopen(last_path, "wb");
        if (lf) {
            fputc((int)result.last_filter, lf);
            fwrite(last, 1U, params.rowbytes, lf);
            fclose(lf);
        }
    }
#if ISAAC_VITA_NATIVE_PNG_LIBDEFLATE_STRICT
    printf("strict=%u/%u/%u\n", result.strict_attempts,
           result.strict_successes, result.strict_refusals);
#endif
    free(raw);
    free(last);
    free(work.staging);
    free(work.tinfl_state);
    free(file);
    return 0;
}
