/* Guest-code sampling profiler (ISAAC_VITA_GUEST_SAMPLER).
 *
 * Why: with the native render surface the GPU stopped being the limiter and
 * the remaining frame time is translated x86 code on the A9 (Game::Update p95
 * 18-30 ms in fights, Brimstone at 33-40 FPS), but nothing said WHICH of the
 * 15,036 translated functions.  This is the cheapest attribution that needs
 * no codegen change: a thread on another core samples the guest stack.
 *
 * Mechanism.  The translated code does not expose EIP, but every translated
 * call site pushes its bare return RVA onto the guest stack before entering
 * the callee (gpush_generated(c, next_rva) in emit.py), and the guest stack is
 * one fixed identity-mapped allocation whose bounds live in the CPU struct.
 * Every millisecond the sampler reads the game thread's ESP, walks upward and
 * takes the first word that is a *validated* x86 return site: the bytes
 * before it in the mapped PE image (GUEST_IMAGE_BASE + rva) must decode to a
 * direct `call rel32` whose target is a registered function start, or to an
 * indirect `call r/m32` (FF /2) of exactly that length.  Plain range checks
 * are not enough: any small integer on the stack looks like an RVA.
 *
 * Two views are recorded per sample, both function-granular via the sorted
 * table gen_all.py emits into guest_table.c (guest_registered_addresses):
 *   self   = the callee of that innermost call site.  For a direct call this
 *            is the function running right now (or one it has since entered
 *            without a marker above ESP, see bias).  An indirect site (vtable
 *            call, `call [IAT]`) names its callee from the word guest.c
 *            publishes, g_kage_guest_last_indirect_target: a registered
 *            translated start becomes that function, an IAT slot or GL token
 *            becomes "ext" (native host code), zero (no live indirect
 *            dispatch) stays "ind".
 *   caller = the function containing the call site, i.e. the frame that is
 *            waiting for the current callee.
 *
 * Publish/restore contract for the indirect word (research-sampler-hotlists
 * finding 9).  Every route that executes an indirect `call r/m32` on the game
 * thread stores its target on entry and restores the enclosing value when the
 * callee returns: guest_call does it in a wrapper around its dispatch body,
 * guest_try_direct_sync_import_call (Enter/LeaveCriticalSection, which never
 * enters guest_call) does it around the sync endpoint, and guest_import_call
 * (ISAAC_VITA_IMPORT_DIRECT: every generated call/jmp [IAT] site, which
 * enters guest_call only on its fail-closed fallback) does it around the
 * validated family endpoint -- before that restore every translated body
 * that kept running after a direct import returned was named "ext" until its
 * enclosing guest_call returned.  Before the fast path
 * published, the ~2k lock/unlock host calls per title frame were labelled
 * with the previous vtable target (9.7 % of render "self" on the
 * 2-instruction getter sub_005a0de0, 23 % on the wrapper bodies); before the
 * restore, code running after a finished indirect callee inherited that
 * callee's name.  Not covered: the eleven generated ReferenceCount direct
 * edges (gen_all render_vita_refcount_direct_edge) call their pinned target
 * without publishing, so time in those bodies is named by the enclosing live
 * indirect callee (their caller), one level up.
 *
 * Bias, deliberately accepted.  (1) The innermost marker names the *call*,
 * not the leaf: with GPR locals c->esp is only published at call/ret
 * boundaries, so a leaf that has not called yet is found at [c->esp] exactly
 * (no stale words below it), but a frame whose callee has returned may keep
 * stale markers from an earlier, deeper call tree in its uninitialised
 * locals, and the scan attributes such a sample to that dead subtree.  The
 * two remedies evaluated for finding 9(3) are not taken: clearing the popped
 * slot on `ret` is an emitter change (one extra store at 142,753 direct
 * sites, all five emitter pins) and bounding the scan by the EBP frame chain
 * is unsafe here (c->ebp is flushed only at call/ret boundaries, so a leaf
 * that has not called yet still shows its caller's frame, and MSVC /Oy leaves
 * frame-pointer-free hot leaves whose EBP is a general register), so it
 * would trade one bias for another that no host test can bound.  What is
 * cheap and safe: the scan covers 256 words (1 KiB) instead of 96, so frames
 * with more than 384 bytes of locals (Room::Render-sized bodies) no longer
 * fall out as "nomark"; `deep` in ph120.hs counts samples the old bound
 * would have lost.  (2) ESP and the stack words are read cross-core without
 * a lock; torn or mid-push values produce a missed or misattributed sample,
 * never a fault, because every dereference is bounded by the stack
 * floor/ceiling and the image size.  (3) A native host import running on the
 * game thread is attributed to its translated caller (caller view) and to
 * "ext" (self view).  (4) Time the game thread spends descheduled still
 * counts as a sample of wherever it stopped; the scheduler's pacing sleep has
 * its own bucket ("pace") so it does not count as update CPU.
 *
 * Output per 120-loop window (after ph120.t, each record < 384 bytes):
 *   ph120.hot  bid= win= loops= phase=P samples=N top=rva:count,... (self,
 *              top 12, rva is the function start in hex, "ind" = indirect
 *              with no live published target, "ext" = native code)
 *   ph120.hotc same for the caller view
 *   ph120.hs   totals: samples per phase (svc,upd,rnd,swp,lim,oth,pace),
 *              no-marker, bad-esp, hash-full, words scanned, deep (marker
 *              found beyond the former 96-word bound)
 * The aggregation is a fixed-size open-addressed hash of
 * (view, phase, function) -> count, double-buffered so the reporter never
 * blocks the sampler and the sampler never writes the buffer being printed.
 *
 * Host oracle (ISAAC_KAGE_VITA_GUEST_SAMPLER_ORACLE): the same attribution
 * code runs on the build box against an array-backed guest stack and PE image
 * (kage_vita_guest_sampler_oracle.c); only the SceKernel thread and the
 * memory accessors differ. */
#include "kage_vita_guest_sampler.h"
#include "guest.h"

#include <string.h>

#if defined(ISAAC_KAGE_VITA_GUEST_SAMPLER_ORACLE)
/* Host oracle: no SceKernel.  The oracle owns the guest stack and the PE
 * image as arrays and drives kvgs_sample() directly; the sampler thread and
 * the production start routine are compiled out. */
#include <stddef.h>
typedef int SceUID;
int sceClibPrintf(const char *format, ...);
int sceClibSnprintf(char *buffer, size_t size, const char *format, ...);
uint32_t kvgs_oracle_stack_word(uint32_t address);
unsigned kvgs_oracle_image_byte(uint32_t rva);
# define KVGS_STACK_WORD(address) kvgs_oracle_stack_word((address))
# define KVGS_IMAGE_BYTE(rva) kvgs_oracle_image_byte((rva))
#else
#include <psp2/kernel/clib.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
/* Cross-core reads of the game thread's stack and of the mapped PE image. */
# define KVGS_STACK_WORD(address) \
    (*(volatile const uint32_t *)(uintptr_t)(address))
# define KVGS_IMAGE_BYTE(rva) \
    (((const unsigned char *)(uintptr_t)GUEST_IMAGE_BASE)[(rva)])
#endif
/* Window dumps (ph120.hot/hotc/hs) are one-call-one-record lines on the
 * game thread; ISAAC_VITA_LOG_ASYNC hands them to the logger thread. */
#if defined(ISAAC_VITA_LOG_ASYNC)
# include "host_vita_log_async.h"
#else
# define ISAAC_VITA_LOG_PRINTF sceClibPrintf
#endif

void isaac_vita_log(const char *format, ...);

#ifndef ISAAC_VITA_GUEST_SAMPLER_BUILD_ID
# define ISAAC_VITA_GUEST_SAMPLER_BUILD_ID "sampler:unstamped"
#endif

#define KVGS_PERIOD_US        1000U
#define KVGS_MIN_SLEEP_US     100U
/* 256 words = 1 KiB above ESP.  The former bound (96 words) lost every sample
 * whose innermost live frame carried more than 384 bytes of locals; `deep`
 * counts how many samples the extension recovers. */
#define KVGS_STACK_SCAN_WORDS 256U
#define KVGS_STACK_SCAN_WORDS_LEGACY 96U
#define KVGS_SLOT_BITS        13U
#define KVGS_SLOTS            (1U << KVGS_SLOT_BITS)
#define KVGS_PROBE_LIMIT      32U
#define KVGS_TOP              12U
#define KVGS_THREAD_PRIORITY  96
#define KVGS_THREAD_STACK     0x4000U
#define KVGS_THREAD_AFFINITY  SCE_KERNEL_CPU_MASK_USER_1
#define KVGS_TEXT_BEGIN_RVA   UINT32_C(0x00001000)
/* Frozen PE: .text ends at 0x605b34, the IAT starts at 0x6060f8. */
#define KVGS_TEXT_END_RVA     UINT32_C(0x00606000)
#define KVGS_RVA_UNKNOWN      UINT32_C(0x0fffffff)
/* An indirect call whose published target is not a translated function:
 * an IAT import, a GL/dynamic token, i.e. native host code (vitaGL, CRT). */
#define KVGS_RVA_EXTERNAL     UINT32_C(0x0ffffffe)
#define KVGS_REPORT_SPIN      200000U

/* Key layout: bit 31 view (0 self, 1 caller), bits 30..28 phase, bits 27..0
 * function RVA (KVGS_RVA_UNKNOWN for an indirect callee).  Zero is never a
 * live key because every function RVA is at least KVGS_TEXT_BEGIN_RVA. */
#define KVGS_KEY(view, phase, rva) \
    ((uint32_t)(view) << 31 | (uint32_t)(phase) << 28 | (uint32_t)(rva))
#define KVGS_KEY_VIEW(key)  ((key) >> 31)
#define KVGS_KEY_PHASE(key) (((key) >> 28) & 7U)
#define KVGS_KEY_RVA(key)   ((key) & KVGS_RVA_UNKNOWN)

_Static_assert(KAGE_VITA_GUEST_SAMPLER_PHASE_COUNT <= 8U,
               "the sampler key holds at most eight phases");

typedef struct kvgs_entry {
    uint32_t key;
    uint32_t count;
} kvgs_entry;

typedef struct kvgs_buffer {
    kvgs_entry slots[KVGS_SLOTS];
    uint32_t samples[KAGE_VITA_GUEST_SAMPLER_PHASE_COUNT];
    uint32_t no_marker;
    uint32_t bad_esp;
    uint32_t hash_full;
    uint32_t words_scanned;
    uint32_t deep;
} kvgs_buffer;

volatile uint32_t g_kage_vita_guest_sampler_phase =
    KAGE_VITA_GUEST_SAMPLER_OTH;

static kvgs_buffer s_buffers[2];
static uint32_t s_active;          /* buffer index the sampler writes */
static uint32_t s_busy;            /* sampler is inside one sample */
static CPU *s_cpu;
static const uint32_t *s_functions;
static uint32_t s_function_count;
static uint32_t s_text_present;    /* mapped .text bytes look real */
static SceUID s_thread = -1;
#if !defined(ISAAC_KAGE_VITA_GUEST_SAMPLER_ORACLE)
static uint32_t s_started;          /* production start guard */
#endif
static uint32_t s_ticks;           /* samples attempted, lifetime */

static const char *const s_phase_names[KAGE_VITA_GUEST_SAMPLER_PHASE_COUNT] =
    { "svc", "upd", "rnd", "swp", "lim", "oth", "pace" };

/* ------------------------------------------------------------ lookups --- */

/* Greatest registered function start <= rva, or KVGS_RVA_UNKNOWN. */
static uint32_t kvgs_function_containing(uint32_t rva)
{
    uint32_t lo = 0U;
    uint32_t hi = s_function_count;

    if (!hi || rva < s_functions[0])
        return KVGS_RVA_UNKNOWN;
    while (hi - lo > 1U) {
        uint32_t mid = lo + (hi - lo) / 2U;
        if (s_functions[mid] <= rva)
            lo = mid;
        else
            hi = mid;
    }
    return s_functions[lo];
}

static int kvgs_function_start_exact(uint32_t rva)
{
    uint32_t lo = 0U;
    uint32_t hi = s_function_count;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2U;
        if (s_functions[mid] == rva)
            return 1;
        if (s_functions[mid] < rva)
            lo = mid + 1U;
        else
            hi = mid;
    }
    return 0;
}

/* Length of an FF /2 (call r/m32) instruction with this ModRM/SIB, or 0. */
static uint32_t kvgs_ff_call_length(unsigned modrm, unsigned sib)
{
    unsigned mod = modrm >> 6;
    unsigned rm = modrm & 7U;
    uint32_t length = 2U;

    if ((modrm & 0x38U) != 0x10U)
        return 0U;
    if (mod == 3U)
        return 2U;
    if (rm == 4U) {
        ++length;
        if (mod == 0U && (sib & 7U) == 5U)
            length += 4U;
    } else if (mod == 0U && rm == 5U) {
        length += 4U;
    }
    if (mod == 1U)
        length += 1U;
    else if (mod == 2U)
        length += 4U;
    return length;
}

/* Non-zero when `rva` is the byte after a call instruction in the mapped
 * .text.  *callee receives the direct target or KVGS_RVA_UNKNOWN. */
static int kvgs_validate_return_site(uint32_t rva, uint32_t *callee)
{
    static const uint32_t lengths[] = { 2U, 3U, 4U, 6U, 7U };
    unsigned i;

    if (rva < KVGS_TEXT_BEGIN_RVA + 7U || rva >= KVGS_TEXT_END_RVA)
        return 0;
    if (!s_text_present) {
        /* No code bytes to decode: accept anything that lies inside a
         * registered function (noisy, only for a loader without .text). */
        *callee = KVGS_RVA_UNKNOWN;
        return kvgs_function_containing(rva) != KVGS_RVA_UNKNOWN;
    }
    if (KVGS_IMAGE_BYTE(rva - 5U) == 0xE8U) {
        uint32_t rel = (uint32_t)KVGS_IMAGE_BYTE(rva - 4U) |
                       (uint32_t)KVGS_IMAGE_BYTE(rva - 3U) << 8 |
                       (uint32_t)KVGS_IMAGE_BYTE(rva - 2U) << 16 |
                       (uint32_t)KVGS_IMAGE_BYTE(rva - 1U) << 24;
        uint32_t target = rva + rel;
        if (target >= KVGS_TEXT_BEGIN_RVA && target < KVGS_TEXT_END_RVA &&
                kvgs_function_start_exact(target)) {
            *callee = target;
            return 1;
        }
    }
    for (i = 0U; i < sizeof lengths / sizeof lengths[0]; ++i) {
        uint32_t length = lengths[i];
        if (KVGS_IMAGE_BYTE(rva - length) != 0xFFU)
            continue;
        if (kvgs_ff_call_length(KVGS_IMAGE_BYTE(rva - length + 1U),
                                KVGS_IMAGE_BYTE(rva - length + 2U)) ==
                length) {
            *callee = KVGS_RVA_UNKNOWN;
            return 1;
        }
    }
    return 0;
}

/* -------------------------------------------------------------- hash ----- */

static void kvgs_count(kvgs_buffer *buffer, uint32_t key)
{
    uint32_t index = (key * UINT32_C(2654435761)) >> (32U - KVGS_SLOT_BITS);
    uint32_t probe;

    for (probe = 0U; probe < KVGS_PROBE_LIMIT; ++probe) {
        kvgs_entry *entry = &buffer->slots[(index + probe) & (KVGS_SLOTS - 1U)];
        if (entry->key == key) {
            ++entry->count;
            return;
        }
        if (entry->key == 0U) {
            entry->key = key;
            entry->count = 1U;
            return;
        }
    }
    ++buffer->hash_full;
}

/* ----------------------------------------------------------- sampler ----- */

static void kvgs_sample(void)
{
    CPU *c = __atomic_load_n(&s_cpu, __ATOMIC_ACQUIRE);
    kvgs_buffer *buffer;
    uint32_t phase;
    uint32_t esp;
    uint32_t floor;
    uint32_t ceiling;
    uint32_t i;
    int found = 0;

    if (!c)
        return;
    /* busy must be visible before the buffer index is read, so a reporter
     * that has already flipped the index either sees busy or is seen. */
    __atomic_store_n(&s_busy, 1U, __ATOMIC_SEQ_CST);
    buffer = &s_buffers[__atomic_load_n(&s_active, __ATOMIC_SEQ_CST) & 1U];

    phase = g_kage_vita_guest_sampler_phase;
    if (phase >= KAGE_VITA_GUEST_SAMPLER_PHASE_COUNT)
        phase = KAGE_VITA_GUEST_SAMPLER_OTH;
    esp = *(volatile const uint32_t *)&c->esp;
    floor = c->stack_floor;
    ceiling = c->stack_ceiling;
    if (!floor || floor >= ceiling || esp < floor || esp > ceiling ||
            (esp & 3U) != 0U) {
        ++buffer->bad_esp;
        goto done;
    }
    for (i = 0U; i < KVGS_STACK_SCAN_WORDS; ++i) {
        uint32_t address = esp + i * 4U;
        uint32_t value;
        uint32_t callee;

        if (address > ceiling - 4U)
            break;
        value = KVGS_STACK_WORD(address);
        ++buffer->words_scanned;
        if (value - KVGS_TEXT_BEGIN_RVA >=
                KVGS_TEXT_END_RVA - KVGS_TEXT_BEGIN_RVA)
            continue;
        if (!kvgs_validate_return_site(value, &callee))
            continue;
        if (callee == KVGS_RVA_UNKNOWN) {
            /* Name the indirect callee from the published innermost live
             * indirect target (see the contract in the header): a registered
             * translated start becomes that function, anything else (import
             * slot, dynamic token) is "ext" = native code, zero stays "ind". */
            uint32_t target = g_kage_guest_last_indirect_target;
            uint32_t rva = target - (uint32_t)GUEST_IMAGE_BASE;
            if (target != 0U) {
                if (rva >= KVGS_TEXT_BEGIN_RVA && rva < KVGS_TEXT_END_RVA &&
                        kvgs_function_start_exact(rva))
                    callee = rva;
                else
                    callee = KVGS_RVA_EXTERNAL;
            }
        }
        kvgs_count(buffer, KVGS_KEY(0U, phase, callee));
        kvgs_count(buffer, KVGS_KEY(1U, phase,
                                    kvgs_function_containing(value)));
        ++buffer->samples[phase];
        if (i >= KVGS_STACK_SCAN_WORDS_LEGACY)
            ++buffer->deep;
        found = 1;
        break;
    }
    if (!found)
        ++buffer->no_marker;
done:
    __atomic_store_n(&s_busy, 0U, __ATOMIC_SEQ_CST);
}

#if !defined(ISAAC_KAGE_VITA_GUEST_SAMPLER_ORACLE)
static int kvgs_thread(SceSize args, void *argp)
{
    uint64_t next = sceKernelGetProcessTimeWide() + KVGS_PERIOD_US;

    (void)args;
    (void)argp;
    for (;;) {
        uint64_t now = sceKernelGetProcessTimeWide();
        if (now < next) {
            uint64_t wait = next - now;
            if (wait < KVGS_MIN_SLEEP_US)
                wait = KVGS_MIN_SLEEP_US;
            (void)sceKernelDelayThread((SceUInt)wait);
            continue;
        }
        next += KVGS_PERIOD_US;
        if (next < now)
            next = now + KVGS_PERIOD_US;   /* do not replay a long stall */
        ++s_ticks;
        kvgs_sample();
    }
    return 0;
}

/* ------------------------------------------------------------ startup ---- */

static uint32_t kvgs_text_present(void)
{
    uint32_t sum = 0U;
    uint32_t i;

    if (KVGS_IMAGE_BYTE(0U) != 'M' || KVGS_IMAGE_BYTE(1U) != 'Z')
        return 0U;
    /* The entry stub and CRT start at the end of .text; any 64 non-zero
     * bytes at a registered function prove the section was copied. */
    if (!s_function_count)
        return 0U;
    for (i = 0U; i < 64U; ++i)
        sum |= KVGS_IMAGE_BYTE(s_functions[s_function_count / 2U] + i);
    return sum != 0U;
}

void kage_vita_guest_sampler_start(CPU *c)
{
    int started;

    if (s_started)
        return;
    s_started = 1U;
    s_functions = guest_registered_addresses(&s_function_count);
    s_text_present = kvgs_text_present();
    memset(s_buffers, 0, sizeof s_buffers);
    __atomic_store_n(&s_cpu, c, __ATOMIC_RELEASE);
    s_thread = sceKernelCreateThread(
        "isaac_guest_sampler", kvgs_thread, KVGS_THREAD_PRIORITY,
        KVGS_THREAD_STACK, 0U, KVGS_THREAD_AFFINITY, NULL);
    started = s_thread < 0 ? (int)s_thread
                           : sceKernelStartThread(s_thread, 0U, NULL);
    if (s_thread < 0 || started < 0) {
        if (s_thread >= 0)
            (void)sceKernelDeleteThread(s_thread);
        isaac_vita_log(
            "KAGE VITA GUEST SAMPLER: build=%s state=FAILED create=0x%08x "
            "start=0x%08x",
            ISAAC_VITA_GUEST_SAMPLER_BUILD_ID, (unsigned)s_thread,
            (unsigned)started);
        s_thread = -1;
        __atomic_store_n(&s_cpu, (CPU *)0, __ATOMIC_RELEASE);
        return;
    }
    isaac_vita_log(
        "KAGE VITA GUEST SAMPLER: build=%s thread=0x%08x affinity=0x%08x "
        "prio=%d period_us=%u scan_words=%u slots=%u probe=%u top=%u "
        "functions=%u text=%06x..%06x decode=%s stack=%08x..%08x",
        ISAAC_VITA_GUEST_SAMPLER_BUILD_ID, (unsigned)s_thread,
        (unsigned)KVGS_THREAD_AFFINITY, (int)KVGS_THREAD_PRIORITY,
        (unsigned)KVGS_PERIOD_US, (unsigned)KVGS_STACK_SCAN_WORDS,
        (unsigned)KVGS_SLOTS, (unsigned)KVGS_PROBE_LIMIT, (unsigned)KVGS_TOP,
        (unsigned)s_function_count, (unsigned)KVGS_TEXT_BEGIN_RVA,
        (unsigned)KVGS_TEXT_END_RVA, s_text_present ? "call" : "range-only",
        (unsigned)c->stack_floor, (unsigned)c->stack_ceiling);
}
#else
/* ------------------------------------------------------ host oracle ------ */

void kage_vita_guest_sampler_oracle_bind(
    CPU *c, const uint32_t *functions, uint32_t function_count,
    uint32_t text_present)
{
    s_functions = functions;
    s_function_count = function_count;
    s_text_present = text_present;
    memset(s_buffers, 0, sizeof s_buffers);
    s_active = 0U;
    s_ticks = 0U;
    s_thread = c ? 1 : -1;
    __atomic_store_n(&s_cpu, c, __ATOMIC_RELEASE);
}

void kage_vita_guest_sampler_oracle_sample(void)
{
    ++s_ticks;
    kvgs_sample();
}

uint32_t kage_vita_guest_sampler_oracle_scan_words(void)
{
    return KVGS_STACK_SCAN_WORDS;
}
#endif

/* ------------------------------------------------------------- report ---- */

typedef struct kvgs_top {
    uint32_t rva[KVGS_TOP];
    uint32_t count[KVGS_TOP];
    uint32_t used;
} kvgs_top;

static void kvgs_top_insert(kvgs_top *top, uint32_t rva, uint32_t count)
{
    uint32_t position = top->used;
    uint32_t i;

    if (position == KVGS_TOP) {
        if (count <= top->count[KVGS_TOP - 1U])
            return;
        position = KVGS_TOP - 1U;
    } else {
        ++top->used;
    }
    while (position > 0U && top->count[position - 1U] < count) {
        top->rva[position] = top->rva[position - 1U];
        top->count[position] = top->count[position - 1U];
        --position;
    }
    top->rva[position] = rva;
    top->count[position] = count;
    for (i = top->used; i < KVGS_TOP; ++i) {
        top->rva[i] = 0U;
        top->count[i] = 0U;
    }
}

static void kvgs_log_top(
    const char *record, const char *build_id, uint32_t window,
    uint32_t last_outer_loop, uint32_t phase, uint32_t samples,
    const kvgs_top *top)
{
    /* Worst case: 27 + 32 + 63 + 12 * 19 + 1 = 351 bytes, under the 384-byte
     * durable log bound.  One call, one newline: the Vita logger attributes
     * records per call. */
    char line[384];
    int length;
    uint32_t i;

    length = sceClibSnprintf(
        line, sizeof line,
        "[kage-vita] %s bid=%.32s win=%u loops=%u phase=%s samples=%u top=",
        record, build_id, window, last_outer_loop, s_phase_names[phase],
        samples);
    if (length < 0)
        return;
    for (i = 0U; i < top->used && (size_t)length < sizeof line - 2U; ++i) {
        int part;
        if (top->rva[i] == KVGS_RVA_UNKNOWN)
            part = sceClibSnprintf(line + length, sizeof line - (size_t)length,
                                   "%sind:%u", i ? "," : "", top->count[i]);
        else if (top->rva[i] == KVGS_RVA_EXTERNAL)
            part = sceClibSnprintf(line + length, sizeof line - (size_t)length,
                                   "%sext:%u", i ? "," : "", top->count[i]);
        else
            part = sceClibSnprintf(line + length, sizeof line - (size_t)length,
                                   "%s%x:%u", i ? "," : "", top->rva[i],
                                   top->count[i]);
        if (part < 0)
            break;
        length += part;
        if ((size_t)length >= sizeof line - 2U) {
            length = (int)sizeof line - 2;
            break;
        }
    }
    line[length] = '\n';
    line[length + 1] = '\0';
    ISAAC_VITA_LOG_PRINTF("%s", line);
}

void kage_vita_guest_sampler_report(
    const char *build_id, uint32_t window, uint32_t last_outer_loop)
{
    kvgs_buffer *retired;
    uint32_t old;
    uint32_t spins = 0U;
    uint32_t phase;
    uint32_t total = 0U;

    if (s_thread < 0)
        return;
    old = s_active & 1U;
    __atomic_store_n(&s_active, old ^ 1U, __ATOMIC_SEQ_CST);
    /* The sampler publishes busy before it reads the index, so once busy is
     * clear it is either finished with the retired buffer or on the new one.
     * A sample takes microseconds; the spin bound only guards a descheduled
     * sampler and then tolerates one torn count. */
    while (__atomic_load_n(&s_busy, __ATOMIC_SEQ_CST) && spins < KVGS_REPORT_SPIN)
        ++spins;
    retired = &s_buffers[old];

    for (phase = 0U; phase < KAGE_VITA_GUEST_SAMPLER_PHASE_COUNT; ++phase)
        total += retired->samples[phase];
    for (phase = 0U; phase < KAGE_VITA_GUEST_SAMPLER_PHASE_COUNT; ++phase) {
        kvgs_top self;
        kvgs_top caller;
        uint32_t i;

        if (!retired->samples[phase])
            continue;
        memset(&self, 0, sizeof self);
        memset(&caller, 0, sizeof caller);
        for (i = 0U; i < KVGS_SLOTS; ++i) {
            const kvgs_entry *entry = &retired->slots[i];
            if (!entry->key || KVGS_KEY_PHASE(entry->key) != phase)
                continue;
            kvgs_top_insert(KVGS_KEY_VIEW(entry->key) ? &caller : &self,
                            KVGS_KEY_RVA(entry->key), entry->count);
        }
        kvgs_log_top("ph120.hot", build_id, window, last_outer_loop, phase,
                     retired->samples[phase], &self);
        kvgs_log_top("ph120.hotc", build_id, window, last_outer_loop, phase,
                     retired->samples[phase], &caller);
    }
    /* Worst case 361 bytes (32-char bid, eighteen ten-digit %u, newline)
     * under the 384-byte durable log bound; pinned by
     * test_kage_vita_guest_sampler.py. */
    ISAAC_VITA_LOG_PRINTF(
        "[kage-vita] ph120.hs bid=%.32s win=%u loops=%u period_us=%u "
        "samples(svc,upd,rnd,swp,lim,oth,pace)=%u,%u,%u,%u,%u,%u,%u "
        "total=%u nomark=%u badesp=%u full=%u words=%u deep=%u ticks=%u "
        "spin=%u\n",
        build_id, window, last_outer_loop, (unsigned)KVGS_PERIOD_US,
        retired->samples[0], retired->samples[1], retired->samples[2],
        retired->samples[3], retired->samples[4], retired->samples[5],
        retired->samples[6], total, retired->no_marker, retired->bad_esp,
        retired->hash_full, retired->words_scanned, retired->deep, s_ticks,
        spins);
    memset(retired, 0, sizeof *retired);
}
