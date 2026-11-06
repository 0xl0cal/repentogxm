/* theora_plane_digest: host oracle for the ISAAC_VITA_NATIVE_THEORA_PLANE_DIGEST
 * receipt (recomp/runtime/host_vita_native_theora.c).  Host-only; not part of
 * any Vita target.
 *
 * Demuxes one Ogg file with libogg the way theoraplayer does (the first
 * stream whose BOS packet th_decode_headerin accepts is the video; every
 * other stream, i.e. the Vorbis audio, is skipped), parses the three Theora
 * headers, allocates the decoder and then, for every video packet in stream
 * order, calls th_decode_packetin and, after every success (0 or TH_DUPFRAME:
 * theoraplayer's decodeNextFrame accepts both as a decoded frame and reads
 * the planes for every frame it keeps; the oracle and the device seam read
 * them after every success, whatever theoraplayer's timing pre-drop later
 * skips, and never install the stripe callback dump_video uses),
 * th_decode_ycbcr_out, and prints one line per packet:
 *
 *   packet=<ordinal> ret=<packetin result> gp=<granule position> y=<crc32> cb=<crc32> cr=<crc32>
 *
 * with the CRC32 (IEEE 802.3 reflected, polynomial 0xEDB88320, initial ~0,
 * final ~x) of exactly width x height bytes of each plane, row by row through
 * the stride, for the plane's own width/height.  The device seam logs the
 * same line behind the prefix "KAGE VITA NATIVE THEORA DIGEST: "; the two
 * crc functions below are textually identical to the seam's (pinned by
 * recomp/test_native_theora_sites.py) and
 * recomp/vita/theora_plane_digest_compare.py diffs the streams.  Packets whose
 * th_decode_packetin fails (negative result) print nothing (like the device)
 * but still count towards the ordinal.  stdout carries the digest lines and
 * one '#' comment line naming the archive; diagnostics go to stderr.
 *
 * Build (one binary per host libtheora archive):
 *   gcc -std=c11 -O2 -Wall -Wextra -Werror -I$OGG/include -I$THEORA/include \
 *       theora_plane_digest.c $THEORA/lib/libtheoradec.a $OGG/lib/libogg.a -o theora_plane_digest
 * Self-test without any header (checks the crc against the IEEE check value
 * and the row/stride handling):
 *   gcc -std=c11 -O2 -Wall -Wextra -Werror -DTHEORA_PLANE_DIGEST_SELFTEST=1 \
 *       theora_plane_digest.c -o theora_plane_digest_selftest && ./theora_plane_digest_selftest
 * Usage: theora_plane_digest <clip.ogv> > digest.txt */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* --- identical to host_vita_native_theora.c (plane digest section) ------ */

static void plane_digest_crc32_init(uint32_t *table)
{
    uint32_t n;

    for (n = 0U; n < 256U; ++n) {
        uint32_t c = n;
        int k;

        for (k = 0; k < 8; ++k)
            c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
        table[n] = c;
    }
}

/* CRC32 of `height` rows of `width` bytes each, rows `stride` bytes apart;
 * the padding beyond `width` is not part of the digest.  0 for an empty
 * plane (the CRC32 of no bytes). */
static uint32_t plane_digest_crc32(const uint32_t *table,
                                   const unsigned char *data,
                                   int width, int height, int stride)
{
    uint32_t crc = 0xFFFFFFFFU;
    int y;

    if (!data || width <= 0 || height <= 0)
        return 0U;
    for (y = 0; y < height; ++y) {
        const unsigned char *row = data + (ptrdiff_t)y * stride;
        int x;

        for (x = 0; x < width; ++x)
            crc = table[(crc ^ row[x]) & 0xFFU] ^ (crc >> 8);
    }
    return ~crc;
}

/* --- end of the shared part -------------------------------------------- */

#if defined(THEORA_PLANE_DIGEST_SELFTEST) && THEORA_PLANE_DIGEST_SELFTEST

static int selftest_check(const char *what, uint32_t got, uint32_t want)
{
    if (got == want)
        return 0;
    fprintf(stderr, "theora_plane_digest selftest: %s = %08x, want %08x\n",
            what, (unsigned)got, (unsigned)want);
    return 1;
}

int main(void)
{
    static uint32_t table[256];
    /* Ten bytes with the NUL: a nine-element array would drop it, which GCC
     * >= 15's -Wextra (-Wunterminated-string-initialization) rejects under
     * -Werror; every use below passes the width 9 explicitly. */
    static const unsigned char check[] = "123456789";
    unsigned char strided[3 * 5];
    unsigned char packed[3 * 3];
    int failures = 0;
    int y;

    plane_digest_crc32_init(table);
    /* Known IEEE 802.3 table entries and the standard check value. */
    failures += selftest_check("table[0]", table[0], 0x00000000U);
    failures += selftest_check("table[1]", table[1], 0x77073096U);
    failures += selftest_check("table[255]", table[255], 0x2D02EF8DU);
    failures += selftest_check("crc32(\"123456789\")",
                               plane_digest_crc32(table, check, 9, 1, 9),
                               0xCBF43926U);
    /* The same nine bytes as three rows of three with two bytes of junk
     * padding per row: the padding must not take part. */
    memset(strided, 0xA5, sizeof strided);
    for (y = 0; y < 3; ++y)
        memcpy(strided + y * 5, check + y * 3, 3U);
    failures += selftest_check("crc32(3x3 stride 5)",
                               plane_digest_crc32(table, strided, 3, 3, 5),
                               0xCBF43926U);
    memcpy(packed, check, sizeof packed);
    failures += selftest_check("crc32(3x3 stride 3)",
                               plane_digest_crc32(table, packed, 3, 3, 3),
                               0xCBF43926U);
    /* A single column through a stride reads one byte per row. */
    failures += selftest_check("crc32(1x3 stride 3 of \"147\")",
                               plane_digest_crc32(table, packed, 1, 3, 3),
                               plane_digest_crc32(table, (const unsigned char *)"147", 3, 1, 3));
    /* Empty planes digest to 0; NULL data too. */
    failures += selftest_check("crc32(empty)", plane_digest_crc32(table, check, 0, 1, 9), 0U);
    failures += selftest_check("crc32(no rows)", plane_digest_crc32(table, check, 9, 0, 9), 0U);
    failures += selftest_check("crc32(NULL)", plane_digest_crc32(table, NULL, 9, 1, 9), 0U);
    if (failures) {
        fprintf(stderr, "theora_plane_digest selftest: %d failure(s)\n", failures);
        return 1;
    }
    printf("theora_plane_digest selftest: OK (crc32 IEEE check value, "
           "row/stride handling, empty planes)\n");
    return 0;
}

#else  /* the oracle proper */

#include <ogg/ogg.h>
#include <theora/theoradec.h>

#define CHUNK 65536

int main(int argc, char **argv)
{
    static uint32_t table[256];
    const char *path;
    FILE *in;
    ogg_sync_state oy;
    ogg_page og;
    ogg_packet op;
    ogg_stream_state to;
    th_info ti;
    th_comment tc;
    th_setup_info *ts = NULL;
    th_dec_ctx *td = NULL;
    int have_theora = 0;
    int theora_serial = 0;
    int headers = 0;
    int eof = 0;
    int status = 0;
    unsigned packet = 0U, dups = 0U, errors = 0U, holes = 0U, pages = 0U;
    unsigned skipped_streams = 0U;

    if (argc != 2 || argv[1][0] == '-') {
        fprintf(stderr, "usage: theora_plane_digest <clip.ogv>  (archive: %s)\n",
                th_version_string());
        return 2;
    }
    path = argv[1];
    in = fopen(path, "rb");
    if (!in) {
        fprintf(stderr, "theora_plane_digest: cannot open %s\n", path);
        return 2;
    }
    plane_digest_crc32_init(table);
    ogg_sync_init(&oy);
    th_info_init(&ti);
    th_comment_init(&tc);
    memset(&to, 0, sizeof to);
    printf("# theora_plane_digest archive='%s' file='%s'\n", th_version_string(), path);

    while (!eof && !status) {
        char *buffer = ogg_sync_buffer(&oy, CHUNK);
        size_t got;
        int sync;

        if (!buffer) {
            fprintf(stderr, "theora_plane_digest: ogg_sync_buffer failed\n");
            status = 3;
            break;
        }
        got = fread(buffer, 1U, (size_t)CHUNK, in);
        if (got == 0U)
            eof = 1;
        else
            ogg_sync_wrote(&oy, (long)got);
        /* Drain every page the sync layer has: -1 is a hole (bytes skipped
         * to the next capture pattern), after which more pages may follow
         * in the same buffer, so only 0 (need more data) ends the loop. */
        while (!status && (sync = ogg_sync_pageout(&oy, &og)) != 0) {
            if (sync < 0) {
                ++holes;
                continue;
            }
            ++pages;
            if (ogg_page_bos(&og)) {
                /* theoraplayer: a fresh stream state per BOS page; the one
                 * whose first packet th_decode_headerin accepts becomes the
                 * video stream (its state is moved, not cleared), the rest
                 * are dropped.  Only the first video stream counts. */
                ogg_stream_state test;

                ogg_stream_init(&test, ogg_page_serialno(&og));
                ogg_stream_pagein(&test, &og);
                if (!have_theora && ogg_stream_packetout(&test, &op) == 1 &&
                    th_decode_headerin(&ti, &tc, &ts, &op) > 0) {
                    memcpy(&to, &test, sizeof to);
                    have_theora = 1;
                    headers = 1;
                    theora_serial = ogg_page_serialno(&og);
                } else {
                    ogg_stream_clear(&test);
                    ++skipped_streams;
                }
                continue;
            }
            if (!have_theora || ogg_page_serialno(&og) != theora_serial)
                continue;
            if (ogg_stream_pagein(&to, &og) < 0) {
                ++holes;
                continue;
            }
            for (;;) {
                int r = ogg_stream_packetout(&to, &op);

                if (r == 0)
                    break;
                if (r < 0) {
                    ++holes;   /* a gap in the stream; libogg resynchronises */
                    continue;
                }
                if (headers < 3) {
                    r = th_decode_headerin(&ti, &tc, &ts, &op);
                    if (r <= 0) {
                        fprintf(stderr, "theora_plane_digest: header packet %d: "
                                "th_decode_headerin=%d\n", headers, r);
                        status = 4;
                        break;
                    }
                    ++headers;
                    if (headers == 3) {
                        td = th_decode_alloc(&ti, ts);
                        th_setup_free(ts);
                        ts = NULL;
                        if (!td) {
                            fprintf(stderr, "theora_plane_digest: th_decode_alloc failed\n");
                            status = 5;
                            break;
                        }
                        fprintf(stderr, "theora_plane_digest: archive='%s' version=%u.%u.%u "
                                "frame=%ux%u pic=%ux%u+%u,%u fps=%u/%u fmt=%d colorspace=%d "
                                "shift=%d\n",
                                th_version_string(), (unsigned)ti.version_major,
                                (unsigned)ti.version_minor, (unsigned)ti.version_subminor,
                                (unsigned)ti.frame_width, (unsigned)ti.frame_height,
                                (unsigned)ti.pic_width, (unsigned)ti.pic_height,
                                (unsigned)ti.pic_x, (unsigned)ti.pic_y,
                                (unsigned)ti.fps_numerator, (unsigned)ti.fps_denominator,
                                (int)ti.pixel_fmt, (int)ti.colorspace,
                                ti.keyframe_granule_shift);
                    }
                    continue;
                }
                /* A video packet: decode, then always read the planes back. */
                {
                    ogg_int64_t gp = -1;
                    int ret = th_decode_packetin(td, &op, &gp);

                    if (ret == 0 || ret == TH_DUPFRAME) {
                        th_ycbcr_buffer planes;
                        int out = th_decode_ycbcr_out(td, planes);

                        if (out != 0) {
                            fprintf(stderr, "theora_plane_digest: packet=%u ret=%d "
                                    "th_decode_ycbcr_out=%d\n", packet, ret, out);
                            ++errors;
                        } else {
                            printf("packet=%u ret=%d gp=%lld y=%08x cb=%08x cr=%08x\n",
                                   packet, ret, (long long)gp,
                                   (unsigned)plane_digest_crc32(table, planes[0].data,
                                                                planes[0].width,
                                                                planes[0].height,
                                                                planes[0].stride),
                                   (unsigned)plane_digest_crc32(table, planes[1].data,
                                                                planes[1].width,
                                                                planes[1].height,
                                                                planes[1].stride),
                                   (unsigned)plane_digest_crc32(table, planes[2].data,
                                                                planes[2].width,
                                                                planes[2].height,
                                                                planes[2].stride));
                        }
                        if (ret == TH_DUPFRAME)
                            ++dups;
                    } else {
                        fprintf(stderr, "theora_plane_digest: packet=%u bytes=%ld "
                                "th_decode_packetin=%d\n", packet, op.bytes, ret);
                        ++errors;
                    }
                    ++packet;
                }
            }
        }
    }
    if (!status && !have_theora) {
        fprintf(stderr, "theora_plane_digest: no Theora stream in %s\n", path);
        status = 6;
    } else if (!status && headers < 3) {
        fprintf(stderr, "theora_plane_digest: only %d of 3 Theora headers in %s\n",
                headers, path);
        status = 7;
    }
    fprintf(stderr, "theora_plane_digest: %s archive='%s' packets=%u dup=%u err=%u "
            "holes=%u pages=%u streams_skipped=%u status=%d\n",
            path, th_version_string(), packet, dups, errors, holes, pages,
            skipped_streams, status);
    if (td)
        th_decode_free(td);
    if (ts)
        th_setup_free(ts);
    th_comment_clear(&tc);
    th_info_clear(&ti);
    if (have_theora)
        ogg_stream_clear(&to);
    ogg_sync_clear(&oy);
    fclose(in);
    return status;
}

#endif
