/* Huffman table memo for the native archive inflater.  See the header for
 * the design and the exactness argument.  Compiled into the eboot only when
 * ISAAC_VITA_INFLATE_TABLE_MEMO is ON and, from the same source, into the host
 * oracle (recomp/test_vita_inflate_table_memo.py) that runs the memoised
 * decoder against the untouched one over every block of a real archive. */
#include "host_vita_inflate_table_memo.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if !defined(ISAAC_VITA_INFLATE_TABLE_MEMO) || !ISAAC_VITA_INFLATE_TABLE_MEMO
#error The inflate table memo must only be compiled for its opt-in feature
#endif

#ifndef ISAAC_VITA_INFLATE_TABLE_MEMO_SLOTS
#define ISAAC_VITA_INFLATE_TABLE_MEMO_SLOTS 16
#endif
#if ISAAC_VITA_INFLATE_TABLE_MEMO_SLOTS < 1 || \
    ISAAC_VITA_INFLATE_TABLE_MEMO_SLOTS > 256
#error ISAAC_VITA_INFLATE_TABLE_MEMO_SLOTS must be in 1..256
#endif
#ifndef ISAAC_VITA_INFLATE_TABLE_MEMO_CALIBRATE
#define ISAAC_VITA_INFLATE_TABLE_MEMO_CALIBRATE 64U
#endif
/* Host measurement builds raise this so every dynamic build is timed. */
#ifndef ISAAC_VITA_INFLATE_TABLE_MEMO_DYN_PROBATION
#define ISAAC_VITA_INFLATE_TABLE_MEMO_DYN_PROBATION 4096U
#endif

/* recomp/vita/platform.h; declared here so the host oracle can stub it. */
void isaac_vita_log(const char *format, ...);

#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
static uint64_t memo_now_ns(void)
{
    return sceKernelGetProcessTimeWide() * 1000ULL;
}
#elif defined(_WIN32)
#include <windows.h>
static uint64_t memo_now_ns(void)
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
static uint64_t memo_now_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}
#endif

enum {
    MEMO_SLOTS = ISAAC_VITA_INFLATE_TABLE_MEMO_SLOTS,
    MEMO_SEEN = 256,
    MEMO_RECEIPT_HEAD = 8U,
    MEMO_RECEIPT_PERIOD = 256U,
    /* Dynamic probation: after this many dynamic lookups the hit rate must
     * be at least 1/64 (about the break-even of one fingerprint hash per
     * block against one skipped build per hit), else dynamic lookups stop
     * for the rest of the run.  The shipped 1.7.9b archives repeat almost
     * none of their dynamic headers (afterbirthp.a 3 recurring of the first
     * 4096, afterbirth.a 1, repentance.a 3.4 % overall with the repeats
     * clustered), so afterbirth(p).a leave probation with the memo off and
     * pay nothing further; a repacked static archive has almost no dynamic
     * blocks and never completes probation.  (MEMO_DYN_PROBATION and
     * MEMO_DYN_MIN_HITS are macros below: the measurement override does not
     * fit an int enumerator.) */
    MEMO_KIND_NONE = 0,
    MEMO_KIND_FIXED = 1,
    MEMO_KIND_DYNAMIC = 2
};
#define MEMO_DYN_PROBATION \
    ((uint32_t)ISAAC_VITA_INFLATE_TABLE_MEMO_DYN_PROBATION)
#define MEMO_DYN_MIN_HITS (MEMO_DYN_PROBATION / 64U)

typedef struct memo_slot {
    uint32_t hash;
    uint32_t live;
    uint32_t sizes[2];
    uint8_t key[ISAAC_VITA_INFLATE_MEMO_KEY_MAX];
    uint8_t built[2][ISAAC_VITA_INFLATE_MEMO_BUILT_BYTES];
} memo_slot;

static isaac_vita_inflate_memo_stats s_stats;
static memo_slot s_slots[MEMO_SLOTS];
static uint32_t s_seen[MEMO_SEEN];
static uint8_t s_fixed_built[2][ISAAC_VITA_INFLATE_MEMO_BUILT_BYTES];
static uint32_t s_fixed_ready;
static uint32_t s_dyn_lookups;
/* Set by a lookup miss, consumed by the following store. */
static uint32_t s_pending_kind;
static uint32_t s_pending_hash;
static uint32_t s_pending_slot;
static uint64_t s_pending_t0;

#if defined(ISAAC_VITA_INFLATE_TABLE_MEMO_ORACLE)
/* Host oracle census hook: every lookup reports the exact key it hashed and
 * whether it hit.  Absent from the eboot. */
void (*isaac_vita_inflate_memo_oracle_observe)(
    uint32_t block_type, const uint32_t sizes[2], const uint8_t *lit_codes,
    const uint8_t *dist_codes, int hit);
#define MEMO_OBSERVE(type, sizes, tables, hit) do { \
    if (isaac_vita_inflate_memo_oracle_observe) \
        isaac_vita_inflate_memo_oracle_observe( \
            (type), (sizes), memo_code_size((tables), 0U), \
            memo_code_size((tables), 1U), (hit)); \
} while (0)
#else
#define MEMO_OBSERVE(type, sizes, tables, hit) ((void)0)
#endif

static const uint8_t *memo_built(const uint8_t *tables, uint32_t index)
{
    return tables + index * ISAAC_VITA_INFLATE_MEMO_TABLE_BYTES +
        ISAAC_VITA_INFLATE_MEMO_CODE_SIZE_BYTES;
}

static uint8_t *memo_built_mut(uint8_t *tables, uint32_t index)
{
    return tables + index * ISAAC_VITA_INFLATE_MEMO_TABLE_BYTES +
        ISAAC_VITA_INFLATE_MEMO_CODE_SIZE_BYTES;
}

static const uint8_t *memo_code_size(const uint8_t *tables, uint32_t index)
{
    return tables + index * ISAAC_VITA_INFLATE_MEMO_TABLE_BYTES;
}

static uint32_t memo_mix(uint32_t hash, uint32_t word)
{
    hash ^= word;
    hash *= 0x9e3779b1U;
    hash ^= hash >> 15;
    return hash;
}

static uint32_t memo_load32(const uint8_t *bytes)
{
    uint32_t word;
    memcpy(&word, bytes, 4U);
    return word;
}

/* Cheap fingerprint of the header; exactness comes from memo_key_equal's
 * full compare, the hash only selects a slot and feeds the seen filter.
 * Mixes both table sizes, the block's 19 code-length code lengths (tables[2],
 * cleared and rewritten by every dynamic header), eight 4-byte samples spread
 * over the literal/length code sizes and the leading distance code sizes:
 * 15 multiply-xor steps instead of one per key word. */
static uint32_t memo_hash(const uint8_t *tables, const uint32_t sizes[2])
{
    const uint8_t *clen = memo_code_size(tables, 2U);
    const uint8_t *lit = memo_code_size(tables, 0U);
    const uint8_t *dist = memo_code_size(tables, 1U);
    uint32_t hash = memo_mix(0x811c9dc5U, sizes[0] | (sizes[1] << 16));
    uint32_t index;
    for (index = 0U; index < 16U; index += 4U)
        hash = memo_mix(hash, memo_load32(clen + index));
    hash = memo_mix(hash, (uint32_t)clen[16] | ((uint32_t)clen[17] << 8) |
                    ((uint32_t)clen[18] << 16));
    for (index = 0U; index < 8U; ++index)
        hash = memo_mix(hash, memo_load32(lit + (sizes[0] - 4U) * index / 7U));
    hash = memo_mix(hash, (uint32_t)dist[0] |
                    (sizes[1] > 1U ? (uint32_t)dist[1] << 8 : 0U) |
                    (sizes[1] > 2U ? (uint32_t)dist[2] << 16 : 0U) |
                    (sizes[1] > 3U ? (uint32_t)dist[3] << 24 : 0U));
    /* 0 is the empty seen-filter word. */
    return hash ? hash : 1U;
}

static int memo_key_equal(const memo_slot *slot, const uint8_t *tables,
                          const uint32_t sizes[2])
{
    if (!slot->live || slot->sizes[0] != sizes[0] ||
            slot->sizes[1] != sizes[1])
        return 0;
    return memcmp(slot->key, memo_code_size(tables, 0U), sizes[0]) == 0 &&
        memcmp(slot->key + sizes[0], memo_code_size(tables, 1U),
               sizes[1]) == 0;
}

static int memo_sizes_ok(const uint32_t sizes[2])
{
    return sizes[0] >= 257U && sizes[0] <= ISAAC_VITA_INFLATE_MEMO_LIT_MAX &&
        sizes[1] >= 1U && sizes[1] <= ISAAC_VITA_INFLATE_MEMO_DIST_MAX;
}

/* `built` addresses a [2][BUILT_BYTES] span (tables[0] then tables[1]);
 * flat pointers keep the const conversion legal in C11. */
static void memo_copy_built(uint8_t *tables, const uint8_t *built)
{
    memcpy(memo_built_mut(tables, 0U), built,
           ISAAC_VITA_INFLATE_MEMO_BUILT_BYTES);
    memcpy(memo_built_mut(tables, 1U),
           built + ISAAC_VITA_INFLATE_MEMO_BUILT_BYTES,
           ISAAC_VITA_INFLATE_MEMO_BUILT_BYTES);
}

static void memo_keep_built(uint8_t *built, const uint8_t *tables)
{
    memcpy(built, memo_built(tables, 0U),
           ISAAC_VITA_INFLATE_MEMO_BUILT_BYTES);
    memcpy(built + ISAAC_VITA_INFLATE_MEMO_BUILT_BYTES,
           memo_built(tables, 1U), ISAAC_VITA_INFLATE_MEMO_BUILT_BYTES);
}

static uint64_t memo_average(uint64_t sum, uint32_t samples)
{
    return samples ? sum / samples : 0U;
}

uint64_t isaac_vita_inflate_memo_saved_ns(void)
{
    uint64_t fixed_build = memo_average(s_stats.fixed_build_ns,
                                        s_stats.fixed_build_samples);
    uint64_t fixed_copy = memo_average(s_stats.fixed_copy_ns,
                                       s_stats.fixed_copy_samples);
    uint64_t dyn_build = memo_average(s_stats.dyn_build_ns,
                                      s_stats.dyn_build_samples);
    uint64_t dyn_copy = memo_average(s_stats.dyn_copy_ns,
                                     s_stats.dyn_copy_samples);
    uint64_t saved = 0U;
    /* Only one fixed build ever runs (cold); the dynamic build is the same
     * loop over about the same symbol count, so once it has warm samples it
     * is the better estimate of what each skipped fixed build would cost. */
    if (s_stats.fixed_build_samples < 4U && s_stats.dyn_build_samples >= 4U)
        fixed_build = dyn_build;
    if (fixed_build > fixed_copy)
        saved += (uint64_t)s_stats.fixed_hits * (fixed_build - fixed_copy);
    if (dyn_build > dyn_copy)
        saved += (uint64_t)s_stats.dyn_hits * (dyn_build - dyn_copy);
    return saved;
}

static void memo_receipt(void)
{
    ++s_stats.receipts;
    isaac_vita_log(
        "KAGE VITA INFLATE MEMO: blocks=%u fixed_hits=%u dyn_hits=%u "
        "dyn_miss=%u build_us_saved~=%u stored=%u fixed=%u dyn=%u "
        "dyn_stores=%u dyn_off=%u slots=%u cal(fb,fc,db,dc)=%u,%u,%u,%u",
        (unsigned)s_stats.blocks, (unsigned)s_stats.fixed_hits,
        (unsigned)s_stats.dyn_hits, (unsigned)s_stats.dyn_miss,
        (unsigned)(isaac_vita_inflate_memo_saved_ns() / 1000U),
        (unsigned)s_stats.stored, (unsigned)s_stats.fixed,
        (unsigned)s_stats.dynamic, (unsigned)s_stats.dyn_stores,
        (unsigned)s_stats.dyn_off, (unsigned)MEMO_SLOTS,
        (unsigned)memo_average(s_stats.fixed_build_ns,
                               s_stats.fixed_build_samples),
        (unsigned)memo_average(s_stats.fixed_copy_ns,
                               s_stats.fixed_copy_samples),
        (unsigned)memo_average(s_stats.dyn_build_ns,
                               s_stats.dyn_build_samples),
        (unsigned)memo_average(s_stats.dyn_copy_ns,
                               s_stats.dyn_copy_samples));
}

void isaac_vita_inflate_memo_note_block(uint32_t block_type)
{
    ++s_stats.blocks;
    if (block_type == ISAAC_VITA_INFLATE_MEMO_BLOCK_STORED)
        ++s_stats.stored;
    else if (block_type == ISAAC_VITA_INFLATE_MEMO_BLOCK_FIXED)
        ++s_stats.fixed;
    else if (block_type == ISAAC_VITA_INFLATE_MEMO_BLOCK_DYNAMIC)
        ++s_stats.dynamic;
    /* A new block header ends any build the previous block did not finish
     * (a failed code set never reaches the store). */
    s_pending_kind = MEMO_KIND_NONE;
    if (s_stats.blocks <= MEMO_RECEIPT_HEAD ||
            (s_stats.blocks % MEMO_RECEIPT_PERIOD) == 0U)
        memo_receipt();
}

int isaac_vita_inflate_memo_lookup(uint8_t *tables, uint32_t block_type,
                                   const uint32_t table_sizes[2])
{
    uint32_t hash;
    uint32_t slot_index;
    memo_slot *slot;

    s_pending_kind = MEMO_KIND_NONE;
    if (block_type == ISAAC_VITA_INFLATE_MEMO_BLOCK_FIXED) {
        if (s_fixed_ready) {
            if (s_stats.fixed_copy_samples <
                    ISAAC_VITA_INFLATE_TABLE_MEMO_CALIBRATE) {
                uint64_t t0 = memo_now_ns();
                memo_copy_built(tables, s_fixed_built[0]);
                s_stats.fixed_copy_ns += memo_now_ns() - t0;
                ++s_stats.fixed_copy_samples;
            } else {
                memo_copy_built(tables, s_fixed_built[0]);
            }
            ++s_stats.fixed_hits;
            MEMO_OBSERVE(block_type, table_sizes, tables, 1);
            return 1;
        }
        s_pending_kind = MEMO_KIND_FIXED;
        MEMO_OBSERVE(block_type, table_sizes, tables, 0);
        if (s_stats.fixed_build_samples <
                ISAAC_VITA_INFLATE_TABLE_MEMO_CALIBRATE)
            s_pending_t0 = memo_now_ns();
        return 0;
    }
    if (block_type != ISAAC_VITA_INFLATE_MEMO_BLOCK_DYNAMIC ||
            !memo_sizes_ok(table_sizes))
        return 0;
    if (s_stats.dyn_off) {
        ++s_stats.dyn_skipped;
        return 0;
    }
    if (++s_dyn_lookups == MEMO_DYN_PROBATION &&
            s_stats.dyn_hits < MEMO_DYN_MIN_HITS)
        s_stats.dyn_off = 1U;

    hash = memo_hash(tables, table_sizes);
    slot_index = hash % (uint32_t)MEMO_SLOTS;
    slot = &s_slots[slot_index];
    if (slot->hash == hash && memo_key_equal(slot, tables, table_sizes)) {
        if (s_stats.dyn_copy_samples <
                ISAAC_VITA_INFLATE_TABLE_MEMO_CALIBRATE) {
            uint64_t t0 = memo_now_ns();
            memo_copy_built(tables, slot->built[0]);
            s_stats.dyn_copy_ns += memo_now_ns() - t0;
            ++s_stats.dyn_copy_samples;
        } else {
            memo_copy_built(tables, slot->built[0]);
        }
        ++s_stats.dyn_hits;
        MEMO_OBSERVE(block_type, table_sizes, tables, 1);
        return 1;
    }
    ++s_stats.dyn_miss;
    MEMO_OBSERVE(block_type, table_sizes, tables, 0);
    s_pending_kind = MEMO_KIND_DYNAMIC;
    s_pending_hash = hash;
    s_pending_slot = slot_index;
    if (s_stats.dyn_build_samples < ISAAC_VITA_INFLATE_TABLE_MEMO_CALIBRATE)
        s_pending_t0 = memo_now_ns();
    return 0;
}

void isaac_vita_inflate_memo_store(const uint8_t *tables, uint32_t block_type,
                                   const uint32_t table_sizes[2])
{
    uint32_t kind = s_pending_kind;
    s_pending_kind = MEMO_KIND_NONE;

    if (kind == MEMO_KIND_FIXED &&
            block_type == ISAAC_VITA_INFLATE_MEMO_BLOCK_FIXED) {
        if (s_stats.fixed_build_samples <
                ISAAC_VITA_INFLATE_TABLE_MEMO_CALIBRATE) {
            s_stats.fixed_build_ns += memo_now_ns() - s_pending_t0;
            ++s_stats.fixed_build_samples;
        }
        memo_keep_built(s_fixed_built[0], tables);
        s_fixed_ready = 1U;
        ++s_stats.fixed_builds;
        return;
    }
    if (kind != MEMO_KIND_DYNAMIC ||
            block_type != ISAAC_VITA_INFLATE_MEMO_BLOCK_DYNAMIC ||
            !memo_sizes_ok(table_sizes))
        return;
    if (s_stats.dyn_build_samples < ISAAC_VITA_INFLATE_TABLE_MEMO_CALIBRATE) {
        s_stats.dyn_build_ns += memo_now_ns() - s_pending_t0;
        ++s_stats.dyn_build_samples;
    }
    /* Store on second sight: a header seen once costs only the hash and a
     * word in the seen filter; the 6.4 KiB copy into a slot is paid when the
     * header recurs, so single-use headers (the shipped archives) never
     * churn the slots. */
    {
        uint32_t *seen = &s_seen[s_pending_hash % (uint32_t)MEMO_SEEN];
        memo_slot *slot;
        if (*seen != s_pending_hash) {
            *seen = s_pending_hash;
            return;
        }
        slot = &s_slots[s_pending_slot];
        if (slot->live)
            ++s_stats.dyn_evictions;
        slot->hash = s_pending_hash;
        slot->sizes[0] = table_sizes[0];
        slot->sizes[1] = table_sizes[1];
        memcpy(slot->key, memo_code_size(tables, 0U), table_sizes[0]);
        memcpy(slot->key + table_sizes[0], memo_code_size(tables, 1U),
               table_sizes[1]);
        memo_keep_built(slot->built[0], tables);
        slot->live = 1U;
        ++s_stats.dyn_stores;
    }
}

const isaac_vita_inflate_memo_stats *isaac_vita_inflate_memo_stats_get(void)
{
    return &s_stats;
}

uint32_t isaac_vita_inflate_memo_slot_count(void)
{
    return (uint32_t)MEMO_SLOTS;
}

void isaac_vita_inflate_memo_reset(void)
{
    memset(&s_stats, 0, sizeof s_stats);
    memset(s_slots, 0, sizeof s_slots);
    memset(s_seen, 0, sizeof s_seen);
    memset(s_fixed_built, 0, sizeof s_fixed_built);
    s_fixed_ready = 0U;
    s_dyn_lookups = 0U;
    s_pending_kind = MEMO_KIND_NONE;
    s_pending_hash = 0U;
    s_pending_slot = 0U;
    s_pending_t0 = 0U;
}
