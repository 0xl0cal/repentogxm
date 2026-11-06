/* LuaBridge binding replays (ISAAC_VITA_LUA_BINDING_REPLAY).  Design and
 * exactness argument in host_vita_lua_binding_replay.h.
 *
 * Compiled into the eboot when ISAAC_VITA_LUA_BINDING_REPLAY is ON, with the
 * generated-unit code generation definitions (GUEST_GPR_LOCAL=1,
 * GUEST_FLAGS_LOCAL=1, GUEST_GENERATED_STACK_GUARD=0, GUEST_IMPORT_DIRECT and
 * GUEST_COVERAGE_HOOKS as the corpus units get them) because the mirrors in
 * host_vita_lua_binding_mirror.h ARE corpus text, and with one
 * -Wl,--wrap=sub_<rva> per replayed body of each enabled family.  The same
 * source builds the 32-bit host oracle (recomp/test_vita_lua_binding_replay.py)
 * that links the translated bodies as the __real_ reference.
 *
 * Every import crossing below is transcribed from the endpoint it replaces;
 * the citations are recomp/runtime/host_vita_lua.c line numbers at this
 * commit.  Nothing in this file is guest-equivalent code written from
 * memory: the statement stream is the generated text, only the LBR_* macros
 * are hand-written.  The table and mirror headers are configure-generated. */
#include "host_vita_lua_binding_replay.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "host_vita_import_id.h"
#include "host_vita_lua.h"
#include "host_vita_lua_binding_table.h"

#if !defined(ISAAC_VITA_LUA_BINDING_REPLAY)
#error The LuaBridge binding replay must only be compiled for its opt-in feature
#endif
#if !defined(ISAAC_VITA_LUA_NATIVE_GETCLASS)
#error ISAAC_VITA_LUA_BINDING_REPLAY requires ISAAC_VITA_LUA_NATIVE_GETCLASS (bridge owner seam API and the getClass --wrap)
#endif
/* The corpus spells IAT slots and .rdata pointers as absolute literals for
 * this base (`ld32((uint32_t)(int32_t)(-1738513908))` is [0x9860620c]). */
#if GUEST_IMAGE_BASE != 0x98000000u
#error the mirrored corpus text assumes GUEST_IMAGE_BASE 0x98000000
#endif
/* The mirrors are proven in the production generated-unit modes only. */
#if !GUEST_GPR_LOCAL
#error ISAAC_VITA_LUA_BINDING_REPLAY needs GUEST_GPR_LOCAL=1 (ISAAC_VITA_TRANSLATED_CPU_GPR_LOCAL)
#endif
#if !(defined(GUEST_FLAGS_LOCAL) && GUEST_FLAGS_LOCAL)
#error ISAAC_VITA_LUA_BINDING_REPLAY needs GUEST_FLAGS_LOCAL=1 (ISAAC_VITA_TRANSLATED_CPU_FLAGS_LOCAL)
#endif
#if GUEST_GENERATED_STACK_GUARD
#error ISAAC_VITA_LUA_BINDING_REPLAY needs the raw generated stack (ISAAC_VITA_GUEST_STACK_GUARD=OFF)
#endif
#if defined(ISAAC_VITA_STALL_PROBE)
#error ISAAC_VITA_LUA_BINDING_REPLAY elides the dispatch breadcrumbs ISAAC_VITA_STALL_PROBE records
#endif
#if (ISAAC_VITA_LUA_BINDING_REPLAY_FAMILY_MASK & 0xfU) == 0U || \
    (ISAAC_VITA_LUA_BINDING_REPLAY_FAMILY_MASK & ~0xfU) != 0U
#error ISAAC_VITA_LUA_BINDING_REPLAY_FAMILY_MASK must be a non-empty subset of 0xf
#endif
#define LBR_HAS_SCALAR     ((ISAAC_VITA_LUA_BINDING_REPLAY_FAMILY_MASK & ISAAC_LBR_FAMILY_BIT_SCALAR) != 0U)
#define LBR_HAS_SINGLE     ((ISAAC_VITA_LUA_BINDING_REPLAY_FAMILY_MASK &       (ISAAC_LBR_FAMILY_BIT_GETDATA | ISAAC_LBR_FAMILY_BIT_BOOLII |        ISAAC_LBR_FAMILY_BIT_INDEX2)) != 0U)

/* recomp/vita/platform.h; declared here so the host oracle can stub it. */
void isaac_vita_log(const char *format, ...);

#ifndef ISAAC_VITA_LUA_BINDING_REPLAY_BUILD_ID
#define ISAAC_VITA_LUA_BINDING_REPLAY_BUILD_ID "binding-replay:unstamped"
#endif

/* Bytes below the entry ESP the replay may use for emulated endpoint frames:
 * the deepest one is 3f90b0's lua_settop at E-88 (frame 12); 446d90's
 * 280cc0 helper reaches E-76.  A crossing whose frame is not inside
 * [E - WINDOW, E + 8) is made for real, so the value is a performance knob,
 * not a correctness one (the oracle shrinks it to force real crossings). */
#ifndef ISAAC_LBR_WINDOW_BELOW
#define ISAAC_LBR_WINDOW_BELOW 0x100U
#endif
#define LBR_NO_COVERAGE 4294967295U
#define LBR_STATS_PERIOD 0x10000U
#define LBR_VA(rva) ((uint32_t)GUEST_IMAGE_BASE + (uint32_t)(rva))

#if defined(__GNUC__) || defined(__clang__)
#define LBR_LIKELY(v) __builtin_expect(!!(v), 1)
#define LBR_UNLIKELY(v) __builtin_expect(!!(v), 0)
#else
#define LBR_LIKELY(v) (!!(v))
#define LBR_UNLIKELY(v) (!!(v))
#endif

/* ---- rows ------------------------------------------------------------- */

typedef struct lbr_row {
    uint32_t rva;       /* translated body (template A) */
    uint32_t cb;        /* canBeConst byte the body pushes */
    uint32_t key;       /* class key VA the body pushes */
    uint32_t helper;    /* 0x3f8820 (signed) or 0x3f8840 (unsigned) */
    uint32_t coverage;  /* body coverage id, LBR_NO_COVERAGE when none */
} lbr_row;

/* One replayed call.  `drift` set: every later crossing of this call is the
 * real one (a check failed after a guest observation point). */
typedef struct lbr_ctx {
    uint32_t lo;        /* E - ISAAC_LBR_WINDOW_BELOW */
    uint32_t span;      /* ISAAC_LBR_WINDOW_BELOW + 8 */
    int drift;
} lbr_ctx;

#if LBR_HAS_SCALAR
#define LBR_ROW_INIT(sym, rva, cb, key, helper, cov) { rva, cb, key, helper, cov },
static const lbr_row s_rows[ISAAC_LBR_SCALAR_COUNT] = {
    ISAAC_LBR_SCALAR_ROWS(LBR_ROW_INIT)
};
#undef LBR_ROW_INIT
#define LBR_ROW_INDEX(sym, rva, cb, key, helper, cov) LBR_IDX_##sym,
enum { ISAAC_LBR_SCALAR_ROWS(LBR_ROW_INDEX) LBR_IDX_COUNT };
#undef LBR_ROW_INDEX
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(LBR_IDX_COUNT == ISAAC_LBR_SCALAR_COUNT, "scalar row count");
#endif
#endif

/* The 17 imports, dense position k in ISAAC_LBR_IMPORTS order. */
#define LBR_IMPORT_K(NAME, slot, id, typed, name) LBR_K_##NAME,
enum { ISAAC_LBR_IMPORTS(LBR_IMPORT_K) LBR_K_COUNT };
#undef LBR_IMPORT_K

typedef struct lbr_import {
    uint32_t slot;
    uint32_t id;
    const char *name;
} lbr_import;
#define LBR_IMPORT_INIT(NAME, slot, id, typed, name) { slot, id, name },
static const lbr_import s_imports[LBR_K_COUNT] = {
    ISAAC_LBR_IMPORTS(LBR_IMPORT_INIT)
};
#undef LBR_IMPORT_INIT

/* ---- state ------------------------------------------------------------ */

static isaac_vita_lua_binding_replay_stats s_stats;
static int s_banner_logged;
static int s_global_pins;                         /* 0 unchecked, 1 ok, -1 bad */
static signed char s_row_pins[ISAAC_LBR_SCALAR_COUNT];
static signed char s_single_pins[3];              /* GETDATA, BOOLII, INDEX2 */
/* 1: the import's endpoint is the typed fast spelling (its leave stores the
 * entered owner back), 0: the generic handler.  Filled by the global pin. */
static unsigned char s_import_typed[LBR_K_COUNT];
static uint32_t s_reason_logged;
static uint32_t s_drift_logged;

static const char *const s_family_names[ISAAC_LBR_FAMILY_COUNT] = {
    "scalar", "getdata", "boolii", "index2"
};
static const char *const s_reason_names[ISAAC_LBR_REASON_COUNT] = {
    "handled", "stack", "owner", "pins"
};

const isaac_vita_lua_binding_replay_stats *
isaac_vita_lua_binding_replay_stats_get(void)
{
    return &s_stats;
}

void isaac_vita_lua_binding_replay_stats_reset(void)
{
    memset(&s_stats, 0, sizeof s_stats);
    s_banner_logged = 0;
    s_global_pins = 0;
    memset(s_row_pins, 0, sizeof s_row_pins);
    memset(s_single_pins, 0, sizeof s_single_pins);
    memset(s_import_typed, 0, sizeof s_import_typed);
    s_reason_logged = 0U;
    s_drift_logged = 0U;
}

void isaac_vita_lua_binding_replay_stats_log(void)
{
    uint32_t calls = 0U, handled = 0U, fallbacks = 0U, f;
    for (f = 0U; f < ISAAC_LBR_FAMILY_COUNT; ++f) {
        calls += s_stats.calls[f];
        handled += s_stats.handled[f];
        fallbacks += s_stats.fallbacks[f];
    }
    isaac_vita_log(
        "[isaac-lua] binding replay stats: calls=%u replayed=%u fallbacks=%u "
        "scalar=%u/%u/%u getdata=%u/%u/%u boolii=%u/%u/%u index2=%u/%u/%u "
        "imports_elided=%u imports_real=%u drift=%u pins_disabled=%u "
        "verify=%u/%u fb(stack,owner,pins)=%u,%u,%u",
        (unsigned)calls, (unsigned)handled, (unsigned)fallbacks,
        (unsigned)s_stats.calls[0], (unsigned)s_stats.handled[0],
        (unsigned)s_stats.fallbacks[0],
        (unsigned)s_stats.calls[1], (unsigned)s_stats.handled[1],
        (unsigned)s_stats.fallbacks[1],
        (unsigned)s_stats.calls[2], (unsigned)s_stats.handled[2],
        (unsigned)s_stats.fallbacks[2],
        (unsigned)s_stats.calls[3], (unsigned)s_stats.handled[3],
        (unsigned)s_stats.fallbacks[3],
        (unsigned)s_stats.imports_elided, (unsigned)s_stats.imports_real,
        (unsigned)s_stats.drift_calls, (unsigned)s_stats.pins_disabled,
        (unsigned)s_stats.verify_mismatches, (unsigned)s_stats.verify_runs,
        (unsigned)s_stats.reasons[ISAAC_LBR_REASON_STACK],
        (unsigned)s_stats.reasons[ISAAC_LBR_REASON_OWNER],
        (unsigned)s_stats.reasons[ISAAC_LBR_REASON_PINS]);
}

/* ---- pins --------------------------------------------------------------- */

static void lbr_pin_failed(const char *what, uint32_t where)
{
    ++s_stats.pins_disabled;
    isaac_vita_log("[isaac-lua] binding replay pin MISMATCH: %s at %08x "
                   "(translated bodies used)", what, (unsigned)where);
}

#if LBR_HAS_SCALAR
static void lbr_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8U);
    p[2] = (uint8_t)(v >> 16U);
    p[3] = (uint8_t)(v >> 24U);
}

/* The mapped template A bytes of one row (holes filled from the row):
 * push cb at +29, push key at +31, call 0x3f8e30 rel32 at +43, call helper
 * rel32 at +79 (host_vita_lua_binding_table.h). */
static int lbr_row_bytes_match(const lbr_row *row)
{
    static const uint8_t tmpl[ISAAC_LBR_TEMPLATE_A_SIZE] =
        ISAAC_LBR_TEMPLATE_A_BYTES;
    uint8_t want[ISAAC_LBR_TEMPLATE_A_SIZE];

    memcpy(want, tmpl, sizeof want);
    want[ISAAC_LBR_TEMPLATE_A_HOLE_CB] = (uint8_t)row->cb;
    lbr_put32(want + ISAAC_LBR_TEMPLATE_A_HOLE_KEY, row->key);
    lbr_put32(want + ISAAC_LBR_TEMPLATE_A_HOLE_GETCLASS,
              0x003f8e30U - (row->rva + ISAAC_LBR_TEMPLATE_A_HOLE_GETCLASS + 4U));
    lbr_put32(want + ISAAC_LBR_TEMPLATE_A_HOLE_HELPER,
              row->helper - (row->rva + ISAAC_LBR_TEMPLATE_A_HOLE_HELPER + 4U));
    return memcmp((const void *)(uintptr_t)LBR_VA(row->rva), want,
                  sizeof want) == 0;
}

static int lbr_row_pins_ok(uint32_t index)
{
    if (s_row_pins[index] == 0) {
        s_row_pins[index] = lbr_row_bytes_match(&s_rows[index]) ? 1 : -1;
        if (s_row_pins[index] < 0)
            lbr_pin_failed("template A row", s_rows[index].rva);
    }
    return s_row_pins[index] > 0;
}
#endif

#if LBR_HAS_SINGLE
static int lbr_single_pins_ok(unsigned family)
{
    static const uint8_t getdata[ISAAC_LBR_GETDATA_SIZE] =
        ISAAC_LBR_GETDATA_BYTES;
    static const uint8_t boolii[ISAAC_LBR_BOOLII_SIZE] =
        ISAAC_LBR_BOOLII_BYTES;
    static const uint8_t index2[ISAAC_LBR_INDEX2_SIZE] =
        ISAAC_LBR_INDEX2_BYTES;
    unsigned slot = family - ISAAC_LBR_FAMILY_GETDATA;
    const uint8_t *bytes;
    uint32_t rva, size;

    if (s_single_pins[slot] == 0) {
        if (family == ISAAC_LBR_FAMILY_GETDATA) {
            bytes = getdata; rva = ISAAC_LBR_GETDATA_RVA; size = sizeof getdata;
        } else if (family == ISAAC_LBR_FAMILY_BOOLII) {
            bytes = boolii; rva = ISAAC_LBR_BOOLII_RVA; size = sizeof boolii;
        } else {
            bytes = index2; rva = ISAAC_LBR_INDEX2_RVA; size = sizeof index2;
        }
        s_single_pins[slot] =
            memcmp((const void *)(uintptr_t)LBR_VA(rva), bytes, size) == 0
                ? 1 : -1;
        if (s_single_pins[slot] < 0)
            lbr_pin_failed(s_family_names[family], rva);
    }
    return s_single_pins[slot] > 0;
}
#endif

/* Once, at the first call (the 413-row import registration ran before any
 * guest code): each of the 17 IAT words is its own tag VA, the slot is the
 * named Lua import, and both dispatch routes (guest_import_call's direct
 * table and guest_call -> guest_host_import_id) bind it to the Lua family
 * endpoint of that name -- so the endpoint each crossing replaces is known.
 * Plus the two .rdata strings 3f90b0 hands to lua_pushstring. */
static int lbr_global_pins_check(void)
{
    uint32_t k;

    for (k = 0U; k < LBR_K_COUNT; ++k) {
        const lbr_import *imp = &s_imports[k];
        uint32_t va = LBR_VA(imp->slot);
        guest_import_family_fn fn = NULL;
        uint32_t local = 0U;
        const char *name = guest_import_name(va);
        const char *local_name;

        if (ld32(va) != va) {
            lbr_pin_failed("IAT tag word", imp->slot);
            return 0;
        }
        if (!name || strcmp(name, imp->name) != 0) {
            lbr_pin_failed("IAT slot import name", imp->slot);
            return 0;
        }
        if (!guest_host_import_id_direct_binding(imp->id, &fn, &local) ||
            !fn) {
            lbr_pin_failed("import binding", imp->slot);
            return 0;
        }
        local_name = isaac_vita_lua_import_name(local);
        if (!local_name || strcmp(local_name, imp->name) != 0) {
            lbr_pin_failed("Lua local import name", imp->slot);
            return 0;
        }
        if (fn == isaac_vita_lua_import_indexed) {
            s_import_typed[k] = 0U;
        }
#if defined(ISAAC_VITA_LUA_IMPORT_FASTDISPATCH)
        else if (fn == isaac_vita_lua_import_fast_binding(local)) {
            s_import_typed[k] = 1U;
        }
#endif
        else {
            lbr_pin_failed("Lua family endpoint", imp->slot);
            return 0;
        }
    }
    if (strcmp((const char *)(uintptr_t)LBR_VA(ISAAC_LBR_STR_PROPGET_RVA),
               "__propget") != 0) {
        lbr_pin_failed(".rdata __propget", ISAAC_LBR_STR_PROPGET_RVA);
        return 0;
    }
    if (strcmp((const char *)(uintptr_t)LBR_VA(ISAAC_LBR_STR_PARENT_RVA),
               "__parent") != 0) {
        lbr_pin_failed(".rdata __parent", ISAAC_LBR_STR_PARENT_RVA);
        return 0;
    }
    return 1;
}

static int lbr_global_pins_ok(void)
{
    if (s_global_pins == 0)
        s_global_pins = lbr_global_pins_check() ? 1 : -1;
    return s_global_pins > 0;
}

static void lbr_log_banner(void)
{
    uint32_t k, typed = 0U;
    int pins;

    if (s_banner_logged)
        return;
    s_banner_logged = 1;
    pins = lbr_global_pins_ok();
    for (k = 0U; k < LBR_K_COUNT; ++k)
        typed += s_import_typed[k];
    isaac_vita_log(
        "[isaac-lua] binding replay: rows=%u+3 families=0x%x window=%u "
        "imports=%u typed=%u pins=%s verify=%d build=%s",
        (unsigned)ISAAC_LBR_SCALAR_COUNT,
        (unsigned)ISAAC_VITA_LUA_BINDING_REPLAY_FAMILY_MASK,
        (unsigned)ISAAC_LBR_WINDOW_BELOW, (unsigned)LBR_K_COUNT,
        (unsigned)typed, pins ? "ok" : "MISMATCH",
#if defined(ISAAC_VITA_LUA_BINDING_REPLAY_VERIFY)
        1,
#else
        0,
#endif
        ISAAC_VITA_LUA_BINDING_REPLAY_BUILD_ID);
}

/* ---- call boundary ---------------------------------------------------- */

static int lbr_fallback(unsigned family, uint32_t reason)
{
    ++s_stats.fallbacks[family];
    ++s_stats.reasons[reason];
    s_stats.last_reason = reason;
    if ((s_reason_logged & (1U << reason)) == 0U) {
        s_reason_logged |= 1U << reason;
        isaac_vita_log("[isaac-lua] binding replay fallback: family=%s "
                       "reason=%s call=%u (translated body used)",
                       s_family_names[family], s_reason_names[reason],
                       (unsigned)s_stats.calls[family]);
    }
    return 0;
}

/* Entry pre-checks; nothing is written before they pass except the owner
 * word round trip of a root seam_enter, which seam_leave undoes. */
static int lbr_enter(CPU *__restrict c, lbr_ctx *__restrict x,
                     unsigned family, int row_pins_ok)
{
    uint32_t e = c->esp;
    int root;

    ++s_stats.calls[family];
    lbr_log_banner();
    if (!row_pins_ok || !lbr_global_pins_ok())
        return lbr_fallback(family, ISAAC_LBR_REASON_PINS);
    if (e < ISAAC_LBR_WINDOW_BELOW ||
        !guest_stack_contains(c, e - ISAAC_LBR_WINDOW_BELOW,
                              ISAAC_LBR_WINDOW_BELOW + 8U))
        return lbr_fallback(family, ISAAC_LBR_REASON_STACK);
    /* A Lua C function runs inside the bridge activation that called it, so
     * the owner word names this CPU (root == 0).  root == 1 means no import
     * scope is open: the translated crossings would then each take and drop
     * the slot, which the replay does not model. */
    if (!isaac_vita_lua_seam_enter(c, &root))
        return lbr_fallback(family, ISAAC_LBR_REASON_OWNER);
    if (root) {
        isaac_vita_lua_seam_leave(c, root);
        return lbr_fallback(family, ISAAC_LBR_REASON_OWNER);
    }
    x->lo = e - ISAAC_LBR_WINDOW_BELOW;
    x->span = ISAAC_LBR_WINDOW_BELOW + 8U;
    x->drift = 0;
    return 1;
}

static void lbr_leave(const lbr_ctx *__restrict x, unsigned family)
{
    uint32_t handled;

    ++s_stats.handled[family];
    s_stats.last_reason = ISAAC_LBR_HANDLED;
    if (x->drift)
        ++s_stats.drift_calls;
    handled = s_stats.handled[0] + s_stats.handled[1] + s_stats.handled[2] +
              s_stats.handled[3];
    if (handled == 1U || (handled % LBR_STATS_PERIOD) == 0U)
        isaac_vita_lua_binding_replay_stats_log();
}

static void lbr_set_drift(lbr_ctx *__restrict x, unsigned why)
{
    static const char *const names[3] = {
        "stack", "owner", "owner-foreign"
    };
    x->drift = 1;
    if ((s_drift_logged & (1U << why)) == 0U) {
        s_drift_logged |= 1U << why;
        isaac_vita_log("[isaac-lua] binding replay drift: %s changed after a "
                       "guest observation point (later crossings real)",
                       names[why]);
    }
}

/* After every point where guest code or a foreign handler may have run
 * (getClass, a member, a real crossing, lua_pushstring's GC step, lua_callk):
 * the window must still be inside the bound guest stack and the owner word
 * must still name this CPU, else the rest of the call crosses for real.
 * `typed`: the replaced endpoint was the typed fast spelling, whose leave
 * stores the entered owner (this CPU) back (host_vita_lua.c:2813); the
 * generic nested leave (host_vita_lua.c:257) stores nothing. */
static void lbr_observed(CPU *__restrict c, lbr_ctx *__restrict x, int typed)
{
    int root;

    if (x->drift)
        return;
    if (!guest_stack_contains(c, x->lo, x->span)) {
        lbr_set_drift(x, 0U);
        return;
    }
    if (LBR_LIKELY(isaac_vita_lua_seam_enter(c, &root))) {
        if (LBR_LIKELY(!root))
            return;
        /* The word was 0.  seam_enter just stored this CPU -- exactly the
         * typed leave; the generic one leaves 0, so give it back. */
        if (!typed)
            isaac_vita_lua_seam_leave(c, root);
        lbr_set_drift(x, 1U);
        return;
    }
    /* Another CPU's word (impossible with the Vita's single guest CPU): the
     * generic leave keeps it; a typed leave would have stored this CPU,
     * which the seam API cannot do -- logged, later crossings real. */
    lbr_set_drift(x, 2U);
}

/* ---- import crossings ---------------------------------------------------- */

/* host_vita_lua.c:316 vita_lua_index / :321 vita_lua_index_result: guest
 * upvalue pseudo-indices move down by one (native upvalue #1 is the bridge's
 * target), registry and stack indices are unchanged. */
static int lbr_index(int32_t index)
{
    return index < LUA_REGISTRYINDEX ? (int)index - 1 : (int)index;
}

static int lbr_index_result(int index)
{
    return index < LUA_REGISTRYINDEX ? index + 1 : index;
}

#if defined(ISAAC_VITA_LUA_BINDING_REPLAY_ORACLE)
/* Host oracle only: forces chosen crossings onto the real route (a seeded
 * schedule), and counts each conditional branch of the mirrors both ways. */
int isaac_lbr_oracle_force_real(uint32_t import_k);
void isaac_lbr_oracle_cond(uint32_t id, int taken);
static int lbr_cond(uint32_t id, int taken)
{
    isaac_lbr_oracle_cond(id, taken);
    return taken;
}
#define LBR_FORCE_REAL(k) isaac_lbr_oracle_force_real(k)
#define LBR_COND(tag, k, cond) lbr_cond(LBR_COND_BASE_##tag + (k), (cond) ? 1 : 0)
#else
#define LBR_FORCE_REAL(k) 0
#define LBR_COND(tag, k, cond) (cond)
#endif
#define LBR_COND_BASE_TA 0U
#define LBR_COND_BASE_GD 1U
#define LBR_COND_BASE_BI 3U
#define LBR_COND_BASE_IX 3U
#define LBR_COND_COUNT 7U

/* Endpoint frame (return word + 4 per argument slot) and the extra guard
 * a crossing needs before it may be emulated. */
#define LBR_FRAME_ISCFUNCTION     12U
#define LBR_FRAME_SETTOP          12U
#define LBR_FRAME_RAWGET          12U
#define LBR_FRAME_PUSHSTRING      12U
#define LBR_FRAME_TOUSERDATA      12U
#define LBR_FRAME_CALLK           24U
#define LBR_FRAME_PUSHBOOLEAN     12U
#define LBR_FRAME_L_CHECKINTEGER  12U
#define LBR_FRAME_TYPE            12U
#define LBR_FRAME_ROTATE          16U
#define LBR_FRAME_GETMETATABLE    12U
#define LBR_FRAME_ABSINDEX        12U
#define LBR_FRAME_PUSHINTEGER     16U
#define LBR_FRAME_L_UNREF         16U
#define LBR_FRAME_RAWGETI         20U
#define LBR_FRAME_L_REF           12U
#define LBR_FRAME_PUSHVALUE       12U
#define LBR_GUARD_ISCFUNCTION     1
#define LBR_GUARD_SETTOP          1
#define LBR_GUARD_RAWGET          1
#define LBR_GUARD_PUSHSTRING      1
#define LBR_GUARD_TOUSERDATA      1
/* host_vita_lua.c:1951: a non-zero continuation faults; that stays real. */
#define LBR_GUARD_CALLK           (ld32(GR(esp) + 20U) == 0U)
#define LBR_GUARD_PUSHBOOLEAN     1
#define LBR_GUARD_L_CHECKINTEGER  1
#define LBR_GUARD_TYPE            1
#define LBR_GUARD_ROTATE          1
#define LBR_GUARD_GETMETATABLE    1
#define LBR_GUARD_ABSINDEX        1
#define LBR_GUARD_PUSHINTEGER     1
#define LBR_GUARD_L_UNREF         1
#define LBR_GUARD_RAWGETI         1
#define LBR_GUARD_L_REF           1
#define LBR_GUARD_PUSHVALUE       1

/* E = GR(esp) after the return word push.  The generic handlers read each
 * argument through guest_stack_address (host_vita_lua.c:269), whose lowest
 * address is E+4, then pop the return word through gpop_at; the typed ones
 * (host_vita_lua.c:2851) note E+4 and pop to E+4.  Inside the validated
 * window both are: note_low(E+4), the API call, EAX/EDX, ESP = E+4. */
#define LBR_W(off)      ld32(GR(esp) + (uint32_t)(off))
#define LBR_STATE()     ((lua_State *)(uintptr_t)LBR_W(4U))
#define LBR_INDEX(off)  lbr_index((int32_t)LBR_W(off))
#define LBR_NOTE()      guest_stack_note_low(c, GR(esp) + 4U)
#define LBR_POP()       ((void)(GR(esp) += 4U))

/* Pure APIs (Lua 5.3.3 lapi.c: no luaC_checkGC, no allocation, no
 * metamethod, no error on a valid frame).  The registers stay in the locals:
 * nothing between this point and the next publication reads the CPU. */
/* host_vita_lua.c:1605 */
#define LBR_EMU_ISCFUNCTION do { LBR_NOTE(); \
    GR(eax) = (uint32_t)lua_iscfunction(LBR_STATE(), LBR_INDEX(8U)); \
    LBR_POP(); } while (0)
/* host_vita_lua.c:1657, typed :2953 */
#define LBR_EMU_SETTOP do { LBR_NOTE(); \
    lua_settop(LBR_STATE(), LBR_INDEX(8U)); \
    LBR_POP(); } while (0)
/* host_vita_lua.c:1746, typed :2934 */
#define LBR_EMU_RAWGET do { LBR_NOTE(); \
    GR(eax) = (uint32_t)lua_rawget(LBR_STATE(), LBR_INDEX(8U)); \
    LBR_POP(); } while (0)
/* host_vita_lua.c:1798 (EAX after the release), typed :2917 */
#define LBR_EMU_TOUSERDATA do { LBR_NOTE(); \
    GR(eax) = (uint32_t)(uintptr_t)lua_touserdata(LBR_STATE(), LBR_INDEX(8U)); \
    LBR_POP(); } while (0)
/* host_vita_lua.c:2009 */
#define LBR_EMU_PUSHBOOLEAN do { LBR_NOTE(); \
    lua_pushboolean(LBR_STATE(), (int32_t)LBR_W(8U)); \
    LBR_POP(); } while (0)
/* host_vita_lua.c:2193, typed :2908 */
#define LBR_EMU_TYPE do { LBR_NOTE(); \
    GR(eax) = (uint32_t)lua_type(LBR_STATE(), LBR_INDEX(8U)); \
    LBR_POP(); } while (0)
/* host_vita_lua.c:2210 */
#define LBR_EMU_ROTATE do { LBR_NOTE(); \
    lua_rotate(LBR_STATE(), LBR_INDEX(8U), (int32_t)LBR_W(12U)); \
    LBR_POP(); } while (0)
/* host_vita_lua.c:2229, typed :2899 */
#define LBR_EMU_GETMETATABLE do { LBR_NOTE(); \
    GR(eax) = (uint32_t)lua_getmetatable(LBR_STATE(), LBR_INDEX(8U)); \
    LBR_POP(); } while (0)
/* host_vita_lua.c:2237 */
#define LBR_EMU_ABSINDEX do { LBR_NOTE(); \
    GR(eax) = (uint32_t)lbr_index_result( \
        lua_absindex(LBR_STATE(), LBR_INDEX(8U))); \
    LBR_POP(); } while (0)
/* host_vita_lua.c:2603 (vita_lua_arg_u64 = lo | hi << 32), typed :2977 */
#define LBR_EMU_PUSHINTEGER do { LBR_NOTE(); \
    lua_pushinteger(LBR_STATE(), (lua_Integer)ld64(GR(esp) + 8U)); \
    LBR_POP(); } while (0)
/* host_vita_lua.c:2635, typed :2943 */
#define LBR_EMU_RAWGETI do { LBR_NOTE(); \
    GR(eax) = (uint32_t)lua_rawgeti(LBR_STATE(), LBR_INDEX(8U), \
                                    (lua_Integer)ld64(GR(esp) + 12U)); \
    LBR_POP(); } while (0)
/* host_vita_lua.c:2660, typed :2926 */
#define LBR_EMU_PUSHVALUE do { LBR_NOTE(); \
    lua_pushvalue(LBR_STATE(), LBR_INDEX(8U)); \
    LBR_POP(); } while (0)

/* Raise-only APIs (luaL_ref/luaL_unref: ERRMEM out of lua_rawseti, whose
 * emergency collection runs no finalizer; luaL_checkinteger: the argument
 * error, whose message formatting may run a GC step and a finalizer before
 * the longjmp).  The CPU is published first, so a raise -- and anything it
 * runs -- sees the translated crossing's CPU; on return nothing ran. */
/* host_vita_lua.c:2125 + vita_lua_return_integer :305 */
#define LBR_EMU_L_CHECKINTEGER do { lua_Integer v_; uint64_t b_; \
    GUEST_GPR_FLUSH(c); LBR_NOTE(); \
    v_ = luaL_checkinteger(LBR_STATE(), LBR_INDEX(8U)); \
    b_ = (uint64_t)v_; \
    GR(eax) = (uint32_t)b_; GR(edx) = (uint32_t)(b_ >> 32U); \
    LBR_POP(); } while (0)
/* host_vita_lua.c:2652 */
#define LBR_EMU_L_REF do { GUEST_GPR_FLUSH(c); LBR_NOTE(); \
    GR(eax) = (uint32_t)luaL_ref(LBR_STATE(), LBR_INDEX(8U)); \
    LBR_POP(); } while (0)
/* host_vita_lua.c:2627 */
#define LBR_EMU_L_UNREF do { GUEST_GPR_FLUSH(c); LBR_NOTE(); \
    luaL_unref(LBR_STATE(), LBR_INDEX(8U), (int32_t)LBR_W(12U)); \
    LBR_POP(); } while (0)

/* Guest-running APIs: lua_pushstring (luaC_checkGC -> a __gc finalizer) and
 * lua_callk.  Published before, reloaded after; the return word is popped
 * with the endpoint's own rule.  host_vita_lua.c:1788 (generic: release,
 * then EAX, then the checked gpop) and :2993 (typed: release, EAX, the
 * checked gpop only when ESP moved, else E+4) -- identical whenever the
 * window holds, which it does at entry to the call. */
#define LBR_EMU_PUSHSTRING do { const char *r_; uint32_t e_ = GR(esp); \
    GUEST_GPR_FLUSH(c); guest_stack_note_low(c, e_ + 4U); \
    r_ = lua_pushstring((lua_State *)(uintptr_t)ld32(e_ + 4U), \
                        (const char *)(uintptr_t)ld32(e_ + 8U)); \
    c->eax = (uint32_t)(uintptr_t)r_; \
    if (LBR_UNLIKELY(c->esp != e_)) (void)gpop(c); else c->esp = e_ + 4U; \
    GUEST_GPR_RELOAD(c); \
    lbr_observed(c, x, s_import_typed[LBR_K_PUSHSTRING]); } while (0)
/* host_vita_lua.c:1951: arg4 checked above (LBR_GUARD_CALLK), then the
 * lowest read E+4, lua_callk(nargs, nresults, (int32) ctx, NULL), the
 * checked gpop. */
#define LBR_EMU_CALLK do { uint32_t e_ = GR(esp); \
    GUEST_GPR_FLUSH(c); guest_stack_note_low(c, e_ + 4U); \
    lua_callk((lua_State *)(uintptr_t)ld32(e_ + 4U), (int32_t)ld32(e_ + 8U), \
              (int32_t)ld32(e_ + 12U), \
              (lua_KContext)(int32_t)ld32(e_ + 16U), NULL); \
    (void)gpop(c); \
    GUEST_GPR_RELOAD(c); \
    lbr_observed(c, x, 0); } while (0)

#if defined(ISAAC_VITA_LUA_BINDING_REPLAY_ORACLE) && defined(ISAAC_LBR_ORACLE_MUTANT)
/* Host oracle negative controls (test_vita_lua_binding_replay.py legs
 * mutant-*): one deliberately wrong emulation each, which the oracle must
 * report by the check named in the leg.  Never defined in a Vita build. */
#if ISAAC_LBR_ORACLE_MUTANT == 1
#undef LBR_EMU_PUSHBOOLEAN
#define LBR_EMU_PUSHBOOLEAN do { LBR_NOTE(); \
    lua_pushboolean(LBR_STATE(), !(int32_t)LBR_W(8U)); \
    LBR_POP(); } while (0)
#elif ISAAC_LBR_ORACLE_MUTANT == 2
#undef LBR_EMU_TYPE
#define LBR_EMU_TYPE do { LBR_NOTE(); \
    GR(eax) = (uint32_t)lua_type(LBR_STATE(), LBR_INDEX(8U)); \
    GR(edx) = 0x0bad0badU; \
    LBR_POP(); } while (0)
#else
#error "unknown ISAAC_LBR_ORACLE_MUTANT"
#endif
#endif

/* The real crossings: exactly the translated statement, with every register
 * published (the corpus publishes the possibly-dirty ones; the rest of the
 * CPU already holds the locals there, so the CPU is the same).
 * Both counters move at the crossing, before anything can raise: a Lua error
 * that unwinds a replayed call (a raising __gc in a GC step, a getter that
 * calls luaL_error, a bad getClass argument) keeps the stats equal to what
 * the import census lost (the oracle checks census_T - census_R == elided). */
#define LBR_REAL_CALL(target, slot, id) do { \
    ++s_stats.imports_real; \
    GUEST_GPR_FLUSH(c); GUEST_IMPORT_CALL((target), (slot), (id)); \
    GUEST_GPR_RELOAD(c); lbr_observed(c, x, 0); } while (0)
#define LBR_REAL_PLAIN(target, slot, id) do { \
    ++s_stats.imports_real; \
    GUEST_GPR_FLUSH(c); guest_call(c, (target)); \
    GUEST_GPR_RELOAD(c); lbr_observed(c, x, 0); } while (0)

/* One import crossing.  `target` is the word the translated statement calls
 * (the IAT slot or the register loaded from it). */
#define LBR_IMPORT(NAME, ROUTE, target, slot, id) do { \
    if (LBR_LIKELY((target) == LBR_VA(slot) && !x->drift && \
                   (uint32_t)(GR(esp) - x->lo) <= \
                       x->span - LBR_FRAME_##NAME && \
                   LBR_GUARD_##NAME && !LBR_FORCE_REAL(LBR_K_##NAME))) { \
        ++s_stats.imports_elided; \
        LBR_EMU_##NAME; \
    } else { \
        LBR_REAL_##ROUTE((target), (slot), (id)); \
    } } while (0)

/* Guest calls the replay keeps as guest calls. */
void sub_003f8e30(CPU *__restrict c);
#define LBR_MEMBER_CALL(target) do { \
    GUEST_GPR_FLUSH(c); guest_call(c, (target)); GUEST_GPR_RELOAD(c); \
    lbr_observed(c, x, 0); } while (0)
/* Userdata::getClass: through the same symbol as the translated bodies, so
 * the link's --wrap=sub_003f8e30 (ISAAC_VITA_LUA_NATIVE_GETCLASS) serves it. */
#define LBR_GETCLASS() do { \
    GUEST_GPR_FLUSH(c); sub_003f8e30(c); GUEST_GPR_RELOAD(c); \
    lbr_observed(c, x, 0); } while (0)
/* The translated helpers 3f8820 / 3f8840 / 280cc0, mirrored the same way;
 * called with the translated call's publish/reload bracket. */
#define LBR_HELPER(fn) do { \
    GUEST_GPR_FLUSH(c); fn(c, x); GUEST_GPR_RELOAD(c); } while (0)
#define LBR_HELPER_ROW(row) do { \
    GUEST_GPR_FLUSH(c); \
    if ((row)->helper == ISAAC_LBR_HELPER_SIGNED_RVA) lbr_m_003f8820(c, x); \
    else lbr_m_003f8840(c, x); \
    GUEST_GPR_RELOAD(c); } while (0)
#define LBR_ROW_COVERAGE(row) do { \
    if ((row)->coverage != LBR_NO_COVERAGE) \
        guest_coverage_function((row)->coverage); } while (0)

/* The mirrors.  A family left out of ISAAC_VITA_LUA_BINDING_REPLAY_FAMILIES
 * leaves its mirror unreferenced. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#elif defined(_MSC_VER)
#pragma warning(push)
/* 4505: unreferenced mirror; 4189: GUEST_FLAGS_DECL's _fl, which guest.h
 * marks unused only for GCC/clang (guest.h:1190-1192). */
#pragma warning(disable : 4505 4189)
#endif
#include "host_vita_lua_binding_mirror.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(LBR_COND_BASE_GD == LBR_COND_BASE_TA + LBR_BRANCHES_TA &&
               LBR_COND_BASE_BI == LBR_COND_BASE_GD + LBR_BRANCHES_GD &&
               LBR_COND_BASE_IX == LBR_COND_BASE_BI + LBR_BRANCHES_BI &&
               LBR_COND_COUNT == LBR_COND_BASE_IX + LBR_BRANCHES_IX &&
               LBR_BRANCHES_H8820 == 0U && LBR_BRANCHES_H8840 == 0U &&
               LBR_BRANCHES_H0cc0 == 0U,
               "LBR_COND id layout");
#endif

/* ---- VERIFY (scalar getters with canBeConst = 1: const members) ---------- */

#if defined(ISAAC_VITA_LUA_BINDING_REPLAY_VERIFY) && LBR_HAS_SCALAR
#define LBR_VERIFY_FIRST 1000U
#define LBR_VERIFY_EVERY 4096U
static uint32_t s_verify_hits;

typedef struct lbr_snapshot {
    uint32_t reg[8];            /* eax ecx edx ebx esp ebp esi edi */
    uint32_t low_water;
    int top;
    uint8_t window[ISAAC_LBR_WINDOW_BELOW + 8U];
} lbr_snapshot;

static void lbr_regs_get(const CPU *c, uint32_t reg[8])
{
    reg[0] = c->eax; reg[1] = c->ecx; reg[2] = c->edx; reg[3] = c->ebx;
    reg[4] = c->esp; reg[5] = c->ebp; reg[6] = c->esi; reg[7] = c->edi;
}

static void lbr_regs_set(CPU *c, const uint32_t reg[8])
{
    c->eax = reg[0]; c->ecx = reg[1]; c->edx = reg[2]; c->ebx = reg[3];
    c->esp = reg[4]; c->ebp = reg[5]; c->esi = reg[6]; c->edi = reg[7];
}

static int lbr_verify_due(void)
{
    ++s_verify_hits;
    return s_verify_hits <= LBR_VERIFY_FIRST ||
           (s_verify_hits % LBR_VERIFY_EVERY) == 0U;
}

static void lbr_snapshot_take(const CPU *c, lbr_snapshot *s)
{
    uint32_t e = c->esp;
    lbr_regs_get(c, s->reg);
    s->low_water = c->stack_low_water;
    s->top = lua_gettop((lua_State *)(uintptr_t)ld32(e + 4U));
    memcpy(s->window, (const void *)(uintptr_t)(e - ISAAC_LBR_WINDOW_BELOW),
           sizeof s->window);
}

/* Live state after a getter: the eight registers, the Lua top and the value
 * the getter pushed (type and integer), the guest words from the final ESP
 * up to the entry frame's end E + 8.  The guest stack below the final ESP is
 * dead scratch and is not compared. */
typedef struct lbr_result {
    uint32_t reg[8];
    int top, type;
    lua_Integer value;
    uint32_t live_size;
    uint8_t live[ISAAC_LBR_WINDOW_BELOW + 8U];
} lbr_result;

static void lbr_result_take(const CPU *c, uint32_t e, lbr_result *r)
{
    lua_State *L = (lua_State *)(uintptr_t)ld32(e + 4U);
    lbr_regs_get(c, r->reg);
    r->top = lua_gettop(L);
    r->type = lua_type(L, -1);
    r->value = lua_tointegerx(L, -1, NULL);
    r->live_size = 0U;
    if (c->esp <= e + 8U && e + 8U - c->esp <= sizeof r->live) {
        r->live_size = e + 8U - c->esp;
        memcpy(r->live, (const void *)(uintptr_t)c->esp, r->live_size);
    }
}

static void lbr_verify_scalar(CPU *__restrict c, const lbr_row *row,
                              const lbr_snapshot *entry,
                              void (*real)(CPU *__restrict))
{
    uint32_t e = entry->reg[4];
    lua_State *L = (lua_State *)(uintptr_t)ld32(e + 4U);
    lbr_result m, t;
    int match;
    uint32_t mirror_low = c->stack_low_water;

    lbr_result_take(c, e, &m);
    /* Rewind: pop what the mirror pushed, restore the CPU registers, the
     * window bytes and the low-water mark, then run the translated body on
     * the same Lua state; its result is the one shipped. */
    lua_settop(L, entry->top);
    lbr_regs_set(c, entry->reg);
    c->stack_low_water = entry->low_water;
    memcpy((void *)(uintptr_t)(e - ISAAC_LBR_WINDOW_BELOW), entry->window,
           sizeof entry->window);
    real(c);
    /* A finalizer a GC step ran inside the mirror run went deeper than the
     * rerun (whose crossings find the step already spent); the translated
     * body alone would have noted that depth, so keep the deeper mark
     * (fixture run_verify_gc: without it the CPU differed by low only). */
    if (mirror_low < c->stack_low_water)
        c->stack_low_water = mirror_low;
    lbr_result_take(c, e, &t);
    match = memcmp(m.reg, t.reg, sizeof m.reg) == 0 && m.top == t.top &&
            m.type == t.type && m.value == t.value &&
            m.live_size == t.live_size &&
            memcmp(m.live, t.live, m.live_size) == 0;
    ++s_stats.verify_runs;
    if (!match)
        ++s_stats.verify_mismatches;
    if (!match || s_verify_hits <= 8U ||
        (s_verify_hits % LBR_VERIFY_EVERY) == 0U)
        isaac_vita_log(
            "[isaac-lua] binding replay VERIFY: row=%08x n=%u "
            "eax=%08x/%08x esp=%08x/%08x top=%d/%d type=%d/%d "
            "value=%lld/%lld live=%u/%u result=%s mismatches=%u",
            (unsigned)row->rva, (unsigned)s_verify_hits,
            (unsigned)m.reg[0], (unsigned)t.reg[0],
            (unsigned)m.reg[4], (unsigned)t.reg[4], m.top, t.top,
            m.type, t.type, (long long)m.value, (long long)t.value,
            (unsigned)m.live_size, (unsigned)t.live_size,
            match ? "MATCH" : "MISMATCH",
            (unsigned)s_stats.verify_mismatches);
}
#endif

/* ---- wraps ------------------------------------------------------------- */

#if ISAAC_VITA_LUA_BINDING_REPLAY_FAMILY_MASK & ISAAC_LBR_FAMILY_BIT_SCALAR
static void lbr_scalar(CPU *__restrict c, uint32_t index,
                       void (*real)(CPU *__restrict))
{
    const lbr_row *row = &s_rows[index];
    lbr_ctx x;
#if defined(ISAAC_VITA_LUA_BINDING_REPLAY_VERIFY)
    lbr_snapshot entry;
    int verify;
#endif

    if (!lbr_enter(c, &x, ISAAC_LBR_FAMILY_SCALAR, lbr_row_pins_ok(index))) {
        real(c);
        return;
    }
#if defined(ISAAC_VITA_LUA_BINDING_REPLAY_VERIFY)
    verify = row->cb != 0U && lbr_verify_due();
    if (verify)
        lbr_snapshot_take(c, &entry);
#endif
    lbr_m_template_a(c, &x, row);
    lbr_leave(&x, ISAAC_LBR_FAMILY_SCALAR);
#if defined(ISAAC_VITA_LUA_BINDING_REPLAY_VERIFY)
    if (verify)
        lbr_verify_scalar(c, row, &entry, real);
#endif
}

#define LBR_SCALAR_WRAP(sym, rva, cb, key, helper, cov) \
    void __real_##sym(CPU *__restrict c); \
    void __wrap_##sym(CPU *__restrict c); \
    void __wrap_##sym(CPU *__restrict c) \
    { \
        lbr_scalar(c, LBR_IDX_##sym, __real_##sym); \
    }
ISAAC_LBR_SCALAR_ROWS(LBR_SCALAR_WRAP)
#undef LBR_SCALAR_WRAP
#endif

#define LBR_SINGLE_WRAP(sym, family, mirror) \
    void __real_##sym(CPU *__restrict c); \
    void __wrap_##sym(CPU *__restrict c); \
    void __wrap_##sym(CPU *__restrict c) \
    { \
        lbr_ctx x; \
        if (!lbr_enter(c, &x, (family), lbr_single_pins_ok(family))) { \
            __real_##sym(c); \
            return; \
        } \
        mirror(c, &x); \
        lbr_leave(&x, (family)); \
    }
#if ISAAC_VITA_LUA_BINDING_REPLAY_FAMILY_MASK & ISAAC_LBR_FAMILY_BIT_GETDATA
LBR_SINGLE_WRAP(sub_00446d90, ISAAC_LBR_FAMILY_GETDATA, lbr_m_00446d90)
#endif
#if ISAAC_VITA_LUA_BINDING_REPLAY_FAMILY_MASK & ISAAC_LBR_FAMILY_BIT_BOOLII
LBR_SINGLE_WRAP(sub_00433870, ISAAC_LBR_FAMILY_BOOLII, lbr_m_00433870)
#endif
#if ISAAC_VITA_LUA_BINDING_REPLAY_FAMILY_MASK & ISAAC_LBR_FAMILY_BIT_INDEX2
LBR_SINGLE_WRAP(sub_003f90b0, ISAAC_LBR_FAMILY_INDEX2, lbr_m_003f90b0)
#endif
#undef LBR_SINGLE_WRAP
