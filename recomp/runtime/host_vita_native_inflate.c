/* Native zlib 1.1.4 inflate_codes seam.  See host_vita_native_inflate.h for
 * the design and the exactness argument.
 *
 * Compiled into the eboot when ISAAC_VITA_NATIVE_INFLATE is ON (with
 * ISAAC_VITA_NATIVE_INFLATE_WRAP=1 and the link's --wrap=sub_005d6d80) and,
 * from the same source, into the host oracles (ISAAC_NI_HOST_ORACLE) that
 * drive the guards on a fake CPU and run the seam end to end on ILP32 hosts. */
#include "host_vita_native_inflate.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "host_vita_native_inflate_hooks.h"

/* Vendored zlib 1.1.4 types for the in-place call (iz_ prefix via -include
 * izlib114_prefix.h; the struct layouts are pinned against the offsets in
 * host_vita_native_inflate_layout.h by host_vita_native_inflate_zlib114_codes.c). */
#include "zutil.h"
#include "infblock.h"
#include "inftrees.h"
#include "infcodes.h"
#include "infutil.h"

#if !defined(ISAAC_VITA_NATIVE_INFLATE)
#error The native inflate seam must only be compiled for its opt-in feature
#endif

/* recomp/vita/platform.h; declared here so the host oracles can stub it. */
void isaac_vita_log(const char *format, ...);

#ifndef ISAAC_VITA_NATIVE_INFLATE_BUILD_ID
#define ISAAC_VITA_NATIVE_INFLATE_BUILD_ID "native-inflate:unstamped"
#endif

#define NI_IMAGE_VA(rva)  ((uint32_t)GUEST_IMAGE_BASE + (uint32_t)(rva))

#if defined(ISAAC_NI_HOST_ORACLE)
uint32_t isaac_ni_oracle_adler32_va  = NI_IMAGE_VA(ISAAC_NI_ADLER32_RVA);
uint32_t isaac_ni_oracle_fixed_tl_va = NI_IMAGE_VA(ISAAC_NI_FIXED_TL_RVA);
uint32_t isaac_ni_oracle_fixed_td_va = NI_IMAGE_VA(ISAAC_NI_FIXED_TD_RVA);
#define NI_ADLER32_VA   isaac_ni_oracle_adler32_va
#define NI_FIXED_TL_VA  isaac_ni_oracle_fixed_tl_va
#define NI_FIXED_TD_VA  isaac_ni_oracle_fixed_td_va
#else
#define NI_ADLER32_VA   NI_IMAGE_VA(ISAAC_NI_ADLER32_RVA)
#define NI_FIXED_TL_VA  NI_IMAGE_VA(ISAAC_NI_FIXED_TL_RVA)
#define NI_FIXED_TD_VA  NI_IMAGE_VA(ISAAC_NI_FIXED_TD_RVA)
#endif

/* The translated caller pushes its return word as an RVA (guest_0180.c:
 * `GPUSH(0x5cf080U)`), exactly like every other `call` in the corpus and
 * like the PNG inflate_flush seam compares return_rva; only data/code
 * pointer immediates are relocated to image VAs.  Comparing against the
 * VA made every call fall back (reason=site, device 2026-09-04). */
#define NI_RETURN_WORD    ((uint32_t)ISAAC_NI_CODES_RETURN_RVA)
#define NI_MSG_LITLEN_VA  NI_IMAGE_VA(ISAAC_NI_MSG_BAD_LITLEN_RVA)
#define NI_MSG_DIST_VA    NI_IMAGE_VA(ISAAC_NI_MSG_BAD_DIST_RVA)
#define NI_HUFTS_BYTES    (ISAAC_NI_MANY * ISAAC_NI_HUFT_BYTES)
#define NI_FIXED_TL_BYTES (ISAAC_NI_FIXED_TL_HUFTS * ISAAC_NI_HUFT_BYTES)
#define NI_FIXED_TD_BYTES (ISAAC_NI_FIXED_TD_HUFTS * ISAAC_NI_HUFT_BYTES)
#define NI_STATS_PERIOD   0x10000U

static isaac_vita_native_inflate_stats s_stats;
static int s_banner_logged;
static uint32_t s_reason_logged;   /* bit per reason: first occurrence only */

static const char *const s_reason_names[ISAAC_NI_REASON_COUNT] = {
    "handled", "stack", "site", "state-ptr", "codes-ptr", "mode", "window",
    "cursor", "hufts", "checkfn", "bits", "tree", "subtree", "extra",
    "input", "output"
};

const isaac_vita_native_inflate_stats *isaac_vita_native_inflate_stats_get(void)
{
    return &s_stats;
}

void isaac_vita_native_inflate_stats_reset(void)
{
    memset(&s_stats, 0, sizeof s_stats);
    s_banner_logged = 0;
    s_reason_logged = 0U;
}

static void ni_log_banner(void)
{
    if (s_banner_logged)
        return;
    s_banner_logged = 1;
    isaac_vita_log(
        "KAGE VITA NATIVE INFLATE: seam=inflate_codes@%08x zlib=1.1.4 in-place "
        "fast=%08x flush=%08x adler=%08x site=%08x hufts=%u window=%u..%u build=%s",
        (unsigned)ISAAC_NI_INFLATE_CODES_RVA,
        (unsigned)ISAAC_NI_INFLATE_FAST_RVA,
        (unsigned)ISAAC_NI_INFLATE_FLUSH_RVA,
        (unsigned)ISAAC_NI_ADLER32_RVA,
        (unsigned)ISAAC_NI_CODES_RETURN_RVA,
        (unsigned)ISAAC_NI_MANY,
        (unsigned)ISAAC_NI_WINDOW_MIN, (unsigned)ISAAC_NI_WINDOW_MAX,
        ISAAC_VITA_NATIVE_INFLATE_BUILD_ID);
}

static void ni_log_stats(void)
{
    isaac_vita_log(
        "KAGE VITA NATIVE INFLATE STATS: calls=%u native=%u fallbacks=%u "
        "faults=%u msg=%u fast=%u flush=%u memcpy=%u adler=%u "
        "fb(stack,site,state,codes,mode,window,cursor,hufts,checkfn,bits,tree,"
        "subtree,extra,in,out)=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
        (unsigned)s_stats.calls, (unsigned)s_stats.handled,
        (unsigned)s_stats.fallbacks, (unsigned)s_stats.faults,
        (unsigned)s_stats.msg_mapped, (unsigned)s_stats.fast_calls,
        (unsigned)s_stats.flush_calls, (unsigned)s_stats.memcpy_calls,
        (unsigned)s_stats.adler_calls,
        (unsigned)s_stats.reasons[1], (unsigned)s_stats.reasons[2],
        (unsigned)s_stats.reasons[3], (unsigned)s_stats.reasons[4],
        (unsigned)s_stats.reasons[5], (unsigned)s_stats.reasons[6],
        (unsigned)s_stats.reasons[7], (unsigned)s_stats.reasons[8],
        (unsigned)s_stats.reasons[9], (unsigned)s_stats.reasons[10],
        (unsigned)s_stats.reasons[11], (unsigned)s_stats.reasons[12],
        (unsigned)s_stats.reasons[13], (unsigned)s_stats.reasons[14],
        (unsigned)s_stats.reasons[15]);
}

/* A Huffman table pointer the native leaf may follow: inside this stream's
 * hufts block (inflate_trees_dynamic/bits) or one of the PE's two fixed
 * tables (inflate_trees_fixed).  Entries are 8 bytes from a 4-aligned base;
 * the sub-table links inside them are index offsets, never pointers. */
static int ni_tree_ok(uint32_t tree, uint32_t hufts)
{
    if ((tree & 3U) != 0U)
        return 0;
    if (tree >= hufts && tree < hufts + NI_HUFTS_BYTES)
        return 1;
    if (tree >= NI_FIXED_TL_VA && tree < NI_FIXED_TL_VA + NI_FIXED_TL_BYTES)
        return 1;
    return tree >= NI_FIXED_TD_VA && tree < NI_FIXED_TD_VA + NI_FIXED_TD_BYTES;
}

static int ni_range_ok(uint32_t base, uint32_t count)
{
    return count == 0U || (base != 0U && base <= UINT32_MAX - count);
}

/* Every precondition the in-place run relies on.  The translated body would
 * run on any state; the native body runs only on states where its loads and
 * stores are exactly the ones the translated body would perform. */
static uint32_t ni_guard(const CPU *__restrict c, uint32_t s, uint32_t z)
{
    uint32_t codes, mode, window, end, size, read, write, hufts, checkfn;

    if (!guest_stack_contains(c, c->esp, 8U))
        return ISAAC_NI_REASON_STACK;
    if (ld32(c->esp) != NI_RETURN_WORD)
        return ISAAC_NI_REASON_SITE;
    if (s == 0U || (s & 3U) != 0U || z == 0U || (z & 3U) != 0U)
        return ISAAC_NI_REASON_STATE_PTR;
    codes = ld32(s + ISAAC_NI_BLK_CODES);
    if (codes == 0U || (codes & 3U) != 0U)
        return ISAAC_NI_REASON_CODES_PTR;
    mode = ld32(codes + ISAAC_NI_CS_MODE);
    if (mode > ISAAC_NI_MODE_BADCODE)
        return ISAAC_NI_REASON_MODE;
    window = ld32(s + ISAAC_NI_BLK_WINDOW);
    end = ld32(s + ISAAC_NI_BLK_END);
    size = end - window;
    if (window == 0U || end <= window || size < ISAAC_NI_WINDOW_MIN ||
            size > ISAAC_NI_WINDOW_MAX || (size & (size - 1U)) != 0U)
        return ISAAC_NI_REASON_WINDOW;
    read = ld32(s + ISAAC_NI_BLK_READ);
    write = ld32(s + ISAAC_NI_BLK_WRITE);
    if (read < window || read > end || write < window || write > end)
        return ISAAC_NI_REASON_CURSOR;
    hufts = ld32(s + ISAAC_NI_BLK_HUFTS);
    if (hufts == 0U || (hufts & 3U) != 0U || hufts > UINT32_MAX - NI_HUFTS_BYTES)
        return ISAAC_NI_REASON_HUFTS;
    checkfn = ld32(s + ISAAC_NI_BLK_CHECKFN);
    if (checkfn != 0U && checkfn != NI_ADLER32_VA)
        return ISAAC_NI_REASON_CHECKFN;
    if (ld8(codes + ISAAC_NI_CS_LBITS) > 16U || ld8(codes + ISAAC_NI_CS_DBITS) > 16U)
        return ISAAC_NI_REASON_BITS;
    if (!ni_tree_ok(ld32(codes + ISAAC_NI_CS_LTREE), hufts) ||
            !ni_tree_ok(ld32(codes + ISAAC_NI_CS_DTREE), hufts))
        return ISAAC_NI_REASON_TREE;
    if (mode == ISAAC_NI_MODE_LEN || mode == ISAAC_NI_MODE_DIST) {
        /* sub.code.tree / sub.code.need drive the next table walk */
        if (!ni_tree_ok(ld32(codes + ISAAC_NI_CS_SUB0), hufts) ||
                ld32(codes + ISAAC_NI_CS_SUB1) > 16U)
            return ISAAC_NI_REASON_SUBTREE;
    } else if (mode == ISAAC_NI_MODE_LENEXT || mode == ISAAC_NI_MODE_DISTEXT) {
        /* sub.copy.get indexes inflate_mask[17] */
        if (ld32(codes + ISAAC_NI_CS_SUB0) > 16U)
            return ISAAC_NI_REASON_EXTRA;
    }
    if (!ni_range_ok(ld32(z + ISAAC_NI_Z_NEXT_IN), ld32(z + ISAAC_NI_Z_AVAIL_IN)))
        return ISAAC_NI_REASON_INPUT;
    if (!ni_range_ok(ld32(z + ISAAC_NI_Z_NEXT_OUT), ld32(z + ISAAC_NI_Z_AVAIL_OUT)))
        return ISAAC_NI_REASON_OUTPUT;
    return ISAAC_NI_HANDLED;
}

static void ni_fallback(uint32_t reason)
{
    ++s_stats.fallbacks;
    ++s_stats.reasons[reason];
    s_stats.last_reason = reason;
    if ((s_reason_logged & (1U << reason)) == 0U) {
        s_reason_logged |= 1U << reason;
        isaac_vita_log("KAGE VITA NATIVE INFLATE FALLBACK: reason=%s call=%u "
                       "(translated inflate_codes used)",
                       s_reason_names[reason], (unsigned)s_stats.calls);
    }
}

/* The native leaf stores a host .rodata pointer into z->msg; the translated
 * body stores the PE's .rdata address of the same text.  Only two messages
 * exist in inflate_codes + inflate_fast. */
static uint32_t ni_map_msg(uint32_t host_msg)
{
    const char *text = (const char *)(uintptr_t)host_msg;

    if (strcmp(text, "invalid distance code") == 0)
        return NI_MSG_DIST_VA;
    if (strcmp(text, "invalid literal/length code") == 0)
        return NI_MSG_LITLEN_VA;
    return 0U;
}

static int ni_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    ++s_stats.faults;
    guest_fault(c, address, what);
    (void)gpop_generated(c);
    return 1;
}

int isaac_vita_native_inflate_codes_try(CPU *__restrict c)
{
    isaac_ni_hook_counters before;
    uint32_t s, z, reason, checkfn, saved_msg, new_msg, i;
    uint32_t fast, flush, memcpys, adlers;
    int32_t r, result;

    ++s_stats.calls;
    ni_log_banner();
    s = c->ecx;
    z = c->edx;
    reason = ni_guard(c, s, z);
    if (reason != ISAAC_NI_HANDLED) {
        ni_fallback(reason);
        /* Keep the census visible even when nothing is ever handled. */
        if ((s_stats.calls % NI_STATS_PERIOD) == 0U)
            ni_log_stats();
        return 0;
    }
    r = (int32_t)ld32(c->esp + 4U);

    /* The native inflate_flush calls s->checkfn directly: route the guest's
     * adler32 to the identical native leaf for this call only. */
    checkfn = ld32(s + ISAAC_NI_BLK_CHECKFN);
    if (checkfn != 0U)
        st32(s + ISAAC_NI_BLK_CHECKFN,
             (uint32_t)(uintptr_t)&isaac_ni_hook_adler32);
    saved_msg = ld32(z + ISAAC_NI_Z_MSG);
    before = isaac_ni_hooks;

    result = inflate_codes((inflate_blocks_statef *)(uintptr_t)s,
                           (z_streamp)(uintptr_t)z, r);

    if (checkfn != 0U)
        st32(s + ISAAC_NI_BLK_CHECKFN, checkfn);
    fast = isaac_ni_hooks.fast_calls - before.fast_calls;
    flush = isaac_ni_hooks.flush_calls - before.flush_calls;
    memcpys = isaac_ni_hooks.memcpy_calls - before.memcpy_calls;
    adlers = isaac_ni_hooks.adler_calls - before.adler_calls;

    new_msg = ld32(z + ISAAC_NI_Z_MSG);
    if (new_msg != saved_msg && new_msg != 0U) {
        uint32_t mapped = ni_map_msg(new_msg);
        if (mapped == 0U)
            return ni_fault(c, z, "native inflate_codes set an unknown zlib msg");
        st32(z + ISAAC_NI_Z_MSG, mapped);
        ++s_stats.msg_mapped;
    }

    ++s_stats.handled;
    s_stats.last_reason = ISAAC_NI_HANDLED;
    s_stats.fast_calls += fast;
    s_stats.flush_calls += flush;
    s_stats.memcpy_calls += memcpys;
    s_stats.adler_calls += adlers;

    /* Census of the translated body: its own entry and its direct callees
     * note coverage; the checkfn pointer call and the memcpy import go
     * through guest_call and must keep guest_call's logical census (same
     * contract as the PNG inflate_flush seam). */
    guest_coverage_function(ISAAC_NI_INFLATE_CODES_COVERAGE);
    if (fast != 0U)
        guest_coverage_function(ISAAC_NI_INFLATE_FAST_COVERAGE);
    if (flush != 0U)
        guest_coverage_function(ISAAC_NI_INFLATE_FLUSH_COVERAGE);
    for (i = 0U; i < adlers; ++i) {
        if (!guest_note_authenticated_translated_call(
                ISAAC_NI_ADLER32_RVA, ISAAC_NI_ADLER32_COVERAGE))
            return ni_fault(c, ISAAC_NI_ADLER32_RVA,
                            "native inflate_codes Adler target disappeared");
    }
    for (i = 0U; i < memcpys; ++i) {
        if (!guest_note_authenticated_import_call(
                ISAAC_NI_MEMCPY_IAT_RVA, ISAAC_NI_MEMCPY_IMPORT_ID,
                ISAAC_NI_MEMCPY_THUNK_COVERAGE))
            return ni_fault(c, ISAAC_NI_MEMCPY_IAT_RVA,
                            "native inflate_codes memcpy import disappeared");
    }

    /* First native run, then every NI_STATS_PERIOD: the device gate reads
     * calls/native/fallbacks from these lines. */
    if (s_stats.handled == 1U || (s_stats.handled % NI_STATS_PERIOD) == 0U)
        ni_log_stats();

    /* Epilogue of the translated body: result in eax, pop the return word;
     * the caller's `add esp, 4` removes r. */
    c->eax = (uint32_t)result;
    (void)gpop_generated(c);
    return 1;
}

#if defined(ISAAC_VITA_NATIVE_INFLATE_WRAP)
void __real_sub_005d6d80(CPU *__restrict c);

void __wrap_sub_005d6d80(CPU *__restrict c)
{
    if (isaac_vita_native_inflate_codes_try(c))
        return;
    __real_sub_005d6d80(c);
}
#endif
