/* Huffman table memo for the native archive inflater (ISAAC_VITA_INFLATE_TABLE_MEMO).
 *
 * The frozen archive reader (ArchivedFile, sub_0059c270) feeds the miniz
 * v1.15 coroutine (sub_005aeb00, native body in host_vita_archive_miniz_native.c)
 * one packed piece at a time.  The packer full-flushes every 0x400 output
 * bytes, so every kilobyte of a mode-2 entry starts a new deflate block and
 * miniz rebuilds its two decode tables (tables[0] literal/length, tables[1]
 * distance) from the block's code lengths: a memset of look_up (2 KiB) and
 * tree (1152 B), one bit-reversal loop per symbol and up to 1024 scattered
 * look_up stores per table.  Per decoded kilobyte that build is a large share
 * of the coroutine's time (see the round notes in the commit).
 *
 * The memo removes the build without changing what the build produces:
 *   (a) the fixed-Huffman tables (block type 1) are a constant; the first fixed
 *       block builds them in the guest state as before and the memo keeps a copy
 *       of the look_up/tree bytes the build wrote, which later fixed blocks
 *       receive by memcpy;
 *   (b) dynamic tables (block type 2) are keyed by their exact code-length
 *       header (HLIT, HDIST and every literal/length and distance code length);
 *       a header seen twice is stored, a header seen again is served by memcpy.
 * look_up and tree are a pure function of (table_sizes[t], code_size[0..size))
 * and the build clears both before writing, so a byte-equal key yields
 * byte-equal tables; the coroutine exits the build loop at the same state
 * value (type == UINT32_MAX) on both paths.  The code_size arrays and every
 * other coroutine field are written by the unchanged original code first.
 *
 * Every entry point here is inert unless the caller compiled the memo in; the
 * archive decoder's edits sit behind ISAAC_MINIZ_TABLE_MEMO and the PNG tinfl
 * entry (host_vita_native_png_miniz.c) never defines the knob.  The translated
 * runtime has one CPU writer, so plain statics suffice (same contract as the
 * native inflate seam's counters).
 */
#ifndef HOST_VITA_INFLATE_TABLE_MEMO_H
#define HOST_VITA_INFLATE_TABLE_MEMO_H

#include <stdint.h>

enum {
    /* miniz tinfl_huff_table: uint8 code_size[288]; int16 look_up[1024];
     * int16 tree[576].  The memo copies the two built arrays as one span. */
    ISAAC_VITA_INFLATE_MEMO_TABLE_BYTES = 0xda0U,
    ISAAC_VITA_INFLATE_MEMO_CODE_SIZE_BYTES = 288U,
    ISAAC_VITA_INFLATE_MEMO_BUILT_BYTES = 0xda0U - 288U,
    ISAAC_VITA_INFLATE_MEMO_LIT_MAX = 288U,
    ISAAC_VITA_INFLATE_MEMO_DIST_MAX = 32U,
    ISAAC_VITA_INFLATE_MEMO_KEY_MAX = 288U + 32U,
    ISAAC_VITA_INFLATE_MEMO_BLOCK_STORED = 0U,
    ISAAC_VITA_INFLATE_MEMO_BLOCK_FIXED = 1U,
    ISAAC_VITA_INFLATE_MEMO_BLOCK_DYNAMIC = 2U
};

typedef struct isaac_vita_inflate_memo_stats {
    uint32_t blocks;             /* deflate block headers read */
    uint32_t stored;             /* type 0 */
    uint32_t fixed;              /* type 1 */
    uint32_t dynamic;            /* type 2 */
    uint32_t fixed_hits;         /* fixed blocks served by the kept tables */
    uint32_t fixed_builds;       /* fixed blocks that ran the build (first one) */
    uint32_t dyn_hits;           /* dynamic blocks served by a memo slot */
    uint32_t dyn_miss;           /* dynamic blocks that ran the build */
    uint32_t dyn_stores;         /* misses whose tables were kept (2nd sight) */
    uint32_t dyn_evictions;      /* stores that replaced a live slot */
    uint32_t dyn_skipped;        /* dynamic blocks built without a lookup */
    uint32_t dyn_off;            /* 1 once the probation window found the
                                    dynamic hit rate below break-even */
    uint32_t receipts;           /* receipt lines logged */
    /* Calibration: the first ISAAC_VITA_INFLATE_TABLE_MEMO_CALIBRATE builds
     * and copies of each kind are timed; build_us_saved~= is derived from the
     * averages, never from a clock read on every block. */
    uint64_t fixed_build_ns;  uint32_t fixed_build_samples;
    uint64_t fixed_copy_ns;   uint32_t fixed_copy_samples;
    uint64_t dyn_build_ns;    uint32_t dyn_build_samples;
    uint64_t dyn_copy_ns;     uint32_t dyn_copy_samples;
} isaac_vita_inflate_memo_stats;

/* Block header read: census plus the bounded receipt (blocks 1..8, then every
 * 256th).  block_type is BTYPE (0 stored, 1 fixed, 2 dynamic, 3 reserved). */
void isaac_vita_inflate_memo_note_block(uint32_t block_type);

/* Called by the decoder at the top of its build loop when tables[0] and
 * tables[1] code_size/table_sizes are final and type == 1 (both still to be
 * built).  `tables` is r->tables (three consecutive tables).  Returns 1 when
 * both tables' look_up/tree were written from the memo (skip the build); 0
 * when the caller must build and then call the store below exactly once
 * unless the build fails. */
int isaac_vita_inflate_memo_lookup(uint8_t *tables, uint32_t block_type,
                                   const uint32_t table_sizes[2]);

/* After a build that followed a lookup miss: keep what the build produced. */
void isaac_vita_inflate_memo_store(const uint8_t *tables, uint32_t block_type,
                                   const uint32_t table_sizes[2]);

const isaac_vita_inflate_memo_stats *isaac_vita_inflate_memo_stats_get(void);
uint32_t isaac_vita_inflate_memo_slot_count(void);
uint64_t isaac_vita_inflate_memo_saved_ns(void);
void isaac_vita_inflate_memo_reset(void);

#endif /* HOST_VITA_INFLATE_TABLE_MEMO_H */
