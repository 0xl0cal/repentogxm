/* Host oracle for ISAAC_VITA_INFLATE_TABLE_MEMO.
 *
 * Drives two copies of the archive tinfl decoder over the same pieces exactly
 * the way ArchivedFile (sub_0059c270) does: one coroutine state per entry, a
 * 0x400-byte wrapping output window, flags = HAS_MORE_INPUT unless the piece
 * is the entry's last, an entry switching to stored mode at the first
 * non-final piece of exactly 0x400 bytes.  After every call the memoised
 * decoder (host_vita_archive_miniz_native.c built with the knob) must match
 * the untouched reference (host_vita_inflate_table_memo_oracle_reference.c)
 * in status, consumed input, produced output bytes and the full 0x2af0-byte
 * coroutine state; both states start from the same pseudo-random garbage so a
 * hit can never be shown to depend on stale table bytes.
 *
 * Inputs: a mode-2 archive (ARCH000) or a FRAMES01 file written by
 * recomp/test_vita_inflate_table_memo.py.  Prints one census line (block
 * types, memo hits/misses, distinct dynamic headers, timings, first receipt).
 * Never part of the eboot. */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS 1
#elif !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_vita_archive_miniz_native.h"
#include "host_vita_inflate_table_memo.h"

#if defined(_WIN32)
#include <windows.h>
static uint64_t oracle_now_ns(void)
{
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if (frequency.QuadPart == 0)
        QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (uint64_t)((double)counter.QuadPart * 1e9 /
                      (double)frequency.QuadPart);
}
#else
#include <time.h>
static uint64_t oracle_now_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}
#endif

int isaac_vita_inflate_memo_oracle_reference(
    void *state, const uint8_t *input, uint32_t *input_size,
    uint8_t *output_start, uint8_t *output_next, uint32_t *output_size,
    uint32_t flags);

extern void (*isaac_vita_inflate_memo_oracle_observe)(
    uint32_t block_type, const uint32_t sizes[2], const uint8_t *lit_codes,
    const uint8_t *dist_codes, int hit);

enum {
    STATE_BYTES = ISAAC_VITA_ARCHIVE_MINIZ_STATE_BYTES,
    INPUT_MAX = ISAAC_VITA_ARCHIVE_MINIZ_INPUT_MAX,
    INPUT_BUFFER = 0x800,
    OUTPUT_BYTES = ISAAC_VITA_ARCHIVE_MINIZ_OUTPUT_BYTES,
    KEY_SET_BITS = 22,
    KEY_SET_SIZE = 1 << KEY_SET_BITS,
    RECEIPT_CAPACITY = 512
};

/* --- isaac_vita_log stub: capture receipts ------------------------------ */
static uint32_t s_receipts;
static char s_first_receipt[RECEIPT_CAPACITY];
static char s_last_receipt[RECEIPT_CAPACITY];

void isaac_vita_log(const char *format, ...)
{
    char line[RECEIPT_CAPACITY];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    if (s_receipts == 0U)
        memcpy(s_first_receipt, line, sizeof line);
    memcpy(s_last_receipt, line, sizeof line);
    ++s_receipts;
}

/* --- exact distinct-header census (64-bit FNV-1a over the exact key) ---- */
static uint64_t *s_dyn_keys;
static uint32_t s_dyn_distinct;
static uint32_t s_dyn_observed;
static uint32_t s_dyn_recurring;    /* observations whose key was seen before */
static uint32_t s_fixed_observed;
static uint32_t s_observed_hits;
static uint32_t s_observed_misses;

static uint64_t fnv64(uint64_t hash, const uint8_t *bytes, size_t count)
{
    size_t index;
    for (index = 0; index < count; ++index) {
        hash ^= bytes[index];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

static void observe(uint32_t block_type, const uint32_t sizes[2],
                    const uint8_t *lit_codes, const uint8_t *dist_codes,
                    int hit)
{
    uint64_t hash;
    uint32_t index;
    if (hit)
        ++s_observed_hits;
    else
        ++s_observed_misses;
    if (block_type == ISAAC_VITA_INFLATE_MEMO_BLOCK_FIXED) {
        ++s_fixed_observed;
        return;
    }
    ++s_dyn_observed;
    hash = 0xcbf29ce484222325ULL;
    hash = fnv64(hash, (const uint8_t *)sizes, 8U);
    hash = fnv64(hash, lit_codes, sizes[0]);
    hash = fnv64(hash, dist_codes, sizes[1]);
    if (hash == 0U)
        hash = 1U;
    index = (uint32_t)(hash >> (64 - KEY_SET_BITS));
    for (;;) {
        if (s_dyn_keys[index] == hash) {
            ++s_dyn_recurring;
            return;
        }
        if (s_dyn_keys[index] == 0U) {
            s_dyn_keys[index] = hash;
            ++s_dyn_distinct;
            return;
        }
        index = (index + 1U) & (KEY_SET_SIZE - 1U);
    }
}

/* --- input sources ------------------------------------------------------ */
typedef struct source {
    FILE *stream;
    int archive;                 /* 1 = ARCH000 mode 2, 0 = FRAMES01 */
    uint32_t entry_count;
    uint32_t entry_index;
    /* archive index */
    uint32_t index_offset;
    /* frames file */
    uint32_t frames_left;
} source;

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static int source_open(source *src, const char *path)
{
    uint8_t header[14];
    memset(src, 0, sizeof *src);
    src->stream = fopen(path, "rb");
    if (!src->stream) {
        fprintf(stderr, "inflate memo oracle: cannot open %s\n", path);
        return 0;
    }
    if (fread(header, 1, sizeof header, src->stream) != sizeof header) {
        fprintf(stderr, "inflate memo oracle: short header\n");
        return 0;
    }
    if (memcmp(header, "ARCH000", 7) == 0) {
        if (header[7] != 2) {
            fprintf(stderr, "inflate memo oracle: archive mode %u is not "
                    "MiniZ mode 2\n", header[7]);
            return 0;
        }
        src->archive = 1;
        src->index_offset = read_le32(header + 8);
        src->entry_count = (uint32_t)header[12] | ((uint32_t)header[13] << 8);
        return 1;
    }
    if (memcmp(header, "FRAMES01", 8) == 0) {
        src->archive = 0;
        src->entry_count = read_le32(header + 8);
        fseek(src->stream, 12, SEEK_SET);
        return 1;
    }
    fprintf(stderr, "inflate memo oracle: unknown input signature\n");
    return 0;
}

/* Position the stream at entry `index`'s first frame; returns the expected
 * decoded length (archives) or 0. */
static int source_seek_entry(source *src, uint32_t index, uint32_t *expected)
{
    *expected = 0U;
    if (src->archive) {
        uint8_t row[20];
        if (fseek(src->stream, (long)(src->index_offset + index * 20U),
                  SEEK_SET) != 0 ||
                fread(row, 1, sizeof row, src->stream) != sizeof row)
            return 0;
        *expected = read_le32(row + 12);
        return fseek(src->stream, (long)read_le32(row + 8), SEEK_SET) == 0;
    }
    {
        uint8_t count[4];
        if (fread(count, 1, 4, src->stream) != 4)
            return 0;
        src->frames_left = read_le32(count);
        return 1;
    }
}

/* Next frame of the current entry into `payload`; 0 = no frame (frames file
 * exhausted its count).  `*last` is the entry's final-piece bit. */
static int source_next_frame(source *src, uint8_t *payload, uint32_t *length,
                             int *last)
{
    uint8_t header[4];
    uint32_t word;
    if (!src->archive) {
        if (src->frames_left == 0U)
            return 0;
        --src->frames_left;
    }
    if (fread(header, 1, 4, src->stream) != 4) {
        fprintf(stderr, "inflate memo oracle: truncated frame header\n");
        return -1;
    }
    word = read_le32(header);
    *length = word & 0x7fffffffU;
    *last = (word >> 31) != 0U;
    if (*length > INPUT_BUFFER) {
        fprintf(stderr, "inflate memo oracle: frame of %u bytes exceeds the "
                "frozen 0x800 buffer\n", (unsigned)*length);
        return -1;
    }
    if (fread(payload, 1, *length, src->stream) != *length) {
        fprintf(stderr, "inflate memo oracle: truncated frame\n");
        return -1;
    }
    return 1;
}

/* --- the differential ---------------------------------------------------- */
static uint32_t s_garbage_seed = 0x5aeb00U;

static uint32_t next_garbage(uint32_t *seed)
{
    *seed = *seed * 1664525U + 1013904223U;
    return *seed;
}

static void fill_garbage(uint8_t *bytes, size_t count, uint32_t seed)
{
    size_t index;
    for (index = 0; index < count; ++index)
        bytes[index] = (uint8_t)(next_garbage(&seed) >> 24);
}

static int report_mismatch(const char *what, uint32_t entry, uint32_t frame)
{
    fprintf(stderr, "inflate memo oracle: MISMATCH %s at entry %u frame %u\n",
            what, (unsigned)entry, (unsigned)frame);
    return 0;
}

static uint8_t s_state_ref[STATE_BYTES];
static uint8_t s_state_memo[STATE_BYTES];
static uint8_t s_input[INPUT_BUFFER];
static uint8_t s_output_ref[OUTPUT_BYTES];
static uint8_t s_output_memo[OUTPUT_BYTES];

typedef struct census {
    uint32_t entries;
    uint32_t compressed_entries;   /* entries with at least one tinfl piece */
    uint32_t stored_switch_entries;/* entries that switched to stored mode */
    uint32_t frames;               /* tinfl calls */
    uint32_t frames_multi_block;   /* frames with more than one Huffman block */
    uint32_t partial_input_frames; /* calls that did not consume every byte */
    uint32_t status_done;
    uint32_t status_more_input;
    uint32_t status_other;
    uint64_t decoded_bytes;
    uint64_t out_fnv64;
    uint64_t ref_ns;
    uint64_t memo_ns;
    uint32_t max_blocks_entry;
    uint32_t max_blocks_frame;
} census;

/* --bench: one decoder at a time over the whole source, both decoders using
 * the same static buffers, so the two totals see identical cache and
 * alignment conditions (the interleaved differential above is biased by up
 * to 10 % between its two buffer sets even for identical decoders). */
static int bench_entry(source *src, uint32_t entry, int which, uint64_t *ns,
                       uint32_t *frames)
{
    uint32_t expected;
    int compressed = 1;
    if (!source_seek_entry(src, entry, &expected))
        return 0;
    memset(s_state_ref, 0, 4);
    for (;;) {
        uint32_t length;
        int last;
        int next = source_next_frame(src, s_input, &length, &last);
        if (next < 0)
            return 0;
        if (next == 0)
            break;
        if (compressed && !(!last && length == OUTPUT_BYTES) &&
                length <= INPUT_MAX) {
            uint32_t in = length, out = OUTPUT_BYTES;
            uint32_t flags = last ? 0U : 2U;
            uint64_t t0 = oracle_now_ns();
            if (which == 1)
                (void)isaac_vita_inflate_memo_oracle_reference(
                    s_state_ref, s_input, &in, s_output_ref, s_output_ref,
                    &out, flags);
            else
                (void)isaac_vita_archive_miniz_native(
                    s_state_ref, s_input, &in, s_output_ref, s_output_ref,
                    &out, flags);
            *ns += oracle_now_ns() - t0;
            ++*frames;
        } else if (compressed) {
            compressed = 0;
        }
        if (last)
            break;
    }
    return 1;
}

static int run_entry(source *src, uint32_t entry, census *c, uint32_t limit_frames)
{
    uint32_t expected;
    uint32_t frame = 0;
    int compressed = 1;
    int had_piece = 0;
    uint32_t blocks_entry_start = isaac_vita_inflate_memo_stats_get()->blocks;
    uint32_t seed;

    if (!source_seek_entry(src, entry, &expected)) {
        fprintf(stderr, "inflate memo oracle: cannot seek entry %u\n",
                (unsigned)entry);
        return 0;
    }
    seed = next_garbage(&s_garbage_seed);
    fill_garbage(s_state_ref, sizeof s_state_ref, seed);
    memcpy(s_state_memo, s_state_ref, sizeof s_state_memo);
    memset(s_state_ref, 0, 4);      /* tinfl_init: r->state = 0 */
    memset(s_state_memo, 0, 4);
    fill_garbage(s_output_ref, sizeof s_output_ref, seed ^ 0x55555555U);
    memcpy(s_output_memo, s_output_ref, sizeof s_output_memo);

    for (;;) {
        uint32_t length;
        int last;
        int next = source_next_frame(src, s_input, &length, &last);
        if (next < 0)
            return 0;
        if (next == 0)
            break;
        if (compressed && !(!last && length == OUTPUT_BYTES) &&
                length <= INPUT_MAX) {
            uint32_t in_ref = length, in_memo = length;
            uint32_t out_ref = OUTPUT_BYTES, out_memo = OUTPUT_BYTES;
            uint32_t flags = last ? 0U : 2U;
            const isaac_vita_inflate_memo_stats *before =
                isaac_vita_inflate_memo_stats_get();
            uint32_t blocks_before = before->blocks;
            uint32_t huffman_before = before->fixed + before->dynamic;
            uint32_t blocks_frame;
            int status_ref, status_memo;
            uint64_t t0, t1, t2;

            if (limit_frames && c->frames >= limit_frames)
                return 1;
            if ((frame & 1U) == 0U) {
                t0 = oracle_now_ns();
                status_ref = isaac_vita_inflate_memo_oracle_reference(
                    s_state_ref, s_input, &in_ref, s_output_ref,
                    s_output_ref, &out_ref, flags);
                t1 = oracle_now_ns();
                status_memo = isaac_vita_archive_miniz_native(
                    s_state_memo, s_input, &in_memo, s_output_memo,
                    s_output_memo, &out_memo, flags);
                t2 = oracle_now_ns();
                c->ref_ns += t1 - t0;
                c->memo_ns += t2 - t1;
            } else {
                t0 = oracle_now_ns();
                status_memo = isaac_vita_archive_miniz_native(
                    s_state_memo, s_input, &in_memo, s_output_memo,
                    s_output_memo, &out_memo, flags);
                t1 = oracle_now_ns();
                status_ref = isaac_vita_inflate_memo_oracle_reference(
                    s_state_ref, s_input, &in_ref, s_output_ref,
                    s_output_ref, &out_ref, flags);
                t2 = oracle_now_ns();
                c->memo_ns += t1 - t0;
                c->ref_ns += t2 - t1;
            }
            ++c->frames;
            ++frame;
            had_piece = 1;
            if (status_ref != status_memo)
                return report_mismatch("status", entry, frame);
            if (in_ref != in_memo)
                return report_mismatch("consumed input", entry, frame);
            if (out_ref != out_memo)
                return report_mismatch("output size", entry, frame);
            if (out_ref > OUTPUT_BYTES)
                return report_mismatch("output overflow", entry, frame);
            if (memcmp(s_output_ref, s_output_memo, OUTPUT_BYTES) != 0)
                return report_mismatch("output window bytes", entry, frame);
            if (memcmp(s_state_ref, s_state_memo, STATE_BYTES) != 0)
                return report_mismatch("coroutine state", entry, frame);
            if (status_ref == ISAAC_VITA_ARCHIVE_MINIZ_DONE)
                ++c->status_done;
            else if (status_ref == ISAAC_VITA_ARCHIVE_MINIZ_NEEDS_MORE_INPUT)
                ++c->status_more_input;
            else
                ++c->status_other;
            if (in_ref != length)
                ++c->partial_input_frames;
            c->decoded_bytes += out_ref;
            c->out_fnv64 = fnv64(c->out_fnv64, s_output_ref, out_ref);
            blocks_frame = isaac_vita_inflate_memo_stats_get()->blocks -
                blocks_before;
            if (blocks_frame > c->max_blocks_frame)
                c->max_blocks_frame = blocks_frame;
            {
                const isaac_vita_inflate_memo_stats *after =
                    isaac_vita_inflate_memo_stats_get();
                if (after->fixed + after->dynamic - huffman_before > 1U)
                    ++c->frames_multi_block;
            }
        } else if (compressed) {
            compressed = 0;
            ++c->stored_switch_entries;
        }
        if (last)
            break;
    }
    ++c->entries;
    if (had_piece)
        ++c->compressed_entries;
    {
        uint32_t blocks_entry = isaac_vita_inflate_memo_stats_get()->blocks -
            blocks_entry_start;
        if (blocks_entry > c->max_blocks_entry)
            c->max_blocks_entry = blocks_entry;
    }
    (void)expected;
    return 1;
}

int main(int argc, char **argv)
{
    source src;
    census c;
    uint32_t limit_entries = 0U;
    uint32_t limit_frames = 0U;
    int census_on = 1;
    int bench_rounds = 0;
    const char *path = NULL;
    int index;
    const isaac_vita_inflate_memo_stats *s;

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--bench") == 0 && index + 1 < argc)
            bench_rounds = (int)strtol(argv[++index], NULL, 0);
        else if (strcmp(argv[index], "--limit-entries") == 0 && index + 1 < argc)
            limit_entries = (uint32_t)strtoul(argv[++index], NULL, 0);
        else if (strcmp(argv[index], "--limit-frames") == 0 && index + 1 < argc)
            limit_frames = (uint32_t)strtoul(argv[++index], NULL, 0);
        else if (strcmp(argv[index], "--seed") == 0 && index + 1 < argc)
            s_garbage_seed = (uint32_t)strtoul(argv[++index], NULL, 0);
        else if (strcmp(argv[index], "--no-census") == 0)
            census_on = 0;   /* timing runs: no observe hook on the memo side */
        else
            path = argv[index];
    }
    if (!path) {
        fprintf(stderr, "usage: inflate_memo_oracle [--limit-entries N] "
                "[--limit-frames N] [--seed S] [--no-census] "
                "<archive.a | frames.bin>\n");
        return 2;
    }
    if (!source_open(&src, path))
        return 1;
    s_dyn_keys = (uint64_t *)calloc(KEY_SET_SIZE, sizeof(uint64_t));
    if (!s_dyn_keys)
        return 1;
    isaac_vita_inflate_memo_reset();
    isaac_vita_inflate_memo_oracle_observe = census_on ? observe : NULL;
    if (bench_rounds) {
        int round;
        isaac_vita_inflate_memo_oracle_observe = NULL;
        for (round = 0; round < bench_rounds; ++round) {
            int which;
            for (which = 1; which <= 2; ++which) {
                uint64_t ns = 0U;
                uint32_t frames = 0U;
                isaac_vita_inflate_memo_reset();
                for (index = 0; (uint32_t)index < src.entry_count; ++index) {
                    if (limit_entries && (uint32_t)index >= limit_entries)
                        break;
                    if (!bench_entry(&src, (uint32_t)index, which, &ns, &frames))
                        return 1;
                }
                s = isaac_vita_inflate_memo_stats_get();
                printf("bench round=%d decoder=%s frames=%u ns=%" PRIu64
                       " ns_per_frame=%" PRIu64 " fixed_hits=%u dyn_hits=%u "
                       "dyn_off=%u\n",
                       round, which == 1 ? "reference" : "memo",
                       (unsigned)frames, ns, frames ? ns / frames : 0U,
                       (unsigned)s->fixed_hits, (unsigned)s->dyn_hits,
                       (unsigned)s->dyn_off);
            }
        }
        fclose(src.stream);
        free(s_dyn_keys);
        return 0;
    }
    memset(&c, 0, sizeof c);
    c.out_fnv64 = 0xcbf29ce484222325ULL;

    for (index = 0; (uint32_t)index < src.entry_count; ++index) {
        if (limit_entries && (uint32_t)index >= limit_entries)
            break;
        if (!run_entry(&src, (uint32_t)index, &c, limit_frames))
            return 1;
        if (limit_frames && c.frames >= limit_frames)
            break;
    }
    fclose(src.stream);
    s = isaac_vita_inflate_memo_stats_get();
    printf("inflate memo oracle: PASS; source=%s entries=%u compressed=%u "
           "stored_switch=%u frames=%u multi_block_frames=%u partial_input=%u "
           "status(done,more,other)="
           "%u,%u,%u decoded=%" PRIu64 " out_fnv64=%016" PRIx64 " "
           "blocks=%u stored=%u fixed=%u dyn=%u max_blocks(entry,frame)=%u,%u "
           "fixed_hits=%u fixed_builds=%u dyn_hits=%u dyn_miss=%u dyn_stores=%u "
           "dyn_evictions=%u dyn_skipped=%u dyn_off=%u dyn_distinct=%u "
           "dyn_recurring=%u observed(hit,miss)=%u,%u slots=%u "
           "cal(fb,fc,db,dc)=%" PRIu64 "/%u,%" PRIu64 "/%u,%" PRIu64 "/%u,%" PRIu64
           "/%u saved_ns=%" PRIu64 " ref_ns=%" PRIu64 " memo_ns=%" PRIu64 " "
           "receipts=%u\n",
           src.archive ? "archive" : "frames",
           (unsigned)c.entries, (unsigned)c.compressed_entries,
           (unsigned)c.stored_switch_entries, (unsigned)c.frames,
           (unsigned)c.frames_multi_block,
           (unsigned)c.partial_input_frames, (unsigned)c.status_done,
           (unsigned)c.status_more_input, (unsigned)c.status_other,
           c.decoded_bytes, c.out_fnv64,
           (unsigned)s->blocks, (unsigned)s->stored, (unsigned)s->fixed,
           (unsigned)s->dynamic, (unsigned)c.max_blocks_entry,
           (unsigned)c.max_blocks_frame,
           (unsigned)s->fixed_hits, (unsigned)s->fixed_builds,
           (unsigned)s->dyn_hits, (unsigned)s->dyn_miss,
           (unsigned)s->dyn_stores, (unsigned)s->dyn_evictions,
           (unsigned)s->dyn_skipped, (unsigned)s->dyn_off,
           (unsigned)s_dyn_distinct, (unsigned)s_dyn_recurring,
           (unsigned)s_observed_hits, (unsigned)s_observed_misses,
           (unsigned)isaac_vita_inflate_memo_slot_count(),
           s->fixed_build_ns, (unsigned)s->fixed_build_samples,
           s->fixed_copy_ns, (unsigned)s->fixed_copy_samples,
           s->dyn_build_ns, (unsigned)s->dyn_build_samples,
           s->dyn_copy_ns, (unsigned)s->dyn_copy_samples,
           isaac_vita_inflate_memo_saved_ns(), c.ref_ns, c.memo_ns,
           (unsigned)s_receipts);
    printf("first receipt: %s\n", s_first_receipt);
    printf("last receipt: %s\n", s_last_receipt);
    free(s_dyn_keys);
    return 0;
}
