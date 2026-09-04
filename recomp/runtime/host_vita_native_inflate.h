/* Native zlib 1.1.4 inflate_codes seam (ISAAC_VITA_NATIVE_INFLATE).
 *
 * The frozen PE statically links zlib 1.1.4.  Room/stage transitions run its
 * translated inflate:  inflate() sub_005c38a0 -> inflate_blocks sub_005ce7d0
 * -> inflate_codes sub_005d6d80 -> inflate_fast sub_005d7610 / inflate_flush
 * sub_005d6720 (+ adler32 sub_005cf3d0 and the memcpy import), at roughly
 * 1/4-1/5 of native speed on the A9.  The inner decoder family (codes + fast
 * + flush) is 26 % of a transition window's samples; inflate_blocks itself
 * (block headers, dynamic trees) is a small remainder and stays translated.
 *
 * Why the seam sits at inflate_codes and not at inflate():
 *   - inflate() has three translated callers and one (sub_005c5fb0) lives in
 *     the same generated TU (guest_0178.c), where GNU ld --wrap cannot
 *     intercept the direct C call; a partial wrap would run one stream
 *     translated and the others natively on the same shared png zstream.
 *   - inflate_codes has exactly one caller, `call 0x5d6d80` at 0x5cf07b inside
 *     inflate_blocks (guest_0180.c -> guest_0181.c), so --wrap=sub_005d6d80
 *     catches every edge; the return site is pinned as a guard.
 *   - inflate_codes is a pure function of state that lives entirely in guest
 *     memory in the exact zlib 1.1.4 ILP32 layout (inflate_blocks_state,
 *     inflate_codes_state, the Huffman tables, the window, the z_stream).
 *     It allocates nothing and calls nothing but inflate_fast, inflate_flush,
 *     memcpy and the check function.  The vendored 1.1.4 infcodes.c (compiled
 *     natively under the iz_ prefix, byte-identical to upstream) advances
 *     those same guest structures in place, so a native call and a translated
 *     call are interchangeable per call: no ownership, no lifetime, no shared
 *     stream hazard, and the seam can fall back to the translated body on any
 *     call (fail closed on every guard below).
 *
 * What the seam does per call (host_vita_native_inflate.c):
 *   1. guards: stack holds [ret, r] and ret is the pinned site; s/z/codes/
 *      hufts aligned and non-null; codes mode <= BADCODE; window is a power of
 *      two in 256..32768 with read/write inside; check function is null or the
 *      PE's adler32; lbits/dbits <= 16; every tree pointer inside the hufts
 *      block or the PE's fixed tables; in/out ranges do not wrap.  Any miss
 *      -> return 0 and the translated body runs on the untouched CPU.
 *   2. swaps s->checkfn (a guest code pointer the native leaf cannot call)
 *      to the native adler32 hook for the duration of the call, restores it.
 *   3. runs iz_inflate_codes(s, z, r) in place.
 *   4. maps a z->msg the native leaf set (host .rodata) back to the guest
 *      .rdata address the translated body stores; an unknown message faults.
 *   5. replays the census the translated body would have produced: coverage
 *      of inflate_codes/inflate_fast/inflate_flush, one authenticated
 *      translated-call note per adler32 call, one authenticated import note
 *      per memcpy call (the hooks count them exactly, including zero-length
 *      segments), faulting if a target disappeared (same contract as the
 *      PNG inflate_flush seam).
 *   6. epilogue of the translated body: eax = result, pop the return word;
 *      the caller's `add esp, 4` removes r.
 *
 * Proof: recomp/test_vita_native_inflate.py (PE pins, corpus pins, vendored
 * sha pins, blob-driven differential oracles incl. an ILP32 end-to-end run of
 * the shipped seam + core against the pristine 1.1.4 decoder, ARM cross
 * compile with the eboot flags).  Default OFF.
 */
#ifndef ISAAC_HOST_VITA_NATIVE_INFLATE_H
#define ISAAC_HOST_VITA_NATIVE_INFLATE_H

#include <stdint.h>

#include "guest.h"
#include "host_vita_native_inflate_layout.h"

/* Why the last call fell back (0 = handled natively). */
enum isaac_vita_native_inflate_reason {
    ISAAC_NI_HANDLED = 0,
    ISAAC_NI_REASON_STACK,      /* [esp, esp+8) not inside the bound guest stack */
    ISAAC_NI_REASON_SITE,       /* return word is not the pinned call site */
    ISAAC_NI_REASON_STATE_PTR,  /* s or z null / unaligned */
    ISAAC_NI_REASON_CODES_PTR,  /* s->sub.decode.codes null / unaligned */
    ISAAC_NI_REASON_MODE,       /* codes mode > BADCODE */
    ISAAC_NI_REASON_WINDOW,     /* window/end not a 256..32768 power of two */
    ISAAC_NI_REASON_CURSOR,     /* read/write outside [window, end] */
    ISAAC_NI_REASON_HUFTS,      /* hufts null / unaligned / wraps */
    ISAAC_NI_REASON_CHECKFN,    /* check function is neither null nor adler32 */
    ISAAC_NI_REASON_BITS,       /* lbits/dbits > 16 */
    ISAAC_NI_REASON_TREE,       /* ltree/dtree outside hufts and fixed tables */
    ISAAC_NI_REASON_SUBTREE,    /* LEN/DIST: sub.code.tree/need out of range */
    ISAAC_NI_REASON_EXTRA,      /* LENEXT/DISTEXT: sub.copy.get > 16 */
    ISAAC_NI_REASON_INPUT,      /* next_in/avail_in null or wrapping */
    ISAAC_NI_REASON_OUTPUT,     /* next_out/avail_out null or wrapping */
    ISAAC_NI_REASON_COUNT
};

typedef struct isaac_vita_native_inflate_stats {
    uint32_t calls;          /* __wrap entries */
    uint32_t handled;        /* served natively */
    uint32_t fallbacks;      /* sent to the translated body */
    uint32_t faults;         /* census/msg contract faults raised */
    uint32_t msg_mapped;     /* z->msg rewritten to a guest address */
    uint32_t fast_calls;     /* nested inflate_fast calls (census replayed) */
    uint32_t flush_calls;    /* nested inflate_flush calls */
    uint32_t memcpy_calls;   /* memcpy import notes replayed */
    uint32_t adler_calls;    /* adler32 translated-call notes replayed */
    uint32_t reasons[ISAAC_NI_REASON_COUNT];
    uint32_t last_reason;
} isaac_vita_native_inflate_stats;

/* Body of __wrap_sub_005d6d80: 1 = handled (CPU epilogue applied), 0 = the
 * caller must run __real_sub_005d6d80 on the untouched CPU. */
int isaac_vita_native_inflate_codes_try(CPU *__restrict c);

const isaac_vita_native_inflate_stats *isaac_vita_native_inflate_stats_get(void);
void isaac_vita_native_inflate_stats_reset(void);

#if defined(ISAAC_VITA_NATIVE_INFLATE_WRAP)
void __wrap_sub_005d6d80(CPU *__restrict c);
#endif

#if defined(ISAAC_NI_HOST_ORACLE)
/* Host oracles run the seam on a live host-side zlib 1.1.4 state: the check
 * function and the fixed tables are host addresses there, not PE addresses.
 * Absent from the eboot. */
extern uint32_t isaac_ni_oracle_adler32_va;
extern uint32_t isaac_ni_oracle_fixed_tl_va;
extern uint32_t isaac_ni_oracle_fixed_td_va;
#endif

#endif /* ISAAC_HOST_VITA_NATIVE_INFLATE_H */
