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
#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
/* USER_1 belongs to the GL render thread; the sampler joins the workers on
 * USER_2 so it never preempts the render thread. */
#define KVGS_THREAD_AFFINITY  SCE_KERNEL_CPU_MASK_USER_2
#else
#define KVGS_THREAD_AFFINITY  SCE_KERNEL_CPU_MASK_USER_1
#endif
#define KVGS_TEXT_BEGIN_RVA   UINT32_C(0x00001000)
/* Frozen PE: .text ends at 0x605b34, the IAT starts at 0x6060f8. */
#define KVGS_TEXT_END_RVA     UINT32_C(0x00606000)
#define KVGS_RVA_UNKNOWN      UINT32_C(0x0fffffff)
/* An indirect call whose published target is not a translated function:
 * an IAT import, a GL/dynamic token, i.e. native host code (vitaGL, CRT). */
#define KVGS_RVA_EXTERNAL     UINT32_C(0x0ffffffe)
#if defined(ISAAC_VITA_GUEST_SAMPLER_PRESENT_LABEL)
/* Profile-only.  The swp phase is the native present (PRESENT_ENTER ..
 * PRESENT_RETURN); the KAGE present is an inline seam in 0x4B0600 with no
 * guest return marker, so a stack scan there names a stale word (on the
 * device 99.9-100 % of swp samples landed on MusicManager::Update 39fbf0).
 * Name that time "present" in both views instead of scanning. */
#define KVGS_RVA_PRESENT      UINT32_C(0x0ffffffd)
#endif
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

#if defined(ISAAC_VITA_GUEST_SAMPLER_CHAIN)
/* ISAAC_VITA_GUEST_SAMPLER_CHAIN (profiling builds, default OFF): an
 * inclusive view of the render phase.  After the innermost marker the scan
 * keeps walking up the whole guest stack and counts every function on the
 * live call chain once per sample (ph120.hoti, top 36 per window), so a
 * chain's total cost reads directly instead of being summed from flat self
 * lists whose top 12 cover about half of the render samples.
 *
 * Chain link rule (keeps stale words in uninitialised locals out): a marker
 * whose call is direct counts only when its target is the function the
 * previous marker returns into, either exactly or through one forwarding
 * thunk (`jmp rel32`, or `add/sub ecx,imm8; jmp rel32`, the MSVC this-adjust
 * stub); any other direct marker is skipped (`skip`).  An indirect marker
 * (vtable, function pointer, import) at A counts only when it is frame
 * linked: [A-4] is the callee's saved EBP, it points above A (< 64 KiB) and
 * [EBP+4] holds a return marker that is indirect or whose direct call
 * reaches the function containing A; the walk then continues at EBP+4
 * (`ind`).  Anything else is dropped (`indrej`).  Session P (2026-10-16)
 * ran the first rule, which accepted every indirect marker: 64 % of its
 * links were indirect and lock wrappers / the release observer showed
 * 20-55 % inclusive at ~0.1 % self -- stale return words of frequent
 * indirect calls (EnterCriticalSection, `call eax` observers).  The frame
 * link drops those at the cost of callees that keep EBP free (FPO) or use
 * the `push ebx; mov ebx,esp` aligned prologue: such a link is lost and the
 * chain above it undercounts.  `us` = the walk's own wall time on the
 * sampler core (sum, max per window).  A thunk
 * the rule does not know truncates the chain upward (outer frames then
 * undercount, never overcount).  Inclusive keys are caller-view keys with
 * bit 26 set: real RVAs stay below 0x00606000 and the two sentinels have
 * bit 27 set, so no key collides. */
# define KVGS_CHAIN_PHASE      KAGE_VITA_GUEST_SAMPLER_RND
# define KVGS_CHAIN_SCAN_WORDS 16384U
# define KVGS_CHAIN_DEPTH      64U
# define KVGS_CHAIN_TOP        36U
# define KVGS_CHAIN_FRAME_SPAN 0x10000U
# if defined(ISAAC_KAGE_VITA_GUEST_SAMPLER_ORACLE)
uint64_t kvgs_oracle_now_us(void);
#  define KVGS_NOW_US() kvgs_oracle_now_us()
# else
#  define KVGS_NOW_US() sceKernelGetProcessTimeWide()
# endif
# define KVGS_INCL_TAG         UINT32_C(0x04000000)
# define KVGS_IS_INCL(rva)     (((rva) & UINT32_C(0x0c000000)) == KVGS_INCL_TAG)
/* ph120.hotr keys: bit 25 tag, bit 24 = the sample is not translated code;
 * root RVAs stay below 0x00800000, the sentinels have bit 27 set. */
# define KVGS_ROOT_TAG         UINT32_C(0x02000000)
# define KVGS_ROOT_EXT         UINT32_C(0x01000000)
# define KVGS_IS_ROOT(rva)     (((rva) & UINT32_C(0x0e000000)) == KVGS_ROOT_TAG)
#endif

#if defined(ISAAC_VITA_GUEST_SAMPLER_RESIDUAL)
# if !defined(ISAAC_VITA_GUEST_SAMPLER_CHAIN)
#  error ISAAC_VITA_GUEST_SAMPLER_RESIDUAL extends the chain view (ISAAC_VITA_GUEST_SAMPLER_CHAIN)
# endif
/* ISAAC_VITA_GUEST_SAMPLER_RESIDUAL (profiling builds, default OFF):
 * what the chain view cannot say about the render
 * residual.
 *
 * 1. Lua boundary.  The native Lua VM calls a translated C function (or the
 *    guest allocator) with this word as its return address
 *    (host_vita_lua.c:71 VITA_LUA_CALLBACK_RETURN; pushed after the state at
 *    host_vita_lua.c:1004-1006 and after four arguments at :1460-1464), and
 *    the import bridge leaves the translated call into the running Lua
 *    import at [esp] (vita_lua_arg reads esp+4.., host_vita_lua.c:269-273).
 *    So that caller's return marker sits 8 bytes above the word for a C
 *    function and 20 for the allocator.  The chain rule above cannot cross
 *    it (the marker's [A-4] is the state, not a saved EBP: indrej), so every
 *    sample inside a translated LuaBridge binding lost its root.  Here the
 *    walk links the word to that marker (lb; lbm = no return site at the
 *    offset) and continues from the caller.  A sample whose innermost marker
 *    already sits above such a word (the binding's own code) is marked too
 *    (lbs).  Only a word at an address host_vita_lua.c published as live
 *    counts, with the offset of its kind (kvgs_lua_live): the triple stays
 *    below ESP after a callback returns, so frames that later reuse those
 *    slots carry stale copies in their locals; those are counted (lbx) and
 *    skipped like any other non-marker word.
 * 2. ph120.hotr keys gain bit 23 (+800000) when the walk crossed a live
 *    boundary at or below the innermost root, i.e. the sample is inside a
 *    Lua callback that root's code started.  For root 0 (none found) the
 *    bit means the walk crossed a boundary and then broke: +800000 alone is
 *    still "no root", and bench_run.py counts it there.
 *    Root RVAs stay below 0x00800000.
 * 3. ph120.hotl: samples whose innermost root is the mod-callback invoker
 *    sub_0040eef0, split by the next root out (which render pass runs the
 *    callbacks) and by t/n/lt/ln (translated, native, and the same inside a
 *    crossed boundary).
 * 4. ph120.hotu: samples with no root, by the outermost function the walk
 *    reached (where the chain broke), +1000000 = the sample is native.
 * 5. ph120.hotw: the render watch list (kage_vita_render_watch.h) with every
 *    count the window holds for it, whatever its rank. */
# include "kage_vita_render_watch.h"
# define KVGS_LUA_CALLBACK_RETURN  UINT32_C(0xfff15333)
# define KVGS_LUA_CALLER_CFUNCTION 8U
# define KVGS_LUA_CALLER_ALLOCATOR 20U
# define KVGS_LUA_INVOKER_RVA      UINT32_C(0x0040eef0)
# define KVGS_ROOT_LUA             UINT32_C(0x00800000)
# define KVGS_UNROOTED_TAG         UINT32_C(0x0c000000)
# define KVGS_IS_UNROOTED(rva)     (((rva) & UINT32_C(0x0e000000)) == KVGS_UNROOTED_TAG)
# define KVGS_NO_BOUNDARY          UINT32_MAX
# define KVGS_ROOT_COUNT           24U
#endif

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
#if defined(ISAAC_VITA_GUEST_SAMPLER_CHAIN)
    uint32_t chain_samples;   /* render samples walked */
    uint32_t chain_links;     /* accepted markers above the innermost */
    uint32_t chain_ind;       /* of those, indirect (unchecked) */
    uint32_t chain_thunk;     /* of those, matched through a thunk */
    uint32_t chain_skip;      /* direct markers that did not link */
    uint32_t chain_full;      /* walks stopped at KVGS_CHAIN_DEPTH */
    uint32_t chain_ind_rejected; /* indirect markers without a frame link */
    uint32_t chain_us;        /* walk wall time, sum (us) */
    uint32_t chain_us_max;    /* walk wall time, longest (us) */
#endif
#if defined(ISAAC_VITA_GUEST_SAMPLER_RESIDUAL)
    uint32_t lua_links;       /* Lua boundary words linked to their caller */
    uint32_t lua_miss;        /* boundary words with no caller marker */
    uint32_t lua_self;        /* samples whose innermost marker is above one */
    uint32_t lua_stale;       /* boundary-valued words at unpublished addresses */
    /* sub_0040eef0 samples by next root out (s_chain_roots index, last =
     * none) x {t, n, lt, ln}. */
    uint32_t lua_split[KVGS_ROOT_COUNT + 1U][4];
#endif
} kvgs_buffer;

volatile uint32_t g_kage_vita_guest_sampler_phase =
    KAGE_VITA_GUEST_SAMPLER_OTH;

#if defined(ISAAC_VITA_PROFILE_FUNCTION_ENTRY_CENSUS)
/* guest_entry_census.h: written by the main thread's body-entry hooks,
 * printed and cleared by the reporter on the same thread.  The ids come from
 * the guest_coverage.json CMake handed vita_render_watch.py at configure
 * time; the corpus this TU is compiled against must have the same id space
 * (function count here, sorted-RVA hash at start-up: a mismatch prints
 * ids=STALE and suppresses ph120.fe rather than charge entries to the wrong
 * functions). */
# include "guest_coverage_generated.h"
uint32_t g_guest_entry_census[GUEST_ENTRY_CENSUS_PHASES]
                             [GUEST_ENTRY_CENSUS_COUNT];
static const uint32_t s_census_rvas[GUEST_ENTRY_CENSUS_COUNT] =
    GUEST_ENTRY_CENSUS_RVAS;
_Static_assert(KAGE_VITA_GUEST_SAMPLER_PHASE_COUNT <= GUEST_ENTRY_CENSUS_PHASES,
               "the census indexes every sampler phase");
_Static_assert(GUEST_ENTRY_CENSUS_FUNCTION_COUNT ==
                   GUEST_COVERAGE_FUNCTION_COUNT,
               "guest_entry_census_ids.h was generated from another corpus");

static uint32_t kvgs_census_stale(void)
{
    return strcmp(GUEST_ENTRY_CENSUS_FUNCTION_RVAS_SHA256,
                  GUEST_COVERAGE_FUNCTION_RVAS_SHA256) != 0 ? 1U : 0U;
}
#endif

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
#if defined(ISAAC_VITA_UPDATE_HITCH_PROBE)
/* Update hitch probe: the last attributed (self, caller) pairs in time
 * order, next to (not instead of) the window aggregation. */
# include "kage_vita_update_hitch.h"
static KageVitaUpdateHitchTrail s_hitch_trail;

uint32_t kage_vita_guest_sampler_trail_seq(void)
{
    return kage_vita_update_hitch_trail_seq(&s_hitch_trail);
}

uint32_t kage_vita_guest_sampler_trail_copy(
    uint32_t begin_seq, uint32_t end_seq, uint32_t *self, uint32_t *caller,
    uint32_t *lost)
{
    return kage_vita_update_hitch_trail_copy(
        &s_hitch_trail, begin_seq, end_seq, self, caller, lost);
}
#endif

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

#if defined(ISAAC_VITA_GUEST_SAMPLER_CHAIN)
static uint32_t kvgs_rel32_target(uint32_t at)
{
    uint32_t rel = (uint32_t)KVGS_IMAGE_BYTE(at) |
                   (uint32_t)KVGS_IMAGE_BYTE(at + 1U) << 8 |
                   (uint32_t)KVGS_IMAGE_BYTE(at + 2U) << 16 |
                   (uint32_t)KVGS_IMAGE_BYTE(at + 3U) << 24;
    return at + 4U + rel;
}

/* Does a direct call to `callee` arrive in `function`?  Exactly, or through
 * one forwarding thunk at the callee: E9 rel32, or 83 C1/E9 ib E9 rel32. */
static int kvgs_chain_reaches(uint32_t callee, uint32_t function, int *thunk)
{
    uint32_t jump = KVGS_RVA_UNKNOWN;

    *thunk = 0;
    if (callee == function)
        return 1;
    if (!s_text_present || callee >= KVGS_TEXT_END_RVA - 8U)
        return 0;
    if (KVGS_IMAGE_BYTE(callee) == 0xE9U)
        jump = kvgs_rel32_target(callee + 1U);
    else if (KVGS_IMAGE_BYTE(callee) == 0x83U &&
             (KVGS_IMAGE_BYTE(callee + 1U) == 0xC1U ||
              KVGS_IMAGE_BYTE(callee + 1U) == 0xE9U) &&
             KVGS_IMAGE_BYTE(callee + 3U) == 0xE9U)
        jump = kvgs_rel32_target(callee + 4U);
    *thunk = 1;
    return jump == function;
}

static void kvgs_chain_add(kvgs_buffer *buffer, uint32_t phase, uint32_t *seen,
                           uint32_t *used, uint32_t rva)
{
    uint32_t i;

    if (rva < KVGS_TEXT_BEGIN_RVA || rva >= KVGS_TEXT_END_RVA)
        return;                       /* ind / ext / unknown: no inclusive row */
    for (i = 0U; i < *used; ++i)
        if (seen[i] == rva)
            return;                   /* recursion: once per sample */
    seen[(*used)++] = rva;
    kvgs_count(buffer, KVGS_KEY(1U, phase, KVGS_INCL_TAG | rva));
}

/* ph120.hotr: exclusive cost by render chain.  Each walked sample is charged
 * to the innermost function of this list that is on its chain (0 = none),
 * split by whether the sample itself is translated code or not (ext / ind:
 * native vitaGL, Lua, CRT).  Unlike ph120.hoti the rows sum to the phase, so
 * "translated ms under Room::Render that is not under RenderFrame" reads
 * directly.  Roots: the render ancestors of session P2b's chain view
 * (2026-10-17), names from REPENTOGON / earlier reports where known. */
static const uint32_t s_chain_roots[] = {
    0x0048bc50u, 0x004b0600u, 0x002ce770u, 0x003ca670u, 0x003cad40u,
    0x003dbfe0u, 0x0056d500u, 0x00564f50u, 0x00003e50u, 0x00005190u,
    0x000051e0u, 0x00014a90u, 0x00560eb0u, 0x003ffe10u, 0x0040eef0u,
    0x001e9810u, 0x005689d0u, 0x00504610u, 0x00508710u, 0x00006dd0u,
    0x0055f610u, 0x0025af60u, 0x0055f990u, 0x00004b60u,
};

#if defined(ISAAC_VITA_GUEST_SAMPLER_RESIDUAL)
_Static_assert(sizeof s_chain_roots / sizeof s_chain_roots[0] == KVGS_ROOT_COUNT,
               "ph120.hotl indexes the chain roots");

#define KVGS_WATCH_ITEM(rva) (rva),
static const uint32_t s_watch[] = { KAGE_VITA_RENDER_WATCH(KVGS_WATCH_ITEM) };
#undef KVGS_WATCH_ITEM
_Static_assert(sizeof s_watch / sizeof s_watch[0] <= KAGE_VITA_RENDER_WATCH_MAX,
               "kage_vita_render_watch.h holds at most 64 entries");

static uint32_t kvgs_watch_count(void)
{
    return (uint32_t)(sizeof s_watch / sizeof s_watch[0]);
}

/* Published by host_vita_lua.c (kage_vita_guest_sampler.h): the guest
 * addresses of the live boundary words, 0 = slot empty. */
volatile uint32_t
    g_kage_vita_guest_sampler_lua_cfunction[KAGE_VITA_GUEST_SAMPLER_LUA_CFUNCTION_SLOTS];
volatile uint32_t g_kage_vita_guest_sampler_lua_allocator;

static uint32_t kvgs_root_index(uint32_t rva)
{
    uint32_t r;

    for (r = 0U; r < KVGS_ROOT_COUNT; ++r)
        if (s_chain_roots[r] == rva)
            return r;
    return KVGS_ROOT_COUNT;
}

/* A KVGS_LUA_CALLBACK_RETURN word at `address` is a live boundary only when
 * host_vita_lua.c published that address: the offset of the translated
 * caller's return marker above it (8 for a C function: the state, then the
 * word, pushed at host_vita_lua.c:1004-1005 over the import caller's marker
 * at frame->saved_esp; 20 for the allocator: four arguments and the word over
 * saved_esp, :1459-1464), or 0 for a stale word -- the same triple stays below
 * ESP after its callback returned (taking it moved the root to
 * the invoker and dropped the live frames above it). */
static uint32_t kvgs_lua_live(uint32_t address)
{
    uint32_t k;

    if (address == g_kage_vita_guest_sampler_lua_allocator)
        return KVGS_LUA_CALLER_ALLOCATOR;
    for (k = 0U; k < KAGE_VITA_GUEST_SAMPLER_LUA_CFUNCTION_SLOTS; ++k)
        if (address == g_kage_vita_guest_sampler_lua_cfunction[k])
            return KVGS_LUA_CALLER_CFUNCTION;
    return 0U;
}

/* Live boundary at `address` with its caller marker `offset` bytes above:
 * the marker's address (its word in *marker), or 0 when that word is not a
 * validated return site (lbm). */
static uint32_t kvgs_lua_caller(uint32_t address, uint32_t offset,
                                uint32_t ceiling, uint32_t *marker)
{
    uint32_t at = address + offset;
    uint32_t word;
    uint32_t callee;

    if (at < address || at > ceiling - 4U)
        return 0U;
    word = KVGS_STACK_WORD(at);
    if (word - KVGS_TEXT_BEGIN_RVA < KVGS_TEXT_END_RVA - KVGS_TEXT_BEGIN_RVA &&
            kvgs_validate_return_site(word, &callee)) {
        *marker = word;
        return at;
    }
    return 0U;
}

/* Residual root attribution; `lua_at` = seen index of the first crossed
 * boundary (the entries before it run inside the Lua callback). */
static void kvgs_chain_root(kvgs_buffer *buffer, uint32_t phase,
                            const uint32_t *seen, uint32_t used, uint32_t self,
                            uint32_t lua_at)
{
    uint32_t root = 0U;
    uint32_t at = used;
    uint32_t ext = (self < KVGS_TEXT_BEGIN_RVA || self >= KVGS_TEXT_END_RVA)
                       ? KVGS_ROOT_EXT : 0U;
    uint32_t lua;
    uint32_t k;

    for (k = 0U; k < used && !root; ++k)
        if (kvgs_root_index(seen[k]) < KVGS_ROOT_COUNT) {
            root = seen[k];
            at = k;
        }
    lua = (lua_at != KVGS_NO_BOUNDARY && lua_at <= at) ? KVGS_ROOT_LUA : 0U;
    kvgs_count(buffer, KVGS_KEY(1U, phase, KVGS_ROOT_TAG | ext | lua | root));
    if (!root) {
        kvgs_count(buffer, KVGS_KEY(1U, phase, KVGS_UNROOTED_TAG | ext |
                                    (used ? seen[used - 1U] : 0U)));
    } else if (root == KVGS_LUA_INVOKER_RVA) {
        uint32_t outer = KVGS_ROOT_COUNT;
        for (k = at + 1U; k < used && outer == KVGS_ROOT_COUNT; ++k)
            outer = kvgs_root_index(seen[k]);
        ++buffer->lua_split[outer][(lua ? 2U : 0U) + (ext ? 1U : 0U)];
    }
}
#else
static void kvgs_chain_root(kvgs_buffer *buffer, uint32_t phase,
                            const uint32_t *seen, uint32_t used, uint32_t self)
{
    uint32_t root = 0U;
    uint32_t ext = (self < KVGS_TEXT_BEGIN_RVA || self >= KVGS_TEXT_END_RVA)
                       ? KVGS_ROOT_EXT : 0U;
    uint32_t k;
    uint32_t r;

    for (k = 0U; k < used && !root; ++k)
        for (r = 0U; r < sizeof s_chain_roots / sizeof s_chain_roots[0]; ++r)
            if (seen[k] == s_chain_roots[r]) {
                root = seen[k];
                break;
            }
    kvgs_count(buffer, KVGS_KEY(1U, phase, KVGS_ROOT_TAG | ext | root));
}
#endif

/* `first` is the scan index of the innermost marker, `value` its word and
 * `self` the self-view callee already counted for it. */
#if defined(ISAAC_VITA_GUEST_SAMPLER_RESIDUAL)
static void kvgs_chain(kvgs_buffer *buffer, uint32_t phase, uint32_t esp,
                       uint32_t ceiling, uint32_t first, uint32_t value,
                       uint32_t self, int lua_below)
#else
static void kvgs_chain(kvgs_buffer *buffer, uint32_t phase, uint32_t esp,
                       uint32_t ceiling, uint32_t first, uint32_t value,
                       uint32_t self)
#endif
{
    uint32_t seen[KVGS_CHAIN_DEPTH];
    uint32_t used = 0U;
    uint32_t current = kvgs_function_containing(value);
    uint32_t i;
#if defined(ISAAC_VITA_GUEST_SAMPLER_RESIDUAL)
    uint32_t lua_at = KVGS_NO_BOUNDARY;
#endif

    ++buffer->chain_samples;
    kvgs_chain_add(buffer, phase, seen, &used, self);
#if defined(ISAAC_VITA_GUEST_SAMPLER_RESIDUAL)
    if (lua_below) {
        /* The innermost marker is the Lua caller's: self is the C function
         * the VM called (guest_call published it), outside it the caller. */
        ++buffer->lua_self;
        lua_at = used;
    }
#endif
    kvgs_chain_add(buffer, phase, seen, &used, current);
    for (i = first + 1U; i < KVGS_CHAIN_SCAN_WORDS; ++i) {
        uint32_t address = esp + i * 4U;
        uint32_t callee;
        int thunk;

        if (address > ceiling - 4U || address < esp)
            break;
        value = KVGS_STACK_WORD(address);
#if defined(ISAAC_VITA_GUEST_SAMPLER_RESIDUAL)
        if (value == KVGS_LUA_CALLBACK_RETURN) {
            uint32_t offset = kvgs_lua_live(address);
            uint32_t marker = 0U;
            uint32_t caller;
            if (!offset) {
                ++buffer->lua_stale;   /* a returned callback's word */
                continue;
            }
            caller = kvgs_lua_caller(address, offset, ceiling, &marker);
            if (!caller) {
                ++buffer->lua_miss;
                continue;
            }
            ++buffer->lua_links;
            if (lua_at == KVGS_NO_BOUNDARY)
                lua_at = used;
            current = kvgs_function_containing(marker);
            if (used == KVGS_CHAIN_DEPTH) {
                ++buffer->chain_full;
                break;
            }
            kvgs_chain_add(buffer, phase, seen, &used, current);
            i = (caller - esp) / 4U;   /* resume above the caller's marker */
            continue;
        }
#endif
        if (value - KVGS_TEXT_BEGIN_RVA >=
                KVGS_TEXT_END_RVA - KVGS_TEXT_BEGIN_RVA)
            continue;
        if (!kvgs_validate_return_site(value, &callee))
            continue;
        if (callee == KVGS_RVA_UNKNOWN) {
            /* Frame link: the callee's `push ebp` left the caller's EBP at
             * [A-4]; the caller's return marker then sits at EBP+4 and must
             * itself link to the function containing A. */
            uint32_t saved = KVGS_STACK_WORD(address - 4U);
            uint32_t next = saved + 4U;
            uint32_t owner = kvgs_function_containing(value);
            uint32_t next_value, next_callee;
            int next_thunk;

            if (saved <= address || saved - address >= KVGS_CHAIN_FRAME_SPAN ||
                    (saved & 3U) != 0U || next > ceiling - 4U ||
                    (next_value = KVGS_STACK_WORD(next)) - KVGS_TEXT_BEGIN_RVA >=
                        KVGS_TEXT_END_RVA - KVGS_TEXT_BEGIN_RVA ||
                    !kvgs_validate_return_site(next_value, &next_callee) ||
                    (next_callee != KVGS_RVA_UNKNOWN &&
                     (owner == KVGS_RVA_UNKNOWN ||
                      !kvgs_chain_reaches(next_callee, owner, &next_thunk)))) {
                ++buffer->chain_ind_rejected;
                continue;
            }
            ++buffer->chain_ind;
            ++buffer->chain_links;
            current = owner;
            if (used == KVGS_CHAIN_DEPTH) {
                ++buffer->chain_full;
                break;
            }
            kvgs_chain_add(buffer, phase, seen, &used, current);
            i = (next - esp) / 4U - 1U;   /* the caller's marker is next */
            continue;
        } else if (current == KVGS_RVA_UNKNOWN ||
                   !kvgs_chain_reaches(callee, current, &thunk)) {
            ++buffer->chain_skip;
            continue;
        } else if (thunk) {
            ++buffer->chain_thunk;
        }
        ++buffer->chain_links;
        current = kvgs_function_containing(value);
        if (used == KVGS_CHAIN_DEPTH) {
            ++buffer->chain_full;
            break;
        }
        kvgs_chain_add(buffer, phase, seen, &used, current);
    }
#if defined(ISAAC_VITA_GUEST_SAMPLER_RESIDUAL)
    kvgs_chain_root(buffer, phase, seen, used, self, lua_at);
#else
    kvgs_chain_root(buffer, phase, seen, used, self);
#endif
}
#endif

/* ----------------------------------------------------------- sampler ----- */

static void kvgs_sample(void)
{
    CPU *c;
    kvgs_buffer *buffer;
    uint32_t phase;
    uint32_t esp;
    uint32_t floor;
    uint32_t ceiling;
    uint32_t i;
    int found = 0;
#if defined(ISAAC_VITA_GUEST_SAMPLER_RESIDUAL)
    int lua_below = 0;
#endif

    /* busy must be visible before the buffer index and the CPU are read: a
     * reporter that has already flipped the index either sees busy or is
     * seen, and kage_vita_guest_sampler_stop, which nulls the CPU and then
     * reads busy (SEQ_CST on both sides), either sees this sample in flight
     * and waits for it or is seen here as a NULL CPU. */
    __atomic_store_n(&s_busy, 1U, __ATOMIC_SEQ_CST);
    c = __atomic_load_n(&s_cpu, __ATOMIC_SEQ_CST);
    if (!c)
        goto done;
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
#if defined(ISAAC_VITA_GUEST_SAMPLER_PRESENT_LABEL)
    if (phase == KAGE_VITA_GUEST_SAMPLER_SWP) {
        kvgs_count(buffer, KVGS_KEY(0U, phase, KVGS_RVA_PRESENT));
        kvgs_count(buffer, KVGS_KEY(1U, phase, KVGS_RVA_PRESENT));
        ++buffer->samples[phase];
        goto done;
    }
#endif
    for (i = 0U; i < KVGS_STACK_SCAN_WORDS; ++i) {
        uint32_t address = esp + i * 4U;
        uint32_t value;
        uint32_t callee;

        if (address > ceiling - 4U)
            break;
        value = KVGS_STACK_WORD(address);
        ++buffer->words_scanned;
#if defined(ISAAC_VITA_GUEST_SAMPLER_RESIDUAL)
        /* Below the innermost marker: a live boundary means the sample is
         * in the C function the VM called; a stale word means nothing. */
        if (value == KVGS_LUA_CALLBACK_RETURN && phase == KVGS_CHAIN_PHASE) {
            if (kvgs_lua_live(address))
                lua_below = 1;
            else
                ++buffer->lua_stale;
        }
#endif
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
#if defined(ISAAC_VITA_UPDATE_HITCH_PROBE)
        kage_vita_update_hitch_trail_push(
            &s_hitch_trail, callee, kvgs_function_containing(value));
#endif
        ++buffer->samples[phase];
        if (i >= KVGS_STACK_SCAN_WORDS_LEGACY)
            ++buffer->deep;
#if defined(ISAAC_VITA_GUEST_SAMPLER_CHAIN)
        if (phase == KVGS_CHAIN_PHASE) {
            uint64_t start = KVGS_NOW_US();
            uint32_t spent;
#if defined(ISAAC_VITA_GUEST_SAMPLER_RESIDUAL)
            kvgs_chain(buffer, phase, esp, ceiling, i, value, callee,
                       lua_below);
#else
            kvgs_chain(buffer, phase, esp, ceiling, i, value, callee);
#endif
            spent = (uint32_t)(KVGS_NOW_US() - start);
            buffer->chain_us += spent;
            if (spent > buffer->chain_us_max)
                buffer->chain_us_max = spent;
        }
#endif
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
#if defined(ISAAC_VITA_PROFILE_FUNCTION_ENTRY_CENSUS)
    /* Before the thread: the census runs on the game thread's entry hooks
     * whether or not the sampler thread starts. */
    isaac_vita_log(
        "KAGE VITA ENTRY CENSUS: build=%s entries=%u phases=%u ids=%s "
        "census_rvas=%.16s corpus_rvas=%.16s generation=%.24s",
        ISAAC_VITA_GUEST_SAMPLER_BUILD_ID, (unsigned)GUEST_ENTRY_CENSUS_COUNT,
        (unsigned)KAGE_VITA_GUEST_SAMPLER_PHASE_COUNT,
        kvgs_census_stale() ? "STALE" : "ok",
        GUEST_ENTRY_CENSUS_FUNCTION_RVAS_SHA256,
        GUEST_COVERAGE_FUNCTION_RVAS_SHA256,
        GUEST_ENTRY_CENSUS_GENERATION_ID);
#endif
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
#if defined(ISAAC_VITA_GUEST_SAMPLER_RESIDUAL)
    isaac_vita_log(
        "KAGE VITA GUEST SAMPLER RESIDUAL: build=%s watch=%u lua_return=0x%08x "
        "caller_offsets=%u,%u live=published slots=%u invoker=%06x roots=%u",
        ISAAC_VITA_GUEST_SAMPLER_BUILD_ID, (unsigned)kvgs_watch_count(),
        (unsigned)KVGS_LUA_CALLBACK_RETURN, (unsigned)KVGS_LUA_CALLER_CFUNCTION,
        (unsigned)KVGS_LUA_CALLER_ALLOCATOR,
        (unsigned)KAGE_VITA_GUEST_SAMPLER_LUA_CFUNCTION_SLOTS,
        (unsigned)KVGS_LUA_INVOKER_RVA, (unsigned)KVGS_ROOT_COUNT);
#endif
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

#if defined(ISAAC_VITA_GUEST_SAMPLER_CHAIN)
typedef struct kvgs_chain_top {
    uint32_t rva[KVGS_CHAIN_TOP];
    uint32_t count[KVGS_CHAIN_TOP];
    uint32_t used;
} kvgs_chain_top;

static void kvgs_chain_top_insert(kvgs_chain_top *top, uint32_t rva,
                                  uint32_t count)
{
    uint32_t position = top->used;

    if (position == KVGS_CHAIN_TOP) {
        if (count <= top->count[KVGS_CHAIN_TOP - 1U])
            return;
        position = KVGS_CHAIN_TOP - 1U;
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
}
#endif

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
#if defined(ISAAC_VITA_GUEST_SAMPLER_PRESENT_LABEL)
        else if (top->rva[i] == KVGS_RVA_PRESENT)
            part = sceClibSnprintf(line + length, sizeof line - (size_t)length,
                                   "%spresent:%u", i ? "," : "", top->count[i]);
#endif
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

#if defined(ISAAC_VITA_GUEST_SAMPLER_RESIDUAL) || \
    defined(ISAAC_VITA_PROFILE_FUNCTION_ENTRY_CENSUS)
/* A record split over as many lines as its items need: every line repeats
 * the header and holds whole items only, under the 384-byte durable log
 * bound; one call, one line.  The header is at most 12 + 10 + 5 + 32 + 5 + 10
 * + 7 + 10 + 1 + 63 (rest) = 155 bytes and an item at most 95, so the first
 * item always fits. */
typedef struct kvgs_lines {
    char line[384];
    int head;
    int length;
    uint32_t items;
} kvgs_lines;

static void kvgs_lines_flush(kvgs_lines *out)
{
    out->line[out->length] = '\n';
    out->line[out->length + 1] = '\0';
    ISAAC_VITA_LOG_PRINTF("%s", out->line);
    out->length = out->head;
    out->items = 0U;
}

static int kvgs_lines_begin(kvgs_lines *out, const char *record,
                            const char *build_id, uint32_t window,
                            uint32_t last_outer_loop, const char *rest)
{
    int length = sceClibSnprintf(
        out->line, sizeof out->line, "[kage-vita] %s bid=%.32s win=%u loops=%u %s",
        record, build_id, window, last_outer_loop, rest);

    out->items = 0U;
    if (length < 0 || length > (int)sizeof out->line - 98)
        return 0;
    out->head = out->length = length;
    return 1;
}

static void kvgs_lines_add(kvgs_lines *out, const char *item)
{
    size_t length = strlen(item);

    if (length > 95U)
        return;
    if (out->items &&
            (size_t)out->length + 1U + length > sizeof out->line - 2U)
        kvgs_lines_flush(out);
    if (out->items)
        out->line[out->length++] = ',';
    memcpy(out->line + out->length, item, length);
    out->length += (int)length;
    ++out->items;
}

static void kvgs_lines_end(kvgs_lines *out, int always)
{
    if (out->items || always)
        kvgs_lines_flush(out);
}
#endif

#if defined(ISAAC_VITA_GUEST_SAMPLER_RESIDUAL)
static uint32_t kvgs_lookup(const kvgs_buffer *buffer, uint32_t key)
{
    uint32_t index = (key * UINT32_C(2654435761)) >> (32U - KVGS_SLOT_BITS);
    uint32_t probe;

    for (probe = 0U; probe < KVGS_PROBE_LIMIT; ++probe) {
        const kvgs_entry *entry =
            &buffer->slots[(index + probe) & (KVGS_SLOTS - 1U)];
        if (entry->key == key)
            return entry->count;
        if (entry->key == 0U)
            return 0U;
    }
    return 0U;
}

/* ph120.hotw / ph120.hotl / ph120.hcl for the render phase. */
static void kvgs_report_residual(const kvgs_buffer *retired,
                                 const char *build_id, uint32_t window,
                                 uint32_t last_outer_loop)
{
    const uint32_t phase = KVGS_CHAIN_PHASE;
    kvgs_lines out;
    char rest[64];
    char item[96];
    uint32_t i;

    /* w=rva:self/caller/incl/root/root_native/lua_root/lua_root_native */
    sceClibSnprintf(rest, sizeof rest, "phase=%s samples=%u w=",
                    s_phase_names[phase], retired->samples[phase]);
    if (kvgs_lines_begin(&out, "ph120.hotw", build_id, window, last_outer_loop,
                         rest)) {
        for (i = 0U; i < kvgs_watch_count(); ++i) {
            uint32_t rva = s_watch[i];
            uint32_t v[7];
            v[0] = kvgs_lookup(retired, KVGS_KEY(0U, phase, rva));
            v[1] = kvgs_lookup(retired, KVGS_KEY(1U, phase, rva));
            v[2] = kvgs_lookup(retired, KVGS_KEY(1U, phase, KVGS_INCL_TAG | rva));
            v[3] = kvgs_lookup(retired, KVGS_KEY(1U, phase, KVGS_ROOT_TAG | rva));
            v[4] = kvgs_lookup(retired, KVGS_KEY(1U, phase, KVGS_ROOT_TAG |
                                                 KVGS_ROOT_EXT | rva));
            v[5] = kvgs_lookup(retired, KVGS_KEY(1U, phase, KVGS_ROOT_TAG |
                                                 KVGS_ROOT_LUA | rva));
            v[6] = kvgs_lookup(retired, KVGS_KEY(1U, phase, KVGS_ROOT_TAG |
                                                 KVGS_ROOT_EXT | KVGS_ROOT_LUA |
                                                 rva));
            if (!(v[0] | v[1] | v[2] | v[3] | v[4] | v[5] | v[6]))
                continue;
            sceClibSnprintf(item, sizeof item, "%x:%u/%u/%u/%u/%u/%u/%u",
                            (unsigned)rva, (unsigned)v[0], (unsigned)v[1],
                            (unsigned)v[2], (unsigned)v[3], (unsigned)v[4],
                            (unsigned)v[5], (unsigned)v[6]);
            kvgs_lines_add(&out, item);
        }
        kvgs_lines_end(&out, 1);
    }
    /* o=outer_root:t/n/lt/ln, outer 0 = no root outside the invoker */
    sceClibSnprintf(rest, sizeof rest, "phase=%s samples=%u inv=%x o=",
                    s_phase_names[phase], retired->samples[phase],
                    (unsigned)KVGS_LUA_INVOKER_RVA);
    if (kvgs_lines_begin(&out, "ph120.hotl", build_id, window, last_outer_loop,
                         rest)) {
        for (i = 0U; i <= KVGS_ROOT_COUNT; ++i) {
            const uint32_t *v = retired->lua_split[i];
            if (!(v[0] | v[1] | v[2] | v[3]))
                continue;
            sceClibSnprintf(item, sizeof item, "%x:%u/%u/%u/%u",
                            (unsigned)(i < KVGS_ROOT_COUNT ? s_chain_roots[i] : 0U),
                            (unsigned)v[0], (unsigned)v[1], (unsigned)v[2],
                            (unsigned)v[3]);
            kvgs_lines_add(&out, item);
        }
        kvgs_lines_end(&out, 1);
    }
    ISAAC_VITA_LOG_PRINTF(
        "[kage-vita] ph120.hcl bid=%.32s win=%u lb=%u lbm=%u lbs=%u lbx=%u "
        "watch=%u\n",
        build_id, window, retired->lua_links, retired->lua_miss,
        retired->lua_self, retired->lua_stale, kvgs_watch_count());
}
#endif

#if defined(ISAAC_VITA_PROFILE_FUNCTION_ENTRY_CENSUS)
/* ph120.fe: body entries of the watch list in this window, per phase
 * (e=rva:entries, non-zero only; upd and rnd always print). */
static void kvgs_report_census(const char *build_id, uint32_t window,
                               uint32_t last_outer_loop)
{
    uint32_t phase;

    if (kvgs_census_stale()) {
        ISAAC_VITA_LOG_PRINTF(
            "[kage-vita] ph120.fe bid=%.32s win=%u loops=%u ids=STALE\n",
            build_id, window, last_outer_loop);
        memset(g_guest_entry_census, 0, sizeof g_guest_entry_census);
        return;
    }
    for (phase = 0U; phase < KAGE_VITA_GUEST_SAMPLER_PHASE_COUNT; ++phase) {
        const uint32_t *counts = g_guest_entry_census[phase];
        kvgs_lines out;
        char rest[32];
        char item[32];
        uint32_t i;
        uint32_t any = 0U;

        for (i = 0U; i < GUEST_ENTRY_CENSUS_COUNT; ++i)
            any |= counts[i];
        if (!any && phase != KAGE_VITA_GUEST_SAMPLER_UPD &&
                phase != KAGE_VITA_GUEST_SAMPLER_RND)
            continue;
        sceClibSnprintf(rest, sizeof rest, "phase=%s e=", s_phase_names[phase]);
        if (!kvgs_lines_begin(&out, "ph120.fe", build_id, window,
                              last_outer_loop, rest))
            continue;
        for (i = 0U; i < GUEST_ENTRY_CENSUS_COUNT; ++i) {
            if (!counts[i])
                continue;
            sceClibSnprintf(item, sizeof item, "%x:%u",
                            (unsigned)s_census_rvas[i], (unsigned)counts[i]);
            kvgs_lines_add(&out, item);
        }
        kvgs_lines_end(&out, 1);
    }
    memset(g_guest_entry_census, 0, sizeof g_guest_entry_census);
}
#endif

void kage_vita_guest_sampler_report(
    const char *build_id, uint32_t window, uint32_t last_outer_loop)
{
    kvgs_buffer *retired;
    uint32_t old;
    uint32_t spins = 0U;
    uint32_t phase;
    uint32_t total = 0U;

#if defined(ISAAC_VITA_PROFILE_FUNCTION_ENTRY_CENSUS)
    kvgs_report_census(build_id, window, last_outer_loop);
#endif
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
#if defined(ISAAC_VITA_GUEST_SAMPLER_CHAIN)
        kvgs_chain_top incl;
        kvgs_chain_top roots;
        memset(&incl, 0, sizeof incl);
        memset(&roots, 0, sizeof roots);
#endif
#if defined(ISAAC_VITA_GUEST_SAMPLER_RESIDUAL)
        kvgs_chain_top unrooted;
        memset(&unrooted, 0, sizeof unrooted);
#endif
        for (i = 0U; i < KVGS_SLOTS; ++i) {
            const kvgs_entry *entry = &retired->slots[i];
            if (!entry->key || KVGS_KEY_PHASE(entry->key) != phase)
                continue;
#if defined(ISAAC_VITA_GUEST_SAMPLER_CHAIN)
            if (KVGS_KEY_VIEW(entry->key) && KVGS_IS_INCL(KVGS_KEY_RVA(entry->key))) {
                kvgs_chain_top_insert(&incl, KVGS_KEY_RVA(entry->key) &
                                      ~KVGS_INCL_TAG, entry->count);
                continue;
            }
            if (KVGS_KEY_VIEW(entry->key) && KVGS_IS_ROOT(KVGS_KEY_RVA(entry->key))) {
                kvgs_chain_top_insert(&roots, KVGS_KEY_RVA(entry->key) &
                                      ~KVGS_ROOT_TAG, entry->count);
                continue;
            }
#endif
#if defined(ISAAC_VITA_GUEST_SAMPLER_RESIDUAL)
            if (KVGS_KEY_VIEW(entry->key) &&
                    KVGS_IS_UNROOTED(KVGS_KEY_RVA(entry->key))) {
                kvgs_chain_top_insert(&unrooted, KVGS_KEY_RVA(entry->key) &
                                      ~KVGS_UNROOTED_TAG, entry->count);
                continue;
            }
#endif
            kvgs_top_insert(KVGS_KEY_VIEW(entry->key) ? &caller : &self,
                            KVGS_KEY_RVA(entry->key), entry->count);
        }
        kvgs_log_top("ph120.hot", build_id, window, last_outer_loop, phase,
                     retired->samples[phase], &self);
        kvgs_log_top("ph120.hotc", build_id, window, last_outer_loop, phase,
                     retired->samples[phase], &caller);
#if defined(ISAAC_VITA_GUEST_SAMPLER_CHAIN)
        /* Three records of twelve, same format as ph120.hot. */
        for (i = 0U; i < incl.used; i += KVGS_TOP) {
            kvgs_top part;
            uint32_t k;
            memset(&part, 0, sizeof part);
            for (k = 0U; k < KVGS_TOP && i + k < incl.used; ++k) {
                part.rva[k] = incl.rva[i + k];
                part.count[k] = incl.count[i + k];
                part.used = k + 1U;
            }
            kvgs_log_top("ph120.hoti", build_id, window, last_outer_loop, phase,
                         retired->samples[phase], &part);
        }
        /* ph120.hotr: rva = root (0 = none), +1000000 = not translated. */
        for (i = 0U; i < roots.used; i += KVGS_TOP) {
            kvgs_top part;
            uint32_t k;
            memset(&part, 0, sizeof part);
            for (k = 0U; k < KVGS_TOP && i + k < roots.used; ++k) {
                part.rva[k] = roots.rva[i + k];
                part.count[k] = roots.count[i + k];
                part.used = k + 1U;
            }
            kvgs_log_top("ph120.hotr", build_id, window, last_outer_loop, phase,
                         retired->samples[phase], &part);
        }
#endif
#if defined(ISAAC_VITA_GUEST_SAMPLER_RESIDUAL)
        /* ph120.hotu: rootless samples by the outermost function their walk
         * reached (0 = none), +1000000 = the sample is native. */
        for (i = 0U; i < unrooted.used; i += KVGS_TOP) {
            kvgs_top part;
            uint32_t k;
            memset(&part, 0, sizeof part);
            for (k = 0U; k < KVGS_TOP && i + k < unrooted.used; ++k) {
                part.rva[k] = unrooted.rva[i + k];
                part.count[k] = unrooted.count[i + k];
                part.used = k + 1U;
            }
            kvgs_log_top("ph120.hotu", build_id, window, last_outer_loop, phase,
                         retired->samples[phase], &part);
        }
        if (phase == KVGS_CHAIN_PHASE)
            kvgs_report_residual(retired, build_id, window, last_outer_loop);
#endif
    }
#if defined(ISAAC_VITA_GUEST_SAMPLER_CHAIN)
    /* Worst case: 26 + 32 + 10 * 10 + fixed text < 260 bytes. */
    ISAAC_VITA_LOG_PRINTF(
        "[kage-vita] ph120.hci bid=%.32s win=%u n=%u links=%u ind=%u "
        "thunk=%u skip=%u full=%u indrej=%u us=%u,%u\n",
        build_id, window, retired->chain_samples, retired->chain_links,
        retired->chain_ind, retired->chain_thunk, retired->chain_skip,
        retired->chain_full, retired->chain_ind_rejected, retired->chain_us,
        retired->chain_us_max);
#endif
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

/* --------------------------------------------------------------- stop ---- */

void kage_vita_guest_sampler_stop(void)
{
    uint32_t spins = 0U;

    if (s_thread < 0)
        return;
    /* The thread has no exit path (it is left to process exit, as before);
     * unbinding the CPU is what makes the guest stack releasable.
     * kvgs_sample publishes busy and then loads the CPU (SEQ_CST on both
     * sides), so after this store either the sampler already sees NULL or
     * busy is set and the bounded spin outlasts the sample in flight
     * (microseconds; the bound only covers a descheduled sampler, like the
     * reporter's). */
    __atomic_store_n(&s_cpu, (CPU *)0, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&s_busy, __ATOMIC_SEQ_CST) &&
           spins < KVGS_REPORT_SPIN)
        ++spins;
    isaac_vita_log("KAGE VITA GUEST SAMPLER: stopped ticks=%u busy_spins=%u",
                   (unsigned)__atomic_load_n(&s_ticks, __ATOMIC_RELAXED),
                   (unsigned)spins);
}
