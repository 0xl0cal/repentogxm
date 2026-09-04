/* Byte-fidelity oracle for the asynchronous save writer image.
 *
 * The device defect report claimed the writer's image was "semantically
 * wrong" for a 65,943-byte gamestate1.dat.  The frozen writer
 * (sub_00521a00, GameState::Save) is a pure sequential stream -- a 16-byte
 * header fwrite (not checksummed), thousands of 1/2/4-byte field writes and
 * array writes, then a trailing 4-byte checksum -- and a serializer callee
 * may length-patch with a seek-back.  This oracle replays that exact call
 * shape for a 65,943-byte body through the memory image AND through a genuine
 * newlib FILE (fopen "wb" + the same fwrite/fseek sequence + fclose) and
 * asserts the two files are byte-identical.  A faithful image is a necessary
 * condition for the game to accept the save: the trailing checksum the guest
 * writes is computed over the same bytes the image records, so any dropped,
 * duplicated or reordered byte would either change the length or break the
 * on-disk checksum self-consistency -- both of which this test would catch.
 *
 * Native I/O is a real temporary file on both sides (tmpfile()), so the
 * comparison is against actual newlib fwrite/fseek semantics, including the
 * sparse zero fill produced by seeking past end of file before writing. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_vita_async_write.h"

/* ---- native worker double: the queued image is written to a real FILE ---- */
static FILE *s_worker_fp;
static FILE *s_worker_result;

int32_t isaac_vita_async_write_oracle_open(const char *path)
{
    (void)path;
    s_worker_fp = tmpfile();
    return s_worker_fp ? 7 : -1;
}
int32_t isaac_vita_async_write_oracle_write(int32_t d, const void *b, uint32_t n)
{
    if (d != 7 || !s_worker_fp) return -1;
    return (int32_t)fwrite(b, 1u, n, s_worker_fp);
}
int32_t isaac_vita_async_write_oracle_close(int32_t d)
{
    (void)d;
    if (!s_worker_fp) return -1;
    fflush(s_worker_fp);
    s_worker_result = s_worker_fp;   /* keep the stream open to read it back */
    s_worker_fp = NULL;
    return 0;
}
static uint8_t s_arena[ISAAC_VITA_ASYNC_WRITE_ARENA_BYTES];
uint8_t *isaac_vita_async_write_oracle_arena(void) { return s_arena; }
static uint64_t s_clock;
uint64_t isaac_vita_async_write_oracle_time_us(void) { return (s_clock += 100u); }
int isaac_vita_async_write_oracle_worker_start(void) { return 1; }
void isaac_vita_log(const char *fmt, ...) { (void)fmt; }

/* ---- one write program applied identically to both sinks ---------------- */
typedef struct { int op; long a; long b; uint32_t off; } wop;
enum { W_WRITE, W_SEEK_SET, W_SEEK_END, W_SEEK_CUR };

static uint8_t s_src[140000];
static wop s_prog[40000];
static uint32_t s_prog_n;

static void emit_write(uint32_t off, uint32_t size, uint32_t count)
{
    s_prog[s_prog_n].op = W_WRITE;
    s_prog[s_prog_n].a = size;
    s_prog[s_prog_n].b = count;
    s_prog[s_prog_n].off = off;
    ++s_prog_n;
}
static void emit_seek(int op, long off)
{
    s_prog[s_prog_n].op = op;
    s_prog[s_prog_n].a = off;
    ++s_prog_n;
}

static uint32_t build_program(uint32_t total_target)
{
    uint32_t off = 0;
    uint32_t i;

    for (i = 0; i < sizeof s_src; ++i)
        s_src[i] = (uint8_t)(i * 131u + 7u);

    emit_write(off, 16, 1); off += 16;                       /* header */
    {
        uint32_t len_field_off = off;
        emit_write(off, 1, 4); off += 4;                     /* length field */

        uint32_t written = 20;
        unsigned pat = 0;
        while (written + 64u < total_target - 64u) {
            switch (pat++ % 6u) {
            case 0: emit_write(off, 4, 1); off += 4; written += 4; break;
            case 1: emit_write(off, 1, 1); off += 1; written += 1; break;
            case 2: emit_write(off, 2, 1); off += 2; written += 2; break;
            case 3: emit_write(off, 1, 4); off += 4; written += 4; break;
            case 4: emit_write(off, 1, 37); off += 37; written += 37; break;
            default: emit_write(off, 1, 100); off += 100; written += 100; break;
            }
        }
        /* interior length patch: seek back, overwrite 4 bytes, seek to end */
        emit_seek(W_SEEK_SET, (long)len_field_off);
        emit_write(off, 1, 4); off += 4;
        emit_seek(W_SEEK_END, 0);
        /* seek 8 past EOF then write 8 -> newlib sparse zero hole of 8 */
        emit_seek(W_SEEK_CUR, 8);
        written += 8;
        emit_write(off, 1, 8); off += 8; written += 8;
        while (written < total_target - 4u) {
            uint32_t chunk = total_target - 4u - written;
            if (chunk > 250u) chunk = 250u;
            emit_write(off, 1, chunk); off += chunk; written += chunk;
        }
        emit_write(off, 4, 1); off += 4; written += 4;       /* trailing sum */
        return written;
    }
}

static uint32_t read_back(FILE *fp, uint8_t *out, uint32_t cap)
{
    uint32_t n;
    if (!fp) return 0xffffffffu;
    fflush(fp);
    rewind(fp);
    n = (uint32_t)fread(out, 1u, cap, fp);
    return n;
}

static uint32_t run_native(uint8_t *out, uint32_t cap)
{
    FILE *f = tmpfile();
    uint32_t i;
    if (!f) return 0xffffffffu;
    for (i = 0; i < s_prog_n; ++i) {
        wop *w = &s_prog[i];
        if (w->op == W_WRITE) {
            if (fwrite(s_src + w->off, (size_t)w->a, (size_t)w->b, f) !=
                (size_t)w->b) { fclose(f); return 0xfffffffeu; }
        } else {
            int origin = w->op == W_SEEK_SET ? SEEK_SET
                       : w->op == W_SEEK_END ? SEEK_END : SEEK_CUR;
            if (fseek(f, w->a, origin)) { fclose(f); return 0xfffffffdu; }
        }
    }
    {
        uint32_t n = read_back(f, out, cap);
        fclose(f);
        return n;
    }
}

static uint32_t run_image(uint8_t *out, uint32_t cap)
{
    isaac_vita_async_write_image img;
    const char *P = "ux0:/data/isaacr001/Documents/gamestate1.dat";
    uint32_t i, n;
    if (!isaac_vita_async_write_image_open(&img, P)) return 0xffffffffu;
    for (i = 0; i < s_prog_n; ++i) {
        wop *w = &s_prog[i];
        if (w->op == W_WRITE) {
            if (isaac_vita_async_write_image_write(
                    &img, s_src + w->off, (size_t)w->a, (size_t)w->b) !=
                (size_t)w->b) return 0xfffffffeu;
        } else {
            int origin = w->op == W_SEEK_SET ? SEEK_SET
                       : w->op == W_SEEK_END ? SEEK_END : SEEK_CUR;
            if (isaac_vita_async_write_image_seek(&img, w->a, origin))
                return 0xfffffffdu;
        }
    }
    if (isaac_vita_async_write_image_close(&img) != 0) return 0xfffffffcu;
    if (isaac_vita_async_write_oracle_run_one() != 1) return 0xfffffffbu;
    n = read_back(s_worker_result, out, cap);
    if (s_worker_result) { fclose(s_worker_result); s_worker_result = NULL; }
    return n;
}

static uint8_t s_ref[140000];
static uint8_t s_got[140000];

static int check_size(uint32_t target)
{
    uint32_t n_ref, n_img, i;

    s_prog_n = 0;
    (void)build_program(target);
    n_ref = run_native(s_ref, sizeof s_ref);
    n_img = run_image(s_got, sizeof s_got);
    if (n_ref != target || n_img != target) {
        fprintf(stderr,
                "fidelity FAIL target=%u native=%u image=%u\n",
                target, n_ref, n_img);
        return 1;
    }
    for (i = 0; i < target; ++i) {
        if (s_ref[i] != s_got[i]) {
            fprintf(stderr,
                    "fidelity FAIL target=%u first diff at %u "
                    "native=%02x image=%02x\n",
                    target, i, s_ref[i], s_got[i]);
            return 1;
        }
    }
    printf("  %6u bytes: image byte-identical to native FILE (ops=%u)\n",
           target, s_prog_n);
    return 0;
}

int main(void)
{
    /* The rejected device save was 65,943 bytes; earlier accepted async saves
     * were 49-52 KiB.  Cover the reported size, the sizes that worked, and the
     * 64 KiB boundary the report suspected. */
    static const uint32_t sizes[] = {
        49152u, 52000u, 65535u, 65536u, 65537u, 65943u, 131072u
    };
    unsigned i;

    for (i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
        isaac_vita_async_write_oracle_reset();
        if (check_size(sizes[i]))
            return 1;
    }
    puts("Vita async save writer byte-fidelity oracle: PASS");
    return 0;
}
