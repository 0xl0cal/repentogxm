/* Guest-side oracle for the native zlib 1.1.4 inflate_codes seam.
 *
 * Drives isaac_vita_native_inflate_codes_try() on a fake CPU over an arena
 * mapped below 4 GiB (guest addresses are host addresses) with the shipped
 * seam linked against a TEST DOUBLE of iz_inflate_codes that records the
 * state it was handed and mutates the guest structures in a scripted way.
 * It proves the seam's own logic independently of zlib: every guard rejects
 * exactly its hostile shape without touching CPU or memory, the check
 * function is swapped for the duration of the call and restored, host-side
 * messages are mapped to the PE's .rdata addresses (unknown ones fault), the
 * census replay counts match the leaf calls and its failures fault, and the
 * epilogue leaves eax/esp as the translated body would.
 *
 * The real core is covered by host_vita_native_inflate_oracle.c (core/e2e). */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include "host_vita_native_inflate.h"
#include "host_vita_native_inflate_hooks.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "zutil.h"       /* iz_ prefix via -include: inflate_codes == iz_inflate_codes */
#include "infblock.h"
#include "inftrees.h"
#include "infcodes.h"
#include "infutil.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "native inflate guest oracle failed at %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

enum {
    ARENA_BYTES     = 0x00020000U,
    BLOCKS_OFFSET   = 0x1000U,
    CODES_OFFSET    = 0x2000U,
    ZSTREAM_OFFSET  = 0x3000U,
    HUFTS_OFFSET    = 0x4000U,           /* 1440 * 8 = 0x2d00 */
    WINDOW_OFFSET   = 0x8000U,           /* 0x8000 bytes */
    INPUT_OFFSET    = 0x10000U,
    OUTPUT_OFFSET   = 0x12000U,
    STACK_OFFSET    = 0x14000U,
    SCRATCH_OFFSET  = 0x16000U,
    WINDOW_BYTES    = 0x8000U,
    INPUT_BYTES     = 0x1000U,
    OUTPUT_BYTES    = 0x1000U
};

static uint8_t *s_arena;
static uint32_t s_base;

typedef struct fixture {
    CPU cpu;
    uint32_t blocks, codes, zstream, hufts, window, input, output, stack, scratch;
} fixture;

/* --- runtime stubs ---------------------------------------------------------- */

static unsigned char s_coverage[16384];
unsigned char *g_guest_coverage_functions = s_coverage;
unsigned char *g_guest_coverage_imports;
unsigned char *g_guest_coverage_cases;

static uint32_t s_fault_calls, s_translated_notes, s_import_notes;
static uint32_t s_fail_translated_note, s_fail_import_note, s_log_calls, s_log_total;

uint32_t gpop_generated(CPU *__restrict c)
{
    uint32_t value = ld32(c->esp);
    c->esp += 4U;
    return value;
}

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    ++s_fault_calls;
    c->fault_addr = address;
    c->fault = what;
}

int guest_note_authenticated_translated_call(uint32_t address, uint32_t coverage_id)
{
    ++s_translated_notes;
    if (address != ISAAC_NI_ADLER32_RVA || coverage_id != ISAAC_NI_ADLER32_COVERAGE)
        return 0;
    return s_fail_translated_note != s_translated_notes;
}

int guest_note_authenticated_import_call(uint32_t slot, uint32_t import_id, uint32_t thunk_id)
{
    ++s_import_notes;
    if (slot != ISAAC_NI_MEMCPY_IAT_RVA || import_id != ISAAC_NI_MEMCPY_IMPORT_ID ||
            thunk_id != ISAAC_NI_MEMCPY_THUNK_COVERAGE)
        return 0;
    return s_fail_import_note != s_import_notes;
}

void isaac_vita_log(const char *format, ...)
{
    va_list args;
    char line[512];
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    ++s_log_calls;
    if (++s_log_total <= 2U)   /* banner + first fallback line as evidence */
        printf("  log: %s\n", line);
}

/* --- the test double of iz_inflate_codes ------------------------------------ */

static struct {
    uint32_t calls;
    uint32_t seen_checkfn;      /* value of s->checkfn observed during the call */
    uint32_t consume_in, produce_out, fast, flush, memcpys, adlers;
    int set_msg;                /* 0 none, 1 bad-dist, 2 bad-litlen, 3 unknown */
    int rc;
} dbl;

int inflate_codes(inflate_blocks_statef *s, z_streamp z, int r)
{
    uint32_t sa = (uint32_t)(uintptr_t)s, za = (uint32_t)(uintptr_t)z;
    uint32_t next_in = ld32(za + ISAAC_NI_Z_NEXT_IN), avail_in = ld32(za + ISAAC_NI_Z_AVAIL_IN);
    uint32_t next_out = ld32(za + ISAAC_NI_Z_NEXT_OUT), avail_out = ld32(za + ISAAC_NI_Z_AVAIL_OUT);
    uint32_t i, k = dbl.consume_in < avail_in ? dbl.consume_in : avail_in;
    uint32_t m = dbl.produce_out < avail_out ? dbl.produce_out : avail_out;
    uint32_t check = ld32(sa + ISAAC_NI_BLK_CHECK);

    ++dbl.calls;
    dbl.seen_checkfn = ld32(sa + ISAAC_NI_BLK_CHECKFN);
    (void)r;
    st32(za + ISAAC_NI_Z_NEXT_IN, next_in + k);
    st32(za + ISAAC_NI_Z_AVAIL_IN, avail_in - k);
    st32(za + ISAAC_NI_Z_TOTAL_IN, ld32(za + ISAAC_NI_Z_TOTAL_IN) + k);
    for (i = 0; i < m; ++i)
        st8(next_out + i, (uint8_t)(0xa5U + i));
    st32(za + ISAAC_NI_Z_NEXT_OUT, next_out + m);
    st32(za + ISAAC_NI_Z_AVAIL_OUT, avail_out - m);
    st32(za + ISAAC_NI_Z_TOTAL_OUT, ld32(za + ISAAC_NI_Z_TOTAL_OUT) + m);
    st32(sa + ISAAC_NI_BLK_BITK, 3U);
    st32(sa + ISAAC_NI_BLK_BITB, 5U);
    st32(ld32(sa + ISAAC_NI_BLK_CODES) + ISAAC_NI_CS_MODE, ISAAC_NI_MODE_LEN);
    /* leaf calls the real core would make: the hooks count them */
    isaac_ni_hooks.fast_calls += dbl.fast;
    isaac_ni_hooks.flush_calls += dbl.flush;
    for (i = 0; i < dbl.memcpys; ++i)
        isaac_ni_hook_zmemcpy((Bytef *)(uintptr_t)next_out, (const Bytef *)(uintptr_t)next_out, 0U);
    for (i = 0; i < dbl.adlers; ++i)
        check = (uint32_t)isaac_ni_hook_adler32(check, (const unsigned char *)(uintptr_t)next_out, m);
    if (dbl.adlers) {
        st32(sa + ISAAC_NI_BLK_CHECK, check);
        st32(za + ISAAC_NI_Z_ADLER, check);
    }
    if (dbl.set_msg) {
        const char *text = dbl.set_msg == 1 ? "invalid distance code"
                         : dbl.set_msg == 2 ? "invalid literal/length code"
                         : "incompatible message";
        uint32_t scratch = s_base + SCRATCH_OFFSET;
        memcpy(s_arena + SCRATCH_OFFSET, text, strlen(text) + 1U);
        st32(za + ISAAC_NI_Z_MSG, scratch);
    }
    return dbl.rc;
}

/* --- arena -------------------------------------------------------------------- */

static int arena_map(void)
{
    static const uint32_t hints[] = { 0x22000000U, 0x32000000U, 0x42000000U, 0x52000000U, 0x62000000U };
    size_t i;
    for (i = 0; i < sizeof hints / sizeof hints[0]; ++i) {
        void *mapped;
#if defined(_WIN32)
        mapped = VirtualAlloc((void *)(uintptr_t)hints[i], ARENA_BYTES,
                              MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (mapped == NULL) continue;
#else
        int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_32BIT)
        flags |= MAP_32BIT;
#endif
        mapped = mmap((void *)(uintptr_t)hints[i], ARENA_BYTES, PROT_READ | PROT_WRITE, flags, -1, 0);
        if (mapped == MAP_FAILED) continue;
#endif
        if ((uintptr_t)mapped > UINT32_MAX - ARENA_BYTES) {
#if defined(_WIN32)
            VirtualFree(mapped, 0U, MEM_RELEASE);
#else
            munmap(mapped, ARENA_BYTES);
#endif
            continue;
        }
        s_arena = (uint8_t *)mapped;
        s_base = (uint32_t)(uintptr_t)mapped;
        return 1;
    }
    return 0;
}

static void reset_runtime(void)
{
    s_fault_calls = s_translated_notes = s_import_notes = 0U;
    s_fail_translated_note = s_fail_import_note = 0U;
    s_log_calls = 0U;
    memset(s_coverage, 0, sizeof s_coverage);
    memset(&isaac_ni_hooks, 0, sizeof isaac_ni_hooks);
    memset(&dbl, 0, sizeof dbl);
    dbl.consume_in = 7U; dbl.produce_out = 40U; dbl.fast = 1U; dbl.flush = 2U;
    dbl.memcpys = 3U; dbl.adlers = 3U; dbl.rc = Z_OK;
    isaac_vita_native_inflate_stats_reset();
}

static void setup(fixture *f)
{
    uint32_t i;
    memset(s_arena, 0x6d, ARENA_BYTES);
    reset_runtime();
    memset(f, 0, sizeof *f);
    f->blocks = s_base + BLOCKS_OFFSET;
    f->codes = s_base + CODES_OFFSET;
    f->zstream = s_base + ZSTREAM_OFFSET;
    f->hufts = s_base + HUFTS_OFFSET;
    f->window = s_base + WINDOW_OFFSET;
    f->input = s_base + INPUT_OFFSET;
    f->output = s_base + OUTPUT_OFFSET;
    f->stack = s_base + STACK_OFFSET + 0x40U;
    f->scratch = s_base + SCRATCH_OFFSET;

    f->cpu.ecx = f->blocks;
    f->cpu.edx = f->zstream;
    f->cpu.esp = f->stack;
    f->cpu.stack_owner = &f->cpu;
    f->cpu.stack_floor = f->stack - 0x40U;
    f->cpu.stack_ceiling = f->stack + 0x80U;
    f->cpu.stack_low_water = f->cpu.stack_floor;
    st32(f->stack, (uint32_t)ISAAC_NI_CODES_RETURN_RVA /* RVA, as the corpus pushes it */);
    st32(f->stack + 4U, (uint32_t)Z_BUF_ERROR);

    /* a live CODES-mode block state mid-stream */
    st32(f->blocks + ISAAC_NI_BLK_MODE, 6U);              /* CODES */
    st32(f->blocks + ISAAC_NI_BLK_CODES, f->codes);
    st32(f->blocks + ISAAC_NI_BLK_BITK, 5U);
    st32(f->blocks + ISAAC_NI_BLK_BITB, 0x13U);
    st32(f->blocks + ISAAC_NI_BLK_HUFTS, f->hufts);
    st32(f->blocks + ISAAC_NI_BLK_WINDOW, f->window);
    st32(f->blocks + ISAAC_NI_BLK_END, f->window + WINDOW_BYTES);
    st32(f->blocks + ISAAC_NI_BLK_READ, f->window + 0x100U);
    st32(f->blocks + ISAAC_NI_BLK_WRITE, f->window + 0x180U);
    st32(f->blocks + ISAAC_NI_BLK_CHECKFN, (uint32_t)GUEST_IMAGE_BASE + ISAAC_NI_ADLER32_RVA);
    st32(f->blocks + ISAAC_NI_BLK_CHECK, 0x00010001U);

    st32(f->codes + ISAAC_NI_CS_MODE, ISAAC_NI_MODE_LEN);
    st32(f->codes + ISAAC_NI_CS_LEN, 0U);
    st32(f->codes + ISAAC_NI_CS_SUB0, f->hufts + 0x40U);   /* sub.code.tree */
    st32(f->codes + ISAAC_NI_CS_SUB1, 9U);                 /* sub.code.need */
    st8(f->codes + ISAAC_NI_CS_LBITS, 9U);
    st8(f->codes + ISAAC_NI_CS_DBITS, 6U);
    st32(f->codes + ISAAC_NI_CS_LTREE, f->hufts);
    st32(f->codes + ISAAC_NI_CS_DTREE, f->hufts + 0x1000U);

    st32(f->zstream + ISAAC_NI_Z_NEXT_IN, f->input);
    st32(f->zstream + ISAAC_NI_Z_AVAIL_IN, 100U);
    st32(f->zstream + ISAAC_NI_Z_TOTAL_IN, 1000U);
    st32(f->zstream + ISAAC_NI_Z_NEXT_OUT, f->output);
    st32(f->zstream + ISAAC_NI_Z_AVAIL_OUT, 64U);
    st32(f->zstream + ISAAC_NI_Z_TOTAL_OUT, 2000U);
    st32(f->zstream + ISAAC_NI_Z_MSG, 0U);
    st32(f->zstream + ISAAC_NI_Z_STATE, 0U);
    st32(f->zstream + ISAAC_NI_Z_ADLER, 0x00010001U);
    for (i = 0; i < WINDOW_BYTES; ++i)
        st8(f->window + i, (uint8_t)(i * 13U + 7U));
    memset(s_arena + OUTPUT_OFFSET, 0xcc, OUTPUT_BYTES);
}

/* --- cases -------------------------------------------------------------------- */

static int run_success_case(void)
{
    fixture f;
    const isaac_vita_native_inflate_stats *st;
    uint32_t i;

    setup(&f);
    CHECK(isaac_vita_native_inflate_codes_try(&f.cpu) == 1);
    CHECK(f.cpu.fault == NULL && s_fault_calls == 0U);
    CHECK(dbl.calls == 1U);
    /* the check function was the native hook during the call and is restored */
    CHECK(dbl.seen_checkfn == (uint32_t)(uintptr_t)&isaac_ni_hook_adler32);
    CHECK(ld32(f.blocks + ISAAC_NI_BLK_CHECKFN) == (uint32_t)GUEST_IMAGE_BASE + ISAAC_NI_ADLER32_RVA);
    /* epilogue: eax = result, return word popped, r left for the caller */
    CHECK(f.cpu.eax == (uint32_t)Z_OK && f.cpu.esp == f.stack + 4U);
    CHECK(ld32(f.cpu.esp) == (uint32_t)Z_BUF_ERROR);
    CHECK(f.cpu.ecx == f.blocks && f.cpu.edx == f.zstream);
    /* the double's mutations reached guest memory */
    CHECK(ld32(f.zstream + ISAAC_NI_Z_NEXT_IN) == f.input + 7U);
    CHECK(ld32(f.zstream + ISAAC_NI_Z_AVAIL_IN) == 93U);
    CHECK(ld32(f.zstream + ISAAC_NI_Z_TOTAL_IN) == 1007U);
    CHECK(ld32(f.zstream + ISAAC_NI_Z_NEXT_OUT) == f.output + 40U);
    CHECK(ld32(f.zstream + ISAAC_NI_Z_AVAIL_OUT) == 24U);
    CHECK(ld32(f.zstream + ISAAC_NI_Z_TOTAL_OUT) == 2040U);
    for (i = 0; i < 40U; ++i)
        CHECK(ld8(f.output + i) == (uint8_t)(0xa5U + i));
    CHECK(ld8(f.output + 40U) == 0xccU);
    CHECK(ld32(f.zstream + ISAAC_NI_Z_MSG) == 0U);
    /* census replay: 3 adler notes, 3 memcpy notes, three coverage bits */
    CHECK(s_translated_notes == 3U && s_import_notes == 3U);
    CHECK(s_coverage[ISAAC_NI_INFLATE_CODES_COVERAGE] == 1U);
    CHECK(s_coverage[ISAAC_NI_INFLATE_FAST_COVERAGE] == 1U);
    CHECK(s_coverage[ISAAC_NI_INFLATE_FLUSH_COVERAGE] == 1U);
    CHECK(s_coverage[ISAAC_NI_ADLER32_COVERAGE] == 0U);     /* notes, not direct */
    st = isaac_vita_native_inflate_stats_get();
    CHECK(st->calls == 1U && st->handled == 1U && st->fallbacks == 0U);
    CHECK(st->fast_calls == 1U && st->flush_calls == 2U && st->memcpy_calls == 3U && st->adler_calls == 3U);
    CHECK(st->msg_mapped == 0U && st->faults == 0U && st->last_reason == ISAAC_NI_HANDLED);
    CHECK(s_log_calls == 2U);   /* banner + the first-native stats line */
    return 0;
}

static int run_no_checkfn_case(void)
{
    fixture f;
    setup(&f);
    st32(f.blocks + ISAAC_NI_BLK_CHECKFN, 0U);
    dbl.adlers = 0U;
    CHECK(isaac_vita_native_inflate_codes_try(&f.cpu) == 1);
    CHECK(dbl.seen_checkfn == 0U && ld32(f.blocks + ISAAC_NI_BLK_CHECKFN) == 0U);
    CHECK(s_translated_notes == 0U && s_import_notes == 3U);
    CHECK(f.cpu.eax == (uint32_t)Z_OK && f.cpu.esp == f.stack + 4U);
    return 0;
}

static int run_no_leaf_case(void)
{
    fixture f;
    setup(&f);
    dbl.fast = 0U; dbl.flush = 0U; dbl.memcpys = 0U; dbl.adlers = 0U; dbl.rc = Z_STREAM_END;
    CHECK(isaac_vita_native_inflate_codes_try(&f.cpu) == 1);
    CHECK(s_translated_notes == 0U && s_import_notes == 0U);
    CHECK(s_coverage[ISAAC_NI_INFLATE_CODES_COVERAGE] == 1U);
    CHECK(s_coverage[ISAAC_NI_INFLATE_FAST_COVERAGE] == 0U);
    CHECK(s_coverage[ISAAC_NI_INFLATE_FLUSH_COVERAGE] == 0U);
    CHECK(f.cpu.eax == (uint32_t)Z_STREAM_END);
    return 0;
}

static int run_fixed_tree_case(void)
{
    fixture f;
    setup(&f);
    st32(f.codes + ISAAC_NI_CS_LTREE, (uint32_t)GUEST_IMAGE_BASE + ISAAC_NI_FIXED_TL_RVA);
    st32(f.codes + ISAAC_NI_CS_DTREE, (uint32_t)GUEST_IMAGE_BASE + ISAAC_NI_FIXED_TD_RVA + 0xf8U);
    st32(f.codes + ISAAC_NI_CS_SUB0, (uint32_t)GUEST_IMAGE_BASE + ISAAC_NI_FIXED_TL_RVA + 0xff8U);
    CHECK(isaac_vita_native_inflate_codes_try(&f.cpu) == 1);
    CHECK(dbl.calls == 1U);
    /* other codes modes: LENEXT (sub.copy.get <= 16), COPY, LIT, WASH, END, BADCODE */
    setup(&f); st32(f.codes + ISAAC_NI_CS_MODE, ISAAC_NI_MODE_LENEXT); st32(f.codes + ISAAC_NI_CS_SUB0, 16U);
    CHECK(isaac_vita_native_inflate_codes_try(&f.cpu) == 1);
    setup(&f); st32(f.codes + ISAAC_NI_CS_MODE, 5U); st32(f.codes + ISAAC_NI_CS_SUB0, 0xffffffffU); st32(f.codes + ISAAC_NI_CS_SUB1, 0xffffffffU);
    CHECK(isaac_vita_native_inflate_codes_try(&f.cpu) == 1);
    setup(&f); st32(f.codes + ISAAC_NI_CS_MODE, ISAAC_NI_MODE_BADCODE);
    CHECK(isaac_vita_native_inflate_codes_try(&f.cpu) == 1);
    /* smallest window, cursors at the inclusive end, empty in/out with null pointers */
    setup(&f);
    st32(f.blocks + ISAAC_NI_BLK_END, f.window + ISAAC_NI_WINDOW_MIN);
    st32(f.blocks + ISAAC_NI_BLK_READ, f.window + ISAAC_NI_WINDOW_MIN);
    st32(f.blocks + ISAAC_NI_BLK_WRITE, f.window + ISAAC_NI_WINDOW_MIN);
    st32(f.zstream + ISAAC_NI_Z_NEXT_IN, 0U); st32(f.zstream + ISAAC_NI_Z_AVAIL_IN, 0U);
    st32(f.zstream + ISAAC_NI_Z_NEXT_OUT, 0U); st32(f.zstream + ISAAC_NI_Z_AVAIL_OUT, 0U);
    dbl.consume_in = 0U; dbl.produce_out = 0U;
    CHECK(isaac_vita_native_inflate_codes_try(&f.cpu) == 1);
    return 0;
}

static int run_msg_cases(void)
{
    fixture f;
    const isaac_vita_native_inflate_stats *st;

    setup(&f); dbl.set_msg = 1; dbl.rc = Z_DATA_ERROR;
    CHECK(isaac_vita_native_inflate_codes_try(&f.cpu) == 1);
    CHECK(ld32(f.zstream + ISAAC_NI_Z_MSG) == (uint32_t)GUEST_IMAGE_BASE + ISAAC_NI_MSG_BAD_DIST_RVA);
    CHECK(f.cpu.eax == (uint32_t)Z_DATA_ERROR && s_fault_calls == 0U);
    CHECK(isaac_vita_native_inflate_stats_get()->msg_mapped == 1U);

    setup(&f); dbl.set_msg = 2; dbl.rc = Z_DATA_ERROR;
    CHECK(isaac_vita_native_inflate_codes_try(&f.cpu) == 1);
    CHECK(ld32(f.zstream + ISAAC_NI_Z_MSG) == (uint32_t)GUEST_IMAGE_BASE + ISAAC_NI_MSG_BAD_LITLEN_RVA);

    /* a message already set by the translated side stays untouched */
    setup(&f); st32(f.zstream + ISAAC_NI_Z_MSG, (uint32_t)GUEST_IMAGE_BASE + ISAAC_NI_MSG_BAD_DIST_RVA);
    CHECK(isaac_vita_native_inflate_codes_try(&f.cpu) == 1);
    CHECK(ld32(f.zstream + ISAAC_NI_Z_MSG) == (uint32_t)GUEST_IMAGE_BASE + ISAAC_NI_MSG_BAD_DIST_RVA);
    CHECK(isaac_vita_native_inflate_stats_get()->msg_mapped == 0U);

    /* an unknown message means the core drifted: fault, return word consumed */
    setup(&f); dbl.set_msg = 3; dbl.rc = Z_DATA_ERROR;
    CHECK(isaac_vita_native_inflate_codes_try(&f.cpu) == 1);
    CHECK(s_fault_calls == 1U && f.cpu.fault != NULL && f.cpu.fault_addr == f.zstream);
    CHECK(f.cpu.esp == f.stack + 4U);
    st = isaac_vita_native_inflate_stats_get();
    CHECK(st->faults == 1U && st->handled == 0U);
    return 0;
}

static int run_census_fault_cases(void)
{
    fixture f;

    setup(&f); s_fail_translated_note = 2U;
    CHECK(isaac_vita_native_inflate_codes_try(&f.cpu) == 1);
    CHECK(s_fault_calls == 1U && f.cpu.fault_addr == ISAAC_NI_ADLER32_RVA);
    CHECK(f.cpu.esp == f.stack + 4U && s_translated_notes == 2U && s_import_notes == 0U);

    setup(&f); s_fail_import_note = 3U;
    CHECK(isaac_vita_native_inflate_codes_try(&f.cpu) == 1);
    CHECK(s_fault_calls == 1U && f.cpu.fault_addr == ISAAC_NI_MEMCPY_IAT_RVA);
    CHECK(f.cpu.esp == f.stack + 4U && s_translated_notes == 3U && s_import_notes == 3U);
    CHECK(isaac_vita_native_inflate_stats_get()->faults == 1U);
    return 0;
}

static int rejected(fixture *f, uint32_t reason)
{
    CPU before;
    static uint8_t arena_before[ARENA_BYTES];
    const isaac_vita_native_inflate_stats *st;
    memcpy(&before, &f->cpu, sizeof before);
    memcpy(arena_before, s_arena, ARENA_BYTES);
    if (isaac_vita_native_inflate_codes_try(&f->cpu) != 0) return 0;
    if (memcmp(&f->cpu, &before, sizeof before) != 0) return 0;
    if (memcmp(s_arena, arena_before, ARENA_BYTES) != 0) return 0;
    if (dbl.calls != 0U || s_fault_calls != 0U || s_translated_notes != 0U || s_import_notes != 0U) return 0;
    st = isaac_vita_native_inflate_stats_get();
    return st->fallbacks == 1U && st->handled == 0U && st->last_reason == reason &&
           st->reasons[reason] == 1U;
}

static int run_rejections(void)
{
    fixture f;
    uint32_t hostile = 0U;

#define REJECT(reason, change) do { \
    setup(&f); change; \
    CHECK(rejected(&f, (reason))); \
    ++hostile; \
} while (0)

    /* stack: esp outside the bound stack / no room for [ret, r] */
    REJECT(ISAAC_NI_REASON_STACK, f.cpu.esp = f.cpu.stack_ceiling - 4U);
    REJECT(ISAAC_NI_REASON_STACK, f.cpu.stack_owner = NULL);
    REJECT(ISAAC_NI_REASON_SITE, st32(f.stack, (uint32_t)ISAAC_NI_CODES_RETURN_RVA + 1U));
    REJECT(ISAAC_NI_REASON_SITE, st32(f.stack, 0x985cf080U));   /* the image VA, not the RVA the corpus pushes */
    REJECT(ISAAC_NI_REASON_STATE_PTR, f.cpu.ecx = 0U);
    REJECT(ISAAC_NI_REASON_STATE_PTR, f.cpu.ecx = f.blocks + 2U);
    REJECT(ISAAC_NI_REASON_STATE_PTR, f.cpu.edx = 0U);
    REJECT(ISAAC_NI_REASON_STATE_PTR, f.cpu.edx = f.zstream + 1U);
    REJECT(ISAAC_NI_REASON_CODES_PTR, st32(f.blocks + ISAAC_NI_BLK_CODES, 0U));
    REJECT(ISAAC_NI_REASON_CODES_PTR, st32(f.blocks + ISAAC_NI_BLK_CODES, f.codes + 1U));
    REJECT(ISAAC_NI_REASON_MODE, st32(f.codes + ISAAC_NI_CS_MODE, 10U));
    REJECT(ISAAC_NI_REASON_MODE, st32(f.codes + ISAAC_NI_CS_MODE, 0x100U));   /* byte-visible mode with dirty upper bytes */
    REJECT(ISAAC_NI_REASON_WINDOW, st32(f.blocks + ISAAC_NI_BLK_WINDOW, 0U));
    REJECT(ISAAC_NI_REASON_WINDOW, st32(f.blocks + ISAAC_NI_BLK_END, f.window));
    REJECT(ISAAC_NI_REASON_WINDOW, st32(f.blocks + ISAAC_NI_BLK_END, f.window + 0x8001U));
    REJECT(ISAAC_NI_REASON_WINDOW, st32(f.blocks + ISAAC_NI_BLK_END, f.window + 0x80U));
    REJECT(ISAAC_NI_REASON_WINDOW, st32(f.blocks + ISAAC_NI_BLK_END, f.window + 0x10000U));
    REJECT(ISAAC_NI_REASON_WINDOW, st32(f.blocks + ISAAC_NI_BLK_END, f.window + 0x6000U));
    REJECT(ISAAC_NI_REASON_CURSOR, st32(f.blocks + ISAAC_NI_BLK_READ, f.window - 1U));
    REJECT(ISAAC_NI_REASON_CURSOR, st32(f.blocks + ISAAC_NI_BLK_READ, f.window + WINDOW_BYTES + 1U));
    REJECT(ISAAC_NI_REASON_CURSOR, st32(f.blocks + ISAAC_NI_BLK_WRITE, f.window - 4U));
    REJECT(ISAAC_NI_REASON_CURSOR, st32(f.blocks + ISAAC_NI_BLK_WRITE, f.window + WINDOW_BYTES + 4U));
    REJECT(ISAAC_NI_REASON_HUFTS, st32(f.blocks + ISAAC_NI_BLK_HUFTS, 0U));
    REJECT(ISAAC_NI_REASON_HUFTS, st32(f.blocks + ISAAC_NI_BLK_HUFTS, f.hufts + 2U));
    REJECT(ISAAC_NI_REASON_HUFTS, st32(f.blocks + ISAAC_NI_BLK_HUFTS, 0xffffe000U));
    REJECT(ISAAC_NI_REASON_CHECKFN, st32(f.blocks + ISAAC_NI_BLK_CHECKFN, (uint32_t)GUEST_IMAGE_BASE + ISAAC_NI_ADLER32_RVA + 4U));
    REJECT(ISAAC_NI_REASON_CHECKFN, st32(f.blocks + ISAAC_NI_BLK_CHECKFN, (uint32_t)(uintptr_t)&isaac_ni_hook_adler32));
    REJECT(ISAAC_NI_REASON_BITS, st8(f.codes + ISAAC_NI_CS_LBITS, 17U));
    REJECT(ISAAC_NI_REASON_BITS, st8(f.codes + ISAAC_NI_CS_DBITS, 17U));
    REJECT(ISAAC_NI_REASON_TREE, st32(f.codes + ISAAC_NI_CS_LTREE, f.hufts + 2U));
    REJECT(ISAAC_NI_REASON_TREE, st32(f.codes + ISAAC_NI_CS_LTREE, f.hufts + ISAAC_NI_MANY * 8U));
    REJECT(ISAAC_NI_REASON_TREE, st32(f.codes + ISAAC_NI_CS_LTREE, f.hufts - 8U));
    REJECT(ISAAC_NI_REASON_TREE, st32(f.codes + ISAAC_NI_CS_DTREE, f.window));
    REJECT(ISAAC_NI_REASON_TREE, st32(f.codes + ISAAC_NI_CS_DTREE, (uint32_t)GUEST_IMAGE_BASE + ISAAC_NI_FIXED_TL_RVA + 0x1000U));
    REJECT(ISAAC_NI_REASON_TREE, st32(f.codes + ISAAC_NI_CS_DTREE, (uint32_t)GUEST_IMAGE_BASE + ISAAC_NI_FIXED_TD_RVA - 8U));
    REJECT(ISAAC_NI_REASON_SUBTREE, st32(f.codes + ISAAC_NI_CS_SUB0, f.hufts + ISAAC_NI_MANY * 8U));
    REJECT(ISAAC_NI_REASON_SUBTREE, st32(f.codes + ISAAC_NI_CS_SUB1, 17U));
    REJECT(ISAAC_NI_REASON_SUBTREE, (st32(f.codes + ISAAC_NI_CS_MODE, ISAAC_NI_MODE_DIST), st32(f.codes + ISAAC_NI_CS_SUB0, 0U)));
    REJECT(ISAAC_NI_REASON_EXTRA, (st32(f.codes + ISAAC_NI_CS_MODE, ISAAC_NI_MODE_LENEXT), st32(f.codes + ISAAC_NI_CS_SUB0, 17U)));
    REJECT(ISAAC_NI_REASON_EXTRA, (st32(f.codes + ISAAC_NI_CS_MODE, ISAAC_NI_MODE_DISTEXT), st32(f.codes + ISAAC_NI_CS_SUB0, 0x80000000U)));
    REJECT(ISAAC_NI_REASON_INPUT, st32(f.zstream + ISAAC_NI_Z_NEXT_IN, 0U));
    REJECT(ISAAC_NI_REASON_INPUT, st32(f.zstream + ISAAC_NI_Z_NEXT_IN, 0xffffffc0U));
    REJECT(ISAAC_NI_REASON_OUTPUT, st32(f.zstream + ISAAC_NI_Z_NEXT_OUT, 0U));
    REJECT(ISAAC_NI_REASON_OUTPUT, st32(f.zstream + ISAAC_NI_Z_AVAIL_OUT, 0xffffffffU));
#undef REJECT
    printf("  hostile shapes rejected: %u\n", (unsigned)hostile);
    return hostile == 44U ? 0 : 1;
}

static int run_stats_period_case(void)
{
    /* 0x10000 handled calls print one stats line and keep counting */
    fixture f;
    uint32_t i;
    setup(&f);
    dbl.consume_in = 0U; dbl.produce_out = 0U; dbl.fast = 0U; dbl.flush = 0U; dbl.memcpys = 0U; dbl.adlers = 0U;
    for (i = 0; i < 0x10000U; ++i) {
        f.cpu.esp = f.stack;
        st32(f.stack, (uint32_t)ISAAC_NI_CODES_RETURN_RVA);
        CHECK(isaac_vita_native_inflate_codes_try(&f.cpu) == 1);
    }
    CHECK(isaac_vita_native_inflate_stats_get()->handled == 0x10000U);
    CHECK(s_log_calls == 3U);   /* banner + first-native line + the period line */
    return 0;
}

int main(void)
{
    CHECK(arena_map());
    CHECK(run_success_case() == 0);
    CHECK(run_no_checkfn_case() == 0);
    CHECK(run_no_leaf_case() == 0);
    CHECK(run_fixed_tree_case() == 0);
    CHECK(run_msg_cases() == 0);
    CHECK(run_census_fault_cases() == 0);
    CHECK(run_rejections() == 0);
    CHECK(run_stats_period_case() == 0);
    printf("native inflate guest oracle: PASS; hostile=44 msg=4 census-faults=2\n");
    return 0;
}
