/* Differential oracle for the native zlib 1.1.4 inflate_codes seam.
 *
 * Reference decoder A: rf_* = the pristine vendored zlib 1.1.4 inflate path
 * (recomp/runtime/vendor/zlib114, sha-pinned to the upstream tarball), built
 * under the rf_ prefix with count-only shims on the leaves the seam replays
 * into the census (inflate_fast/inflate_flush/zmemcpy/check function).
 *
 * Decoder B depends on the mode:
 *   core  B = iz_*: the shipped core (infcodes.c through the codes wrapper
 *         with its ILP32 pins, inffast.c, infutil.c with the zmemcpy hook,
 *         adler32.c, host_vita_native_inflate_hooks.c) driven by the vendored
 *         inflate()/inflate_blocks() under the same iz_ prefix.  Proves the
 *         prefix/hook/NO_MEMCPY machinery is transparent and that the shipped
 *         leaves decode every stream exactly like the pristine reference.
 *         Any pointer width.
 *   e2e   B = sm_*: the pristine inflate()/inflate_blocks() whose inflate_codes
 *         call is bound to isaac_ni_e2e_codes_entry, which builds the guest
 *         CPU the translated caller would present (ecx=s, edx=z, [esp]=return
 *         site, [esp+4]=r) and runs the SHIPPED SEAM (host_vita_native_inflate.c
 *         guards, checkfn swap, in-place iz_inflate_codes, msg mapping, census
 *         replay, epilogue) on the live host-side 1.1.4 state.  ILP32 hosts only
 *         (the seam overlays the ILP32 zlib layout; built with clang -m32).
 *
 * Streams come from a blob written by test_vita_native_inflate.py with the
 * system zlib (levels 0-9, window bits 9-15, raw/zlib/dictionary streams, all
 * strategies incl. fixed/huffman-only/rle, sync/full/partial/block flush
 * points, sizes from 0 bytes to ~150 KB, raw streams decoded with a smaller
 * window than they were made with so the 1.1.4 copy-source wrap loops run,
 * and hand-built fixed-Huffman streams carrying the invalid distance codes
 * 30/31 and the invalid literal/length codes 286/287).  The oracle drives
 * each stream through several schedules (1-byte / small / medium /
 * whole-buffer avail_in and avail_out, Z_NO_FLUSH / Z_SYNC_FLUSH / Z_FINISH)
 * plus truncated and bit-flipped copies, and after EVERY inflate() call
 * asserts A == B on the return code, next_in/avail_in/total_in,
 * next_out/avail_out/total_out, adler, data_type, msg and the output bytes;
 * in e2e mode also on the inflate_blocks/inflate_codes state and the window.
 * Valid streams must additionally reproduce the original payload (the
 * independent system-zlib deflate side); Z_STREAM_END proves its Adler-32.
 *
 * usage: oracle core|e2e <blob> [seed] [max-streams]
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS 1   /* fopen under clang -m32 (MSVC CRT) */
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zutil.h"      /* zlib 1.1.4 types, unprefixed: the functions are
                           declared below with their explicit rf_/iz_/sm_ names */
#include "infblock.h"
#include "inftrees.h"
#include "infcodes.h"
#include "infutil.h"

#include "host_vita_native_inflate_layout.h"
#include "host_vita_native_inflate_hooks.h"

#ifndef ISAAC_NI_E2E
#define ISAAC_NI_E2E 0
#endif

#if ISAAC_NI_E2E
#include "host_vita_native_inflate.h"
_Static_assert(sizeof(void *) == 4, "the e2e oracle overlays the ILP32 zlib layout");
#endif

/* --- reference (rf_) and candidate (iz_/sm_) entry points ------------------ */

#define ZV "1.1.4"

extern int rf_inflateInit2_(z_streamp, int, const char *, int);
extern int rf_inflate(z_streamp, int);
extern int rf_inflateEnd(z_streamp);
extern int rf_inflateSetDictionary(z_streamp, const Bytef *, uInt);
extern int rf_inflate_trees_fixed(uIntf *, uIntf *, inflate_huft **, inflate_huft **, z_streamp);
extern int rf_inflate_fast(uInt, uInt, inflate_huft *, inflate_huft *, inflate_blocks_statef *, z_streamp);
extern int rf_inflate_flush(inflate_blocks_statef *, z_streamp, int);
extern uLong rf_adler32(uLong, const Bytef *, uInt);

#if ISAAC_NI_E2E
extern int sm_inflateInit2_(z_streamp, int, const char *, int);
extern int sm_inflate(z_streamp, int);
extern int sm_inflateEnd(z_streamp);
extern int sm_inflateSetDictionary(z_streamp, const Bytef *, uInt);
extern int sm_inflate_trees_fixed(uIntf *, uIntf *, inflate_huft **, inflate_huft **, z_streamp);
extern int sm_inflate_codes(inflate_blocks_statef *, z_streamp, int);
extern uLong sm_adler32(uLong, const Bytef *, uInt);
#define cand_inflateInit2_       sm_inflateInit2_
#define cand_inflate             sm_inflate
#define cand_inflateEnd          sm_inflateEnd
#define cand_inflateSetDictionary sm_inflateSetDictionary
#else
extern int iz_inflateInit2_(z_streamp, int, const char *, int);
extern int iz_inflate(z_streamp, int);
extern int iz_inflateEnd(z_streamp);
extern int iz_inflateSetDictionary(z_streamp, const Bytef *, uInt);
#define cand_inflateInit2_       iz_inflateInit2_
#define cand_inflate             iz_inflate
#define cand_inflateEnd          iz_inflateEnd
#define cand_inflateSetDictionary iz_inflateSetDictionary
#endif

/* inflate()'s private state (zlib 1.1.4 inflate.c), mirrored to reach the
 * blocks state; the compiler lays it out exactly as the vendored TU does. */
typedef enum {
    NI_METHOD, NI_FLAG, NI_DICT4, NI_DICT3, NI_DICT2, NI_DICT1, NI_DICT0,
    NI_BLOCKS, NI_CHECK4, NI_CHECK3, NI_CHECK2, NI_CHECK1, NI_DONE, NI_BAD
} ni_inflate_mode;
struct ni_internal_state {
    ni_inflate_mode mode;
    union {
        uInt method;
        struct { uLong was; uLong need; } check;
        uInt marker;
    } sub;
    int nowrap;
    uInt wbits;
    inflate_blocks_statef *blocks;
};

static struct inflate_blocks_state *blocks_of(const z_stream *z)
{
    return z->state ? (struct inflate_blocks_state *)
                      ((struct ni_internal_state *)z->state)->blocks : NULL;
}

/* --- reference-side counting shims (what the translated body would count) - */

typedef struct ni_counts { uint32_t fast, flush, memcpys, adler; } ni_counts;
static ni_counts g_rf;

int ni_rf_hook_inflate_fast(uInt bl, uInt bd, inflate_huft *tl, inflate_huft *td,
                            inflate_blocks_statef *s, z_streamp z)
{
    ++g_rf.fast;
    return rf_inflate_fast(bl, bd, tl, td, s, z);
}
int ni_rf_hook_inflate_flush(inflate_blocks_statef *s, z_streamp z, int r)
{
    ++g_rf.flush;
    return rf_inflate_flush(s, z, r);
}
void ni_rf_hook_zmemcpy(Bytef *dest, const Bytef *source, uInt len)
{
    ++g_rf.memcpys;
    if (len) memcpy(dest, source, len);
}
static uLong ni_rf_hook_adler32(uLong adler, const Bytef *buf, uInt len)
{
    ++g_rf.adler;
    return rf_adler32(adler, buf, len);
}

/* --- e2e: the seam entry bound into sm_'s inflate_blocks ------------------ */

#if ISAAC_NI_E2E
static struct {
    uint32_t calls, handled, fallbacks, bad_epilogue, faults;
    uint32_t translated_notes, import_notes;
    uint32_t seam_fast, seam_flush, seam_memcpy, seam_adler;   /* from hooks */
} g_e2e;
static unsigned char g_coverage[16384];
unsigned char *g_guest_coverage_functions = g_coverage;
unsigned char *g_guest_coverage_imports;
unsigned char *g_guest_coverage_cases;

uint32_t gpop_generated(CPU *__restrict c)
{
    uint32_t value = *(uint32_t *)(uintptr_t)c->esp;
    c->esp += 4U;
    return value;
}
void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    ++g_e2e.faults;
    c->fault = what;
    c->fault_addr = address;
    fprintf(stderr, "e2e: unexpected guest_fault(%08x, %s)\n", (unsigned)address, what);
}
int guest_note_authenticated_translated_call(uint32_t address, uint32_t coverage_id)
{
    ++g_e2e.translated_notes;
    return address == ISAAC_NI_ADLER32_RVA && coverage_id == ISAAC_NI_ADLER32_COVERAGE;
}
int guest_note_authenticated_import_call(uint32_t slot, uint32_t import_id, uint32_t thunk_id)
{
    ++g_e2e.import_notes;
    return slot == ISAAC_NI_MEMCPY_IAT_RVA && import_id == ISAAC_NI_MEMCPY_IMPORT_ID &&
           thunk_id == ISAAC_NI_MEMCPY_THUNK_COVERAGE;
}
void isaac_vita_log(const char *format, ...) { (void)format; }

int isaac_ni_e2e_codes_entry(inflate_blocks_statef *s, z_streamp z, int r)
{
    CPU cpu;
    uint32_t stack[16];
    isaac_ni_hook_counters before = isaac_ni_hooks;

    memset(&cpu, 0, sizeof cpu);
    memset(stack, 0xcd, sizeof stack);
    cpu.ecx = (uint32_t)(uintptr_t)s;
    cpu.edx = (uint32_t)(uintptr_t)z;
    cpu.esp = (uint32_t)(uintptr_t)&stack[8];
    cpu.stack_owner = &cpu;
    cpu.stack_floor = (uint32_t)(uintptr_t)&stack[0];
    cpu.stack_ceiling = (uint32_t)(uintptr_t)&stack[16];
    cpu.stack_low_water = cpu.stack_floor;
    stack[8] = (uint32_t)ISAAC_NI_CODES_RETURN_RVA /* RVA, as the corpus pushes it */;
    stack[9] = (uint32_t)r;
    ++g_e2e.calls;
    if (isaac_vita_native_inflate_codes_try(&cpu)) {
        ++g_e2e.handled;
        if (cpu.esp != (uint32_t)(uintptr_t)&stack[9] || cpu.fault != NULL ||
                cpu.ecx != (uint32_t)(uintptr_t)s || cpu.edx != (uint32_t)(uintptr_t)z ||
                stack[9] != (uint32_t)r)
            ++g_e2e.bad_epilogue;
        g_e2e.seam_fast += isaac_ni_hooks.fast_calls - before.fast_calls;
        g_e2e.seam_flush += isaac_ni_hooks.flush_calls - before.flush_calls;
        g_e2e.seam_memcpy += isaac_ni_hooks.memcpy_calls - before.memcpy_calls;
        g_e2e.seam_adler += isaac_ni_hooks.adler_calls - before.adler_calls;
        return (int)cpu.eax;
    }
    ++g_e2e.fallbacks;
    fprintf(stderr, "e2e: seam fell back (reason %u) on a live state\n",
            (unsigned)isaac_vita_native_inflate_stats_get()->last_reason);
    return sm_inflate_codes(s, z, r);
}
#endif /* ISAAC_NI_E2E */

/* --- blob ------------------------------------------------------------------- */

enum { KIND_ZLIB = 0, KIND_RAW = 1, KIND_DICT = 2, KIND_SMALLWIN = 3, KIND_BADCODE = 4 };

typedef struct blob_stream {
    uint8_t kind;
    int8_t wbits;            /* inflate window bits (negative = raw) */
    const uint8_t *orig; uint32_t orig_len;
    const uint8_t *comp; uint32_t comp_len;
    const uint8_t *dict; uint32_t dict_len;
} blob_stream;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf; long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    buf = (uint8_t *)malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

/* --- PRNG ------------------------------------------------------------------- */

static uint64_t rng_s;
static uint32_t rnd(void)
{
    rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17;
    return (uint32_t)(rng_s >> 11);
}
static uint32_t rndm(uint32_t n) { return n ? rnd() % n : 0U; }

/* --- statistics ------------------------------------------------------------- */

static struct {
    unsigned long streams, passes, calls, streamend, buferr, dataerr, needdict, dictset, noprogress;
    unsigned long trunc_passes, corrupt_passes, smallwin_passes, badcode_passes, badcode_msgs;
} g;

static int fail(const char *what, long a, long b, unsigned long stream, unsigned long pass, unsigned long call)
{
    fprintf(stderr, "MISMATCH %s: A=%ld B=%ld (stream %lu pass %lu call %lu)\n",
            what, a, b, stream, pass, call);
    return 1;
}

/* --- per-call comparison ---------------------------------------------------- */

#if ISAAC_NI_E2E
static inflate_huft *g_fixed_tl_a, *g_fixed_td_a, *g_fixed_tl_b, *g_fixed_td_b;

/* A tree pointer normalised to (space, offset) so A's and B's private tables
 * can be compared: 1 = this stream's hufts block, 2/3 = fixed tl/td. */
static uint64_t norm_tree(uint32_t t, const struct inflate_blocks_state *b,
                          inflate_huft *tl, inflate_huft *td)
{
    uint32_t hufts = (uint32_t)(uintptr_t)b->hufts;
    if (t >= hufts && t < hufts + ISAAC_NI_MANY * ISAAC_NI_HUFT_BYTES)
        return ((uint64_t)1 << 32) | (t - hufts);
    if (t >= (uint32_t)(uintptr_t)tl && t < (uint32_t)(uintptr_t)tl + ISAAC_NI_FIXED_TL_HUFTS * 8U)
        return ((uint64_t)2 << 32) | (t - (uint32_t)(uintptr_t)tl);
    if (t >= (uint32_t)(uintptr_t)td && t < (uint32_t)(uintptr_t)td + ISAAC_NI_FIXED_TD_HUFTS * 8U)
        return ((uint64_t)3 << 32) | (t - (uint32_t)(uintptr_t)td);
    return t;
}

static uint32_t cs32(const void *codes, int off) { return *(const uint32_t *)((const char *)codes + off); }
static uint8_t cs8(const void *codes, int off) { return *(const uint8_t *)((const char *)codes + off); }

static int compare_internal(const z_stream *za, const z_stream *zb, int full_window,
                            unsigned long stream, unsigned long pass, unsigned long call)
{
    const struct inflate_blocks_state *a = blocks_of(za), *b = blocks_of(zb);
    uint32_t wsize;
    if (!a || !b) return (a == b) ? 0 : fail("blocks", a != NULL, b != NULL, stream, pass, call);
    if ((int)a->mode != (int)b->mode) return fail("blk.mode", (int)a->mode, (int)b->mode, stream, pass, call);
    if (a->bitk != b->bitk) return fail("blk.bitk", (long)a->bitk, (long)b->bitk, stream, pass, call);
    if (a->bitb != b->bitb) return fail("blk.bitb", (long)a->bitb, (long)b->bitb, stream, pass, call);
    if (a->check != b->check) return fail("blk.check", (long)a->check, (long)b->check, stream, pass, call);
    if ((a->end - a->window) != (b->end - b->window)) return fail("blk.size", a->end - a->window, b->end - b->window, stream, pass, call);
    if ((a->read - a->window) != (b->read - b->window)) return fail("blk.read", a->read - a->window, b->read - b->window, stream, pass, call);
    if ((a->write - a->window) != (b->write - b->window)) return fail("blk.write", a->write - a->window, b->write - b->window, stream, pass, call);
    wsize = (uint32_t)(a->end - a->window);
    if (a->mode == CODES) {
        const void *ca = a->sub.decode.codes, *cb = b->sub.decode.codes;
        uint32_t ma, mb;
        if (!ca || !cb) return fail("codes ptr", ca != NULL, cb != NULL, stream, pass, call);
        ma = cs32(ca, ISAAC_NI_CS_MODE); mb = cs32(cb, ISAAC_NI_CS_MODE);
        if (ma != mb) return fail("cs.mode", (long)ma, (long)mb, stream, pass, call);
        if (cs32(ca, ISAAC_NI_CS_LEN) != cs32(cb, ISAAC_NI_CS_LEN)) return fail("cs.len", (long)cs32(ca, 4), (long)cs32(cb, 4), stream, pass, call);
        if (cs8(ca, ISAAC_NI_CS_LBITS) != cs8(cb, ISAAC_NI_CS_LBITS) || cs8(ca, ISAAC_NI_CS_DBITS) != cs8(cb, ISAAC_NI_CS_DBITS))
            return fail("cs.bits", cs8(ca, 0x10) * 256 + cs8(ca, 0x11), cs8(cb, 0x10) * 256 + cs8(cb, 0x11), stream, pass, call);
        if (norm_tree(cs32(ca, ISAAC_NI_CS_LTREE), a, g_fixed_tl_a, g_fixed_td_a) !=
                norm_tree(cs32(cb, ISAAC_NI_CS_LTREE), b, g_fixed_tl_b, g_fixed_td_b))
            return fail("cs.ltree", 0, 1, stream, pass, call);
        if (norm_tree(cs32(ca, ISAAC_NI_CS_DTREE), a, g_fixed_tl_a, g_fixed_td_a) !=
                norm_tree(cs32(cb, ISAAC_NI_CS_DTREE), b, g_fixed_tl_b, g_fixed_td_b))
            return fail("cs.dtree", 0, 1, stream, pass, call);
        if (ma == ISAAC_NI_MODE_LEN || ma == ISAAC_NI_MODE_DIST) {
            if (norm_tree(cs32(ca, ISAAC_NI_CS_SUB0), a, g_fixed_tl_a, g_fixed_td_a) !=
                    norm_tree(cs32(cb, ISAAC_NI_CS_SUB0), b, g_fixed_tl_b, g_fixed_td_b))
                return fail("cs.sub.tree", 0, 1, stream, pass, call);
            if (cs32(ca, ISAAC_NI_CS_SUB1) != cs32(cb, ISAAC_NI_CS_SUB1))
                return fail("cs.sub.need", (long)cs32(ca, 0xc), (long)cs32(cb, 0xc), stream, pass, call);
        } else if (ma != 0U && ma <= 6U) {  /* LENEXT/DISTEXT/COPY/LIT: sub is plain data */
            if (cs32(ca, ISAAC_NI_CS_SUB0) != cs32(cb, ISAAC_NI_CS_SUB0) ||
                    cs32(ca, ISAAC_NI_CS_SUB1) != cs32(cb, ISAAC_NI_CS_SUB1))
                return fail("cs.sub", (long)cs32(ca, 8), (long)cs32(cb, 8), stream, pass, call);
        }
    }
    if (full_window && memcmp(a->window, b->window, wsize) != 0)
        return fail("window bytes", 0, 1, stream, pass, call);
    return 0;
}
#endif /* ISAAC_NI_E2E */

static const char *msg_text_b(const z_stream *zb)
{
#if ISAAC_NI_E2E
    /* the seam rewrites the two codes/fast messages to the PE's addresses */
    uint32_t m = (uint32_t)(uintptr_t)zb->msg;
    if (m == (uint32_t)GUEST_IMAGE_BASE + ISAAC_NI_MSG_BAD_DIST_RVA) return "invalid distance code";
    if (m == (uint32_t)GUEST_IMAGE_BASE + ISAAC_NI_MSG_BAD_LITLEN_RVA) return "invalid literal/length code";
#endif
    return zb->msg;
}

static int compare_public(const z_stream *za, const z_stream *zb, int ra, int rb,
                          const uint8_t *comp, const uint8_t *oa, const uint8_t *ob,
                          unsigned long stream, unsigned long pass, unsigned long call)
{
    const char *ma, *mb;
    if (ra != rb) return fail("ret", ra, rb, stream, pass, call);
    if ((za->next_in - comp) != (zb->next_in - comp)) return fail("next_in", za->next_in - comp, zb->next_in - comp, stream, pass, call);
    if (za->avail_in != zb->avail_in) return fail("avail_in", (long)za->avail_in, (long)zb->avail_in, stream, pass, call);
    if (za->total_in != zb->total_in) return fail("total_in", (long)za->total_in, (long)zb->total_in, stream, pass, call);
    if ((za->next_out - oa) != (zb->next_out - ob)) return fail("next_out", za->next_out - oa, zb->next_out - ob, stream, pass, call);
    if (za->avail_out != zb->avail_out) return fail("avail_out", (long)za->avail_out, (long)zb->avail_out, stream, pass, call);
    if (za->total_out != zb->total_out) return fail("total_out", (long)za->total_out, (long)zb->total_out, stream, pass, call);
    if (za->adler != zb->adler) return fail("adler", (long)za->adler, (long)zb->adler, stream, pass, call);
    if (za->data_type != zb->data_type) return fail("data_type", za->data_type, zb->data_type, stream, pass, call);
    ma = za->msg; mb = msg_text_b(zb);
    if ((ma == NULL) != (mb == NULL) || (ma && strcmp(ma, mb) != 0)) {
        fprintf(stderr, "MISMATCH msg: A=%s B=%s (stream %lu pass %lu call %lu)\n",
                ma ? ma : "(null)", mb ? mb : "(null)", stream, pass, call);
        return 1;
    }
    if (memcmp(oa, ob, (size_t)(za->next_out - oa)) != 0) return fail("output bytes", 0, 1, stream, pass, call);
    return 0;
}

/* --- one pass over one stream ---------------------------------------------- */

static int run_pass(const blob_stream *bs, const uint8_t *comp, uint32_t comp_len, int corrupt,
                    unsigned long stream, unsigned long pass)
{
    z_stream za, zb;
    struct inflate_blocks_state *ba, *bb;
    uint8_t *oa, *ob;
    size_t out_cap = (size_t)bs->orig_len + 64U;
    size_t pi = 0;
    uint32_t in_mode = rndm(4), out_mode = rndm(4);
    int fl = (int)rndm(3); int flush = fl == 0 ? Z_NO_FLUSH : fl == 1 ? Z_SYNC_FLUSH : Z_FINISH;
    int ra = Z_OK, rb, rc = 0, done = 0, dict_asked = 0, got_msg = 0;
    unsigned long call = 0, stalls = 0;
    unsigned long guard = 8UL * ((unsigned long)comp_len + bs->orig_len) + 4096UL;
    ni_counts rf0;
    isaac_ni_hook_counters iz0;

    /* corrupt / wrongly-windowed / hand-built streams may produce any amount */
    if (corrupt || bs->kind == KIND_SMALLWIN || bs->kind == KIND_BADCODE)
        out_cap = (size_t)bs->orig_len * 2U + 4096U;
    oa = (uint8_t *)malloc(out_cap); ob = (uint8_t *)malloc(out_cap);
    if (!oa || !ob) { fprintf(stderr, "oom\n"); return 1; }
    memset(&za, 0, sizeof za); memset(&zb, 0, sizeof zb);
    ra = rf_inflateInit2_(&za, bs->wbits, ZV, (int)sizeof(z_stream));
    rb = cand_inflateInit2_(&zb, bs->wbits, ZV, (int)sizeof(z_stream));
    if (ra != rb) { rc = fail("init", ra, rb, stream, pass, 0); goto out; }
    if (ra != Z_OK) { rc = fail("init not ok", ra, rb, stream, pass, 0); goto out; }

    /* count-only check functions on the reference; the candidate's is the
     * pristine adler32 (core) or the seam's swap target (e2e) */
    ba = blocks_of(&za); bb = blocks_of(&zb);
    if (!ba || !bb) { rc = fail("blocks after init", ba != NULL, bb != NULL, stream, pass, 0); goto out; }
    if (ba->checkfn) ba->checkfn = ni_rf_hook_adler32;
#if ISAAC_NI_E2E
    if (bb->checkfn) {
        if (bb->checkfn != sm_adler32) { rc = fail("sm checkfn", 0, 1, stream, pass, 0); goto out; }
        isaac_ni_oracle_adler32_va = (uint32_t)(uintptr_t)sm_adler32;
    }
#else
    if (bb->checkfn) bb->checkfn = isaac_ni_hook_adler32;
#endif
    rf0 = g_rf; iz0 = isaac_ni_hooks;

    za.next_out = oa; za.avail_out = 0; zb.next_out = ob; zb.avail_out = 0;
    for (;;) {
        uint32_t in_give, out_give;
        size_t in_rem = comp_len - pi, out_rem = out_cap - (size_t)(za.next_out - oa);
        uint32_t ai0, ao0;
        if (za.avail_in == 0U) {
            switch (in_mode) {
            case 0: in_give = in_rem ? 1U : 0U; break;
            case 1: in_give = (uint32_t)(in_rem < 7U ? in_rem : 1U + rndm(7)); break;
            case 2: in_give = (uint32_t)(in_rem < 64U ? in_rem : 1U + rndm(64)); break;
            default: in_give = (uint32_t)in_rem; break;
            }
            za.next_in = (Bytef *)comp + pi; za.avail_in = in_give;
            zb.next_in = (Bytef *)comp + pi; zb.avail_in = in_give;
            pi += in_give;
        }
        if (za.avail_out == 0U) {
            if (out_rem == 0U) break;                       /* capacity exhausted */
            switch (out_mode) {
            case 0: out_give = 1U; break;
            case 1: out_give = 1U + rndm(15); break;
            case 2: out_give = 1U + rndm(255); break;
            default: out_give = (uint32_t)out_rem; break;
            }
            if (out_give > out_rem) out_give = (uint32_t)out_rem;
            za.avail_out = out_give; zb.avail_out = out_give;
        }
        ai0 = za.avail_in; ao0 = za.avail_out;
        ra = rf_inflate(&za, flush);
        rb = cand_inflate(&zb, flush);
        ++call; ++g.calls;
        if (compare_public(&za, &zb, ra, rb, comp, oa, ob, stream, pass, call)) { rc = 1; goto out; }
#if ISAAC_NI_E2E
        if (compare_internal(&za, &zb, (call % 64U) == 0U || out_mode == 3U, stream, pass, call)) { rc = 1; goto out; }
#endif
        if (za.msg) got_msg = 1;
        if (ra == Z_STREAM_END) { done = 1; ++g.streamend; break; }
        if (ra == Z_NEED_DICT) {
            ++g.needdict;
            if (bs->dict_len == 0U || dict_asked) break;
            dict_asked = 1;
            ra = rf_inflateSetDictionary(&za, bs->dict, bs->dict_len);
            rb = cand_inflateSetDictionary(&zb, bs->dict, bs->dict_len);
            if (ra != rb) { rc = fail("setdict", ra, rb, stream, pass, call); goto out; }
            if (compare_public(&za, &zb, ra, rb, comp, oa, ob, stream, pass, call)) { rc = 1; goto out; }
#if ISAAC_NI_E2E
            if (compare_internal(&za, &zb, 1, stream, pass, call)) { rc = 1; goto out; }
#endif
            if (ra != Z_OK) break;                          /* corrupt dict id */
            ++g.dictset;
            continue;
        }
        if (ra == Z_DATA_ERROR) { ++g.dataerr; break; }
        if (ra == Z_STREAM_ERROR || ra == Z_MEM_ERROR) break;
        if (za.avail_in == ai0 && za.avail_out == ao0) {
            /* no progress: input exhausted (truncated) or a genuine stall */
            if (pi >= comp_len && za.avail_in == 0U) { if (ra == Z_BUF_ERROR) ++g.buferr; break; }
            if (++stalls > 2UL) { ++g.noprogress; break; }
        } else stalls = 0;
        if (call > guard) { fprintf(stderr, "runaway pass (stream %lu pass %lu)\n", stream, pass); rc = 1; goto out; }
    }
#if ISAAC_NI_E2E
    if (compare_internal(&za, &zb, 1, stream, pass, call)) { rc = 1; goto out; }
#endif
    /* the census the seam/candidate replays must equal the reference's own
     * leaf call counts */
    {
        ni_counts d = { g_rf.fast - rf0.fast, g_rf.flush - rf0.flush, g_rf.memcpys - rf0.memcpys, g_rf.adler - rf0.adler };
        uint32_t cf = isaac_ni_hooks.fast_calls - iz0.fast_calls, cl = isaac_ni_hooks.flush_calls - iz0.flush_calls;
        uint32_t cm = isaac_ni_hooks.memcpy_calls - iz0.memcpy_calls, ca = isaac_ni_hooks.adler_calls - iz0.adler_calls;
#if ISAAC_NI_E2E
        /* sm_'s own inflate_blocks flushes (stored blocks / block ends) call
         * sm_ leaves, not the hooks: only the codes-nested part is visible to
         * the seam, so compare fast (all nested in codes) exactly and the rest
         * as "seam count <= reference count" plus the coverage notes identity. */
        if (cf != d.fast) { rc = fail("census fast", (long)d.fast, (long)cf, stream, pass, call); goto out; }
        if (cl > d.flush || cm > d.memcpys || ca > d.adler) { rc = fail("census nested > reference", (long)d.flush, (long)cl, stream, pass, call); goto out; }
#else
        if (cf != d.fast) { rc = fail("census fast", (long)d.fast, (long)cf, stream, pass, call); goto out; }
        if (cl != d.flush) { rc = fail("census flush", (long)d.flush, (long)cl, stream, pass, call); goto out; }
        if (cm != d.memcpys) { rc = fail("census memcpy", (long)d.memcpys, (long)cm, stream, pass, call); goto out; }
        if (ca != d.adler) { rc = fail("census adler", (long)d.adler, (long)ca, stream, pass, call); goto out; }
#endif
    }
    if (!corrupt && (bs->kind == KIND_ZLIB || bs->kind == KIND_RAW || bs->kind == KIND_DICT)) {
        /* every valid stream must reach STREAM_END with the original payload */
        if (!done) {
            fprintf(stderr, "stream %lu: kind %u wbits %d orig %lu comp %lu in_mode %u out_mode %u "
                    "flush %d consumed %lu produced %lu stalls %lu\n",
                    stream, (unsigned)bs->kind, (int)bs->wbits, (unsigned long)bs->orig_len, (unsigned long)comp_len,
                    (unsigned)in_mode, (unsigned)out_mode, flush, (unsigned long)za.total_in, (unsigned long)za.total_out, stalls);
            rc = fail("valid stream did not end", ra, rb, stream, pass, call); goto out;
        }
        if ((size_t)(za.next_out - oa) != bs->orig_len || memcmp(oa, bs->orig, bs->orig_len) != 0) {
            fprintf(stderr, "MISMATCH decoded-vs-original (stream %lu len %zu orig %lu)\n",
                    stream, (size_t)(za.next_out - oa), (unsigned long)bs->orig_len);
            rc = 1; goto out;
        }
        /* (1.1.4 resets z->adler to 1 at Z_STREAM_END after comparing it with the
         * trailer, so STREAM_END itself proves the Adler-32 of the output.) */
        if (bs->kind == KIND_DICT && !dict_asked) {
            rc = fail("dict stream never asked for its dictionary", ra, rb, stream, pass, call); goto out;
        }
    }
    if (!corrupt && bs->kind == KIND_BADCODE) {
        /* hand-built: must end in Z_DATA_ERROR with one of the two codes messages */
        if (ra != Z_DATA_ERROR || !got_msg ||
                (strcmp(za.msg, "invalid distance code") != 0 &&
                 strcmp(za.msg, "invalid literal/length code") != 0)) {
            fprintf(stderr, "badcode stream %lu ended with %d msg=%s\n", stream, ra, za.msg ? za.msg : "(null)");
            rc = 1; goto out;
        }
        ++g.badcode_msgs;
    }
out:
    rf_inflateEnd(&za); cand_inflateEnd(&zb);
    free(oa); free(ob);
    ++g.passes;
    return rc;
}

int main(int argc, char **argv)
{
    const char *mode; size_t blob_len; uint8_t *blob; const uint8_t *p, *end;
    uint32_t count, i, max_streams = 0U;
    int e2e = 0;

    if (argc < 3) { fprintf(stderr, "usage: oracle core|e2e <blob> [seed] [max-streams]\n"); return 2; }
    mode = argv[1];
    e2e = strcmp(mode, "e2e") == 0;
    if (e2e != ISAAC_NI_E2E) { fprintf(stderr, "this binary is built for mode %s\n", ISAAC_NI_E2E ? "e2e" : "core"); return 2; }
    rng_s = (argc > 3 ? strtoull(argv[3], NULL, 10) : 1ULL) * 0x9e3779b97f4a7c15ULL + 0x632be59bd9b4e019ULL;
    if (!rng_s) rng_s = 1;
    for (i = 0; i < 32U; ++i) (void)rnd();   /* warm the xorshift out of a small seed */
    if (argc > 4) max_streams = (uint32_t)strtoul(argv[4], NULL, 10);
    blob = read_file(argv[2], &blob_len);
    if (!blob || blob_len < 12U || memcmp(blob, "NIBL", 4) != 0 || rd32(blob + 4) != 2U) {
        fprintf(stderr, "bad blob %s\n", argv[2]); return 2;
    }
    count = rd32(blob + 8);
    p = blob + 12; end = blob + blob_len;

#if ISAAC_NI_E2E
    {
        uIntf bl, bd; z_stream zt; memset(&zt, 0, sizeof zt);
        rf_inflate_trees_fixed(&bl, &bd, &g_fixed_tl_a, &g_fixed_td_a, &zt);
        sm_inflate_trees_fixed(&bl, &bd, &g_fixed_tl_b, &g_fixed_td_b, &zt);
        /* the seam's tree guard must accept sm_'s fixed tables */
        isaac_ni_oracle_fixed_tl_va = (uint32_t)(uintptr_t)g_fixed_tl_b;
        isaac_ni_oracle_fixed_td_va = (uint32_t)(uintptr_t)g_fixed_td_b;
        isaac_vita_native_inflate_stats_reset();
    }
#endif

    for (i = 0; i < count; ++i) {
        blob_stream bs; uint8_t *tmp; uint32_t passes, k;
        if (max_streams && i >= max_streams) break;
        if (end - p < 16) { fprintf(stderr, "truncated blob\n"); return 2; }
        bs.kind = p[0]; bs.wbits = (int8_t)p[1];
        bs.orig_len = rd32(p + 4); bs.comp_len = rd32(p + 8); bs.dict_len = rd32(p + 12); p += 16;
        if ((size_t)(end - p) < (size_t)bs.orig_len + bs.comp_len + bs.dict_len) { fprintf(stderr, "truncated blob\n"); return 2; }
        bs.orig = p; p += bs.orig_len; bs.comp = p; p += bs.comp_len; bs.dict = p; p += bs.dict_len;
        ++g.streams;

        passes = bs.comp_len > 20000U ? 2U : bs.comp_len > 4000U ? 3U : 5U;
        for (k = 0; k < passes; ++k)
            if (run_pass(&bs, bs.comp, bs.comp_len, 0, i, k)) return 1;
        if (bs.kind == KIND_SMALLWIN) g.smallwin_passes += passes;
        if (bs.kind == KIND_BADCODE) g.badcode_passes += passes;

        tmp = (uint8_t *)malloc(bs.comp_len + 1U);
        if (!tmp) return 1;
        memcpy(tmp, bs.comp, bs.comp_len);
        if (bs.comp_len > 2U) {
            /* truncated */
            uint32_t t = 1U + rndm(bs.comp_len - 1U);
            ++g.trunc_passes;
            if (run_pass(&bs, tmp, bs.comp_len - t, 1, i, 100U)) return 1;
            /* one or two bit flips anywhere (header, trees, codes, trailer) */
            for (k = 0; k < 2U; ++k) {
                uint32_t pos = rndm(bs.comp_len);
                tmp[pos] ^= (uint8_t)(1U << rndm(8));
                if (k == 1U) { uint32_t pos2 = rndm(bs.comp_len); tmp[pos2] ^= (uint8_t)(1U << rndm(8)); }
                ++g.corrupt_passes;
                if (run_pass(&bs, tmp, bs.comp_len, 1, i, 200U + k)) return 1;
                memcpy(tmp, bs.comp, bs.comp_len);
            }
        }
        free(tmp);
    }
    free(blob);

    if (g.badcode_passes && g.badcode_msgs != g.badcode_passes) {
        fprintf(stderr, "badcode streams: %lu passes, %lu reached the codes message\n",
                g.badcode_passes, g.badcode_msgs);
        return 1;
    }

#if ISAAC_NI_E2E
    {
        const isaac_vita_native_inflate_stats *st = isaac_vita_native_inflate_stats_get();
        if (g_e2e.fallbacks || g_e2e.bad_epilogue || g_e2e.faults || st->faults) {
            fprintf(stderr, "e2e: fallbacks=%u bad_epilogue=%u faults=%u/%u\n",
                    (unsigned)g_e2e.fallbacks, (unsigned)g_e2e.bad_epilogue,
                    (unsigned)g_e2e.faults, (unsigned)st->faults);
            return 1;
        }
        if (st->calls != g_e2e.calls || st->handled != g_e2e.handled ||
                st->fast_calls != g_e2e.seam_fast || st->flush_calls != g_e2e.seam_flush ||
                st->memcpy_calls != g_e2e.seam_memcpy || st->adler_calls != g_e2e.seam_adler) {
            fprintf(stderr, "e2e: seam stats disagree with the entry's own counts\n");
            return 1;
        }
        if (g_e2e.translated_notes != g_e2e.seam_adler || g_e2e.import_notes != g_e2e.seam_memcpy) {
            fprintf(stderr, "e2e: census notes %u/%u != hook counts %u/%u\n",
                    (unsigned)g_e2e.translated_notes, (unsigned)g_e2e.import_notes,
                    (unsigned)g_e2e.seam_adler, (unsigned)g_e2e.seam_memcpy);
            return 1;
        }
        if (!g_coverage[ISAAC_NI_INFLATE_CODES_COVERAGE] || !g_coverage[ISAAC_NI_INFLATE_FAST_COVERAGE] ||
                !g_coverage[ISAAC_NI_INFLATE_FLUSH_COVERAGE]) {
            fprintf(stderr, "e2e: coverage census incomplete\n");
            return 1;
        }
        if (g.badcode_passes && st->msg_mapped == 0U) {
            fprintf(stderr, "e2e: the codes messages were never mapped\n");
            return 1;
        }
        printf("native inflate e2e oracle: PASS; streams=%lu passes=%lu calls=%lu streamend=%lu "
               "buferr=%lu dataerr=%lu needdict=%lu dictset=%lu smallwin=%lu badcode=%lu trunc=%lu corrupt=%lu "
               "codes_calls=%u handled=%u fallbacks=%u msg_mapped=%u fast=%u flush=%u memcpy=%u adler=%u\n",
               g.streams, g.passes, g.calls, g.streamend, g.buferr, g.dataerr, g.needdict, g.dictset,
               g.smallwin_passes, g.badcode_passes, g.trunc_passes, g.corrupt_passes,
               (unsigned)st->calls, (unsigned)st->handled, (unsigned)st->fallbacks,
               (unsigned)st->msg_mapped, (unsigned)st->fast_calls, (unsigned)st->flush_calls,
               (unsigned)st->memcpy_calls, (unsigned)st->adler_calls);
    }
#else
    printf("native inflate core oracle: PASS; streams=%lu passes=%lu calls=%lu streamend=%lu "
           "buferr=%lu dataerr=%lu needdict=%lu dictset=%lu smallwin=%lu badcode=%lu trunc=%lu corrupt=%lu "
           "fast=%u flush=%u memcpy=%u adler=%u\n",
           g.streams, g.passes, g.calls, g.streamend, g.buferr, g.dataerr, g.needdict, g.dictset,
           g.smallwin_passes, g.badcode_passes, g.trunc_passes, g.corrupt_passes,
           (unsigned)isaac_ni_hooks.fast_calls, (unsigned)isaac_ni_hooks.flush_calls,
           (unsigned)isaac_ni_hooks.memcpy_calls, (unsigned)isaac_ni_hooks.adler_calls);
#endif
    return 0;
}
