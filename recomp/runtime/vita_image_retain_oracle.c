/* Host oracle for the ImageManager retention seam (host_vita_image_retain.c,
 * ISAAC_VITA_IMAGE_RETAIN).  Built ILP32 (clang -m32 / MSVC x86) so a plain
 * calloc arena is identity-mapped into the 32-bit guest address space that
 * guest.h's ld/st operate on.
 *
 * The arena holds a fake ImageManager (32 bucket vectors of {Image*, ctrl*}
 * behind the frozen bucket-table pointer), control blocks (frozen vtable word,
 * u16 strong/weak, an embedded lock object whose vtable's Enter/Leave slots
 * point at fake code addresses), Images of the three retained vtables and a
 * name arena.  The module is compiled unmodified with
 * ISAAC_VITA_IMAGE_RETAIN_ORACLE, so its production replays (gpush of the
 * observer's return words + guest_call) land in this file's guest_call, which
 * emulates Enter (thiscall ret 4), Leave (ret) and ImageManager::Unregister
 * (ret 4, erase + destroy + optional nested hook calls) and records every
 * event.  A reference observer reproduces sub_0056d8c0 instruction for
 * instruction on the same fake, so every passthrough scenario is checked to
 * produce the identical event sequence and identical arena, and every
 * retain/evict transition is checked to leave the fake manager consistent. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest.h"
#include "host_vita_image_retain.h"

#if UINTPTR_MAX != UINT32_MAX
#error the image retain oracle must be built ILP32 (identity-mapped guest addresses)
#endif

/* ------------------------------------------------------------- harness ---- */

static unsigned s_checks, s_fail, s_scenarios;
#define CHECK(cond) do { ++s_checks; if (!(cond)) { \
    fprintf(stderr, "CHECK failed %s:%d [%s]: %s\n", __FILE__, __LINE__, \
            s_scenario, #cond); ++s_fail; } } while (0)
static const char *s_scenario = "-";
static void scenario(const char *name) { s_scenario = name; ++s_scenarios; }

/* guest.h externs the module and the oracle must satisfy. */
static char s_last_log[1024];
static char s_last_receipt[1024];
static char s_last_mem[1024];
static unsigned s_log_calls, s_receipt_calls, s_mem_calls;
static size_t s_max_log_len;
static unsigned s_totals_seen;
#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN)
#define PIN_KEEP 32U
static char s_pin_log[PIN_KEEP][400];  /* "KAGE VITA IMAGE PRELOAD" lines since p_begin */
static unsigned s_pin_n, s_pin_totals_seen;
#endif
#if defined(ISAAC_VITA_IMAGE_RETAIN_BIG_TRACE)
#define BIG_KEEP 16U
static char s_big[BIG_KEEP][1024];     /* the first BIG_KEEP trace lines since big_reset() */
static unsigned s_big_n;               /* every trace line since big_reset() */
static void big_reset(void) { s_big_n = 0U; memset(s_big, 0, sizeof s_big); }
#endif
void isaac_vita_log(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vsnprintf(s_last_log, sizeof s_last_log, format, ap);
    va_end(ap);
    ++s_log_calls;
    if (strlen(s_last_log) > s_max_log_len)
        s_max_log_len = strlen(s_last_log);
    if (strncmp(s_last_log, "KAGE VITA IMAGE RETAIN: ", 24) == 0) {
        memcpy(s_last_receipt, s_last_log, sizeof s_last_receipt);
        ++s_receipt_calls;
    }
    if (strncmp(s_last_log, "KAGE VITA IMAGE PRELOAD: totals levels=4294967295 ", 50) == 0)
        ++s_totals_seen;
#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN)
    if (strncmp(s_last_log, "KAGE VITA IMAGE PRELOAD", 23) == 0) {
        char *slot = s_pin_log[s_pin_n % PIN_KEEP];
        memcpy(slot, s_last_log, sizeof s_pin_log[0] - 1U);
        slot[sizeof s_pin_log[0] - 1U] = '\0';
        ++s_pin_n;
    }
    if (strncmp(s_last_log, "KAGE VITA IMAGE PRELOAD PIN: totals mode=", 41) == 0 &&
            strstr(s_last_log, " pinned=4294967295 refused=4294967295 ") != NULL)
        ++s_pin_totals_seen;
#endif
    if (strncmp(s_last_log, "KAGE VITA IMAGE RETAIN MEM: ", 28) == 0) {
        memcpy(s_last_mem, s_last_log, sizeof s_last_mem);
        ++s_mem_calls;
    }
#if defined(ISAAC_VITA_IMAGE_RETAIN_BIG_TRACE)
    if (strncmp(s_last_log, "KAGE VITA IMAGE RETAIN BIG", 26) == 0) {
        if (s_big_n < BIG_KEEP)
            memcpy(s_big[s_big_n], s_last_log, sizeof s_big[0]);
        ++s_big_n;
    }
#endif
}

static unsigned s_fault_hit;
static char s_fault_what[128];
void guest_fault(CPU *__restrict c, uint32_t addr, const char *what)
{
    (void)c; (void)addr;
    ++s_fault_hit;
    snprintf(s_fault_what, sizeof s_fault_what, "%s", what ? what : "");
}

static unsigned s_stack_violations;
int guest_stack_violation(CPU *__restrict c, uint32_t pc, uint32_t kind,
                          uint32_t address, uint32_t size)
{ (void)c; (void)pc; (void)kind; (void)address; (void)size; ++s_stack_violations; return 0; }
int guest_stack_owner_violation(CPU *__restrict c, uint32_t pc)
{ (void)c; (void)pc; ++s_stack_violations; return 0; }

#if GUEST_STACK_REQUIRED
/* The production (GUEST_STACK_REQUIRED) build routes gpush/gpop through
 * guest.c's out-of-line validators.  Same bodies as guest.c / the mutex seam
 * oracle, so the module is exercised with the exact production stack
 * contract too (the test runner builds both variants). */
GUEST_STACK_HOT_NOINLINE int guest_stack_set(
    CPU *__restrict c, uint32_t value, uint32_t pc)
{
    uint32_t capacity;
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c)))
        return guest_stack_owner_violation(c, pc);
    capacity = c->stack_ceiling - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(value - c->stack_floor > capacity))
        return guest_stack_violation(
            c, pc, GUEST_STACK_FAULT_ADJUST, value, 0U);
    c->esp = value;
    guest_stack_note_low(c, value);
    return 1;
}

GUEST_STACK_HOT_NOINLINE int guest_stack_adjust(
    CPU *__restrict c, uint32_t amount, uint32_t pc)
{
    uint32_t capacity, offset;
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c)))
        return guest_stack_owner_violation(c, pc);
    capacity = c->stack_ceiling - c->stack_floor;
    offset = c->esp - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(offset > capacity || amount > capacity - offset))
        return guest_stack_violation(
            c, pc, GUEST_STACK_FAULT_ADJUST, c->esp, amount);
    c->esp += amount;
    return 1;
}

GUEST_STACK_HOT_NOINLINE uint32_t guest_stack_address(
    CPU *__restrict c, uint32_t address, uint32_t size, uint32_t pc)
{
    uint32_t capacity, offset;
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c))) {
        (void)guest_stack_owner_violation(c, pc);
        return 0U;
    }
    capacity = c->stack_ceiling - c->stack_floor;
    offset = address - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(
            size == 0U || size > capacity || offset > capacity - size)) {
        (void)guest_stack_violation(
            c, pc, GUEST_STACK_FAULT_ACCESS, address, size);
        return 0U;
    }
    guest_stack_note_low(c, address);
    return address;
}

GUEST_STACK_HOT_NOINLINE void gpush_at(
    CPU *__restrict c, uint32_t value, uint32_t pc)
{
    uint32_t capacity, offset, next;
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c))) {
        (void)guest_stack_owner_violation(c, pc);
        return;
    }
    capacity = c->stack_ceiling - c->stack_floor;
    offset = c->esp - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(offset < 4U || offset > capacity)) {
        (void)guest_stack_violation(
            c, pc, GUEST_STACK_FAULT_PUSH, c->esp - 4U, 4U);
        return;
    }
    next = c->esp - 4U;
    c->esp = next;
    guest_stack_note_low(c, next);
    st32(next, value);
}

GUEST_STACK_HOT_NOINLINE uint32_t gpop_at(
    CPU *__restrict c, uint32_t pc)
{
    uint32_t capacity, offset, value;
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c))) {
        (void)guest_stack_owner_violation(c, pc);
        return 0U;
    }
    capacity = c->stack_ceiling - c->stack_floor;
    offset = c->esp - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(capacity < 4U || offset > capacity - 4U)) {
        (void)guest_stack_violation(
            c, pc, GUEST_STACK_FAULT_POP, c->esp, 4U);
        return 0U;
    }
    value = ld32(c->esp);
    c->esp += 4U;
    return value;
}
#endif

/* ------------------------------------------------------------- arena ------ */

#define ARENA_BYTES   0x00810000U
#define OFF_CTRL      0x00080000U   /* control blocks, stride 0x20, <= 4096 */
#define OFF_IMG       0x00100000U   /* Images, stride 0x90, <= 4096 */
#define OFF_NAME      0x00200000U   /* name strings, stride 0x20 */
#define OFF_ENTRY     0x00300000U   /* 32 bucket vectors x 0x2000 B (1024 pairs) */
#define OFF_ENTRY2    0x00340000U   /* second storage generation (bucket growth) */
#define OFF_BUCKETS   0x00400000U   /* 32 x {begin,end,cap} */
#define OFF_LOCKVT    0x00410000U   /* the fake Mutex vtable (manager + ctrl locks) */
#define OFF_MGR_CS    0x00411000U   /* the manager Mutex's CRITICAL_SECTION (+0x18 held) */
#define OFF_PAIRS     0x00420000U   /* pair slots handed to the hook */
#define ENTRY_CAP     1024U
#define CTRL_STRIDE   0x20U
#define IMG_STRIDE    0x90U

/* Building against the HEAD-57fc40b module/header (the refutation run) --
 * those constants and env fields do not exist there. */
#ifndef ISAAC_IR_MUTEX_VT_RVA
#define IR_ORACLE_OLD_ABI 1
#define ISAAC_IR_MUTEX_ENTER_RVA 0x00562e00U
#define ISAAC_IR_MUTEX_LEAVE_RVA 0x00562ec0U
#define ISAAC_IR_MUTEX_INIT 0x04U
#define ISAAC_IR_MUTEX_CS   0x08U
#define ISAAC_IR_CS_HELD    0x18U
#define ISAAC_IR_UNREGISTER_SHIFT_SITE_RVA 0x0056d429U
#define ISAAC_IR_UNREGISTER_TAIL_SITE_RVA  0x0056d469U
#endif
/* The fake Mutex vtable's Enter/Leave slots hold the frozen RVAs relative to
 * the arena (the module pins them at arm); the oracle's guest_call dispatches
 * on those addresses, no code lives there. */
#define OFF_FAKE_ENTER ISAAC_IR_MUTEX_ENTER_RVA
#define OFF_FAKE_LEAVE ISAAC_IR_MUTEX_LEAVE_RVA

static uint8_t *s_arena;
static uint32_t s_base;
static uint32_t s_ctrl_n, s_img_n, s_name_n;

static uint32_t va(uint32_t off) { return s_base + off; }
static uint32_t bucket_of(uint32_t image) { return (image / IMG_STRIDE) & 31U; }
static uint32_t bucket_hdr(uint32_t b) { return va(OFF_BUCKETS) + b * 12U; }

/* Free-memory model (RAM + VRAM as vglMemFree would report them). */
static uint32_t s_free_ram = 160U << 20, s_free_vram = 40U << 20;
uint32_t isaac_ir_oracle_free_ram(void)  { return s_free_ram; }
uint32_t isaac_ir_oracle_free_vram(void) { return s_free_vram; }
#if defined(ISAAC_VITA_IMAGE_RETAIN_HEADROOM_PHYCONT)
static uint32_t s_free_phycont;
uint32_t isaac_ir_oracle_free_phycont(void) { return s_free_phycont; }
/* Every read advances 1 s unless frozen, so the reading cache never hits in
 * the shared scenarios and they see the free-memory model live. */
static uint64_t s_now_us;
static int s_clock_frozen;
uint64_t isaac_ir_oracle_now_us(void)
{
    if (!s_clock_frozen)
        s_now_us += 1000000U;
    return s_now_us;
}
#endif

/* ------------------------------------------------------------- events ----- */

enum ev_kind { EV_ENTER = 1, EV_LEAVE, EV_UNREG, EV_UNREG_MISS, EV_DESTROY,
               EV_MGR_ENTER, EV_MGR_LEAVE, EV_REAL_PROBE, EV_BAD_TARGET,
               EV_OBSERVER, EV_MGR_DEADLOCK };
typedef struct event { uint32_t kind, ret, a, b; } event;
static event s_events[4096];
static unsigned s_event_n;
static unsigned s_unknown_targets;

static void ev(uint32_t kind, uint32_t ret, uint32_t a, uint32_t b)
{
    if (s_event_n < sizeof s_events / sizeof s_events[0]) {
        s_events[s_event_n].kind = kind;
        s_events[s_event_n].ret = ret;
        s_events[s_event_n].a = a;
        s_events[s_event_n].b = b;
    }
    ++s_event_n;
}
static void ev_reset(void) { s_event_n = 0; }
static uint32_t first_unreg(void)
{
    unsigned i, n = s_event_n < sizeof s_events / sizeof s_events[0]
        ? s_event_n : (unsigned)(sizeof s_events / sizeof s_events[0]);
    for (i = 0; i < n; ++i)
        if (s_events[i].kind == EV_UNREG)
            return s_events[i].a;
    return 0U;
}
static unsigned ev_count(uint32_t kind)
{
    unsigned i, n = 0;
    for (i = 0; i < s_event_n && i < 4096; ++i)
        if (s_events[i].kind == kind) ++n;
    return n;
}

/* ------------------------------------------------------- fake manager ----- */

static uint32_t img_bytes(uint32_t img)
{
    return (uint32_t)ld16(img + ISAAC_IR_IMG_PW) * (uint32_t)ld16(img + ISAAC_IR_IMG_PH) *
           ld32(img + ISAAC_IR_IMG_BPP);
}

#if !defined(IR_ORACLE_OLD_ABI)
static void fv_reset(void);
#endif
static void manager_reset(void)
{
    uint32_t i;
    memset(s_arena, 0, ARENA_BYTES);
    st32(va(ISAAC_IR_HOOK_GLOBAL_RVA), va(ISAAC_IR_HOOK_RVA));   /* observer installed */
    st32(va(ISAAC_IR_HOOK_GLOBAL2_RVA), va(ISAAC_IR_HOOK_RVA));
    st32(va(ISAAC_IR_BUCKETS_PTR_RVA), va(OFF_BUCKETS));
    for (i = 0; i < 32U; ++i) {
        uint32_t vec = va(OFF_ENTRY) + i * 0x2000U;
        st32(bucket_hdr(i) + 0U, vec);
        st32(bucket_hdr(i) + 4U, vec);
        st32(bucket_hdr(i) + 8U, vec + ENTRY_CAP * 8U);
    }
    st32(va(OFF_LOCKVT) + ISAAC_IR_LOCK_VT_ENTER, va(OFF_FAKE_ENTER));
    st32(va(OFF_LOCKVT) + ISAAC_IR_LOCK_VT_LEAVE, va(OFF_FAKE_LEAVE));
    /* The manager Mutex object {vt, u8 init, CS*} with its CS's held byte. */
    st32(va(ISAAC_IR_MANAGER_LOCK_RVA), va(OFF_LOCKVT));
    st8(va(ISAAC_IR_MANAGER_LOCK_RVA) + ISAAC_IR_MUTEX_INIT, 1U);
    st32(va(ISAAC_IR_MANAGER_LOCK_RVA) + ISAAC_IR_MUTEX_CS, va(OFF_MGR_CS));
    st8(va(OFF_MGR_CS) + ISAAC_IR_CS_HELD, 0U);
    s_ctrl_n = s_img_n = s_name_n = 0U;
#if !defined(IR_ORACLE_OLD_ABI)
    fv_reset();
#endif
    s_fault_hit = 0; s_fault_what[0] = 0;
    s_unknown_targets = 0;
    ev_reset();
}

/* The manager Mutex exactly as sub_00562e00/sub_00562ec0 behave on one
 * thread: Enter with the held byte already set never returns (recorded as a
 * deadlock instead of spinning), Leave clears it. */
static unsigned s_deadlock;
static int mgr_held(void) { return ld8(va(OFF_MGR_CS) + ISAAC_IR_CS_HELD) != 0U; }
static int mgr_enter(uint32_t ret)
{
    if (mgr_held()) {
        ++s_deadlock;
        ev(EV_MGR_DEADLOCK, ret, 0, 0);
        return 0;
    }
    st8(va(OFF_MGR_CS) + ISAAC_IR_CS_HELD, 1U);
    ev(EV_MGR_ENTER, ret, 0, 0);
    return 1;
}
static void mgr_leave(uint32_t ret)
{
    st8(va(OFF_MGR_CS) + ISAAC_IR_CS_HELD, 0U);
    ev(EV_MGR_LEAVE, ret, 0, 0);
}

static uint32_t new_ctrl(uint16_t strong)
{
    uint32_t ctrl = va(OFF_CTRL) + s_ctrl_n++ * CTRL_STRIDE;
    st32(ctrl, va(ISAAC_IR_CTRL_VT_RVA));               /* frozen ctrl vtable */
    st16(ctrl + ISAAC_IR_CTRL_STRONG, strong);
    st16(ctrl + 6U, 1U);                                 /* weak */
    st32(ctrl + ISAAC_IR_CTRL_LOCK, va(OFF_LOCKVT));     /* lock object vtable */
    st32(ctrl + 0xcU, 0U);                               /* lock depth */
    return ctrl;
}

static uint32_t new_name(const char *s)
{
    uint32_t p = va(OFF_NAME) + s_name_n++ * 0x20U;
    size_t i;
    for (i = 0; s[i] && i < 0x1fU; ++i)
        st8(p + (uint32_t)i, (uint8_t)s[i]);
    st8(p + (uint32_t)i, 0U);
    return p;
}

/* An Image of vtable `vt` after a completed upload (glGenTextures gave two
 * names; the base ctor had set tex0 = 0xffffffff). */
static uint32_t new_image(uint32_t vt, const char *name, uint32_t pw,
                          uint32_t ph, uint32_t bpp, uint32_t flags)
{
    uint32_t img = va(OFF_IMG) + s_img_n++ * IMG_STRIDE;
    st32(img + 0x00U, vt);
    st32(img + ISAAC_IR_IMG_FLAGS, flags);
    st32(img + ISAAC_IR_IMG_NAME, name ? new_name(name) : 0U);
    st32(img + ISAAC_IR_IMG_TEX0, 0x1000U + s_img_n);
    st32(img + ISAAC_IR_IMG_TEX1, 0x2000U + s_img_n);
    st16(img + ISAAC_IR_IMG_PW, (uint16_t)pw);
    st16(img + ISAAC_IR_IMG_PH, (uint16_t)ph);
    st32(img + ISAAC_IR_IMG_BPP, bpp);
    return img;
}

/* Register: strong copy of {image, ctrl} into the image's bucket. */
static void manager_register(uint32_t image, uint32_t ctrl)
{
    uint32_t b = bucket_of(image);
    uint32_t end = ld32(bucket_hdr(b) + 4U);
    if (end >= ld32(bucket_hdr(b) + 8U)) { fprintf(stderr, "bucket %u full\n", b); exit(2); }
    st32(end + 0U, image);
    st32(end + 4U, ctrl);
    st32(bucket_hdr(b) + 4U, end + 8U);
    st32(ctrl + 0x14U, image);                           /* owner */
}

static int manager_present(uint32_t image)
{
    uint32_t b = bucket_of(image);
    uint32_t p = ld32(bucket_hdr(b)), end = ld32(bucket_hdr(b) + 4U);
    for (; p < end; p += 8U)
        if (ld32(p) == image)
            return 1;
    return 0;
}

static uint32_t manager_count(void)
{
    uint32_t b, n = 0;
    for (b = 0; b < 32U; ++b)
        n += (ld32(bucket_hdr(b) + 4U) - ld32(bucket_hdr(b))) / 8U;
    return n;
}

#if !defined(IR_ORACLE_OLD_ABI)
#define IR_ORACLE_FLUSH 1
/* ------------------------------------------------ fake batch renderer ----- */
/* The three vectors ImageManager::Unregister scrubs before it erases the
 * bucket entry (PE 0x56d280 DIRTY, 0x56d2c1 TOUCHED, 0x56d2f6 PAIRS) and the
 * last-pair Image global, at their frozen RVAs.  Storage lives in the arena.
 * A "walker" models a flush/sweep loop of the game that is on the stack while
 * a release runs: the OFF build shrinks these vectors under such a loop only
 * for the Image being released, so any other shrink is a violation. */
#define OFF_FV_DIRTY   0x00430000U
#define OFF_FV_TOUCHED 0x00438000U
#define OFF_FV_PAIRS   0x00440000U
#define FV_DIRTY   ISAAC_IR_DIRTY_BEGIN_RVA, ISAAC_IR_DIRTY_END_RVA
#define FV_TOUCHED ISAAC_IR_TOUCHED_BEGIN_RVA, ISAAC_IR_TOUCHED_END_RVA
static unsigned s_walker_active;
static uint32_t s_walker_image;
static unsigned s_walker_violations;
static unsigned s_vec_erases;         /* Unregister calls that shrank a vector */

static void fv_reset(void)
{
    st32(va(ISAAC_IR_DIRTY_BEGIN_RVA), va(OFF_FV_DIRTY));
    st32(va(ISAAC_IR_DIRTY_END_RVA), va(OFF_FV_DIRTY));
    st32(va(ISAAC_IR_TOUCHED_BEGIN_RVA), va(OFF_FV_TOUCHED));
    st32(va(ISAAC_IR_TOUCHED_END_RVA), va(OFF_FV_TOUCHED));
    st32(va(ISAAC_IR_PAIRS_BEGIN_RVA), va(OFF_FV_PAIRS));
    st32(va(ISAAC_IR_PAIRS_END_RVA), va(OFF_FV_PAIRS));
    st32(va(ISAAC_IR_LAST_PAIR_IMAGE_RVA), 0U);
    s_walker_active = 0; s_walker_image = 0U; s_walker_violations = 0; s_vec_erases = 0;
}
static void fv_push(uint32_t begin_rva, uint32_t end_rva, uint32_t image)
{
    uint32_t end = ld32(va(end_rva));
    (void)begin_rva;
    st32(end, image);
    st32(va(end_rva), end + 4U);
}
static unsigned fv_count(uint32_t begin_rva, uint32_t end_rva, uint32_t image)
{
    uint32_t p = ld32(va(begin_rva)), end = ld32(va(end_rva));
    unsigned n = 0;
    for (; p < end; p += 4U)
        if (ld32(p) == image) ++n;
    return n;
}
/* Erase the first occurrence the way the PE does (memmove the tail down,
 * `sub [end],4`).  Returns 1 when the vector shrank. */
static int fv_erase(uint32_t begin_rva, uint32_t end_rva, uint32_t image)
{
    uint32_t p = ld32(va(begin_rva)), end = ld32(va(end_rva));
    for (; p < end; p += 4U) {
        if (ld32(p) == image) {
            for (; p + 4U < end; p += 4U)
                st32(p, ld32(p + 4U));
            st32(va(end_rva), end - 4U);
            return 1;
        }
    }
    return 0;
}
/* A flush drew the Image (0x56d5cf): bit 0x10 + TOUCHED membership. */
static void touched_add(uint32_t image)
{
    st32(image + ISAAC_IR_IMG_FLAGS, ld32(image + ISAAC_IR_IMG_FLAGS) | ISAAC_IR_IMG_FLAG_TOUCHED);
    fv_push(FV_TOUCHED, image);
}
/* A quad was queued (0x5601f6): bit 0x04 + DIRTY membership. */
static void dirty_add(uint32_t image)
{
    st32(image + ISAAC_IR_IMG_FLAGS, ld32(image + ISAAC_IR_IMG_FLAGS) | ISAAC_IR_IMG_FLAG_DIRTY);
    fv_push(FV_DIRTY, image);
}
/* A {batch, Image} pair was appended (0x5608f6 also latches the Image). */
static void pair_add(uint32_t batch, uint32_t image)
{
    uint32_t end = ld32(va(ISAAC_IR_PAIRS_END_RVA));
    st32(end, batch);
    st32(end + 4U, image);
    st32(va(ISAAC_IR_PAIRS_END_RVA), end + 8U);
    st32(va(ISAAC_IR_LAST_PAIR_IMAGE_RVA), image);
}
/* The game's idle sweep (0x561f81 / 0x564fec): bit off, entry erased. */
__attribute__((unused)) static void fake_sweep(uint32_t image)
{
    st32(image + ISAAC_IR_IMG_FLAGS, ld32(image + ISAAC_IR_IMG_FLAGS) & ~ISAAC_IR_IMG_FLAG_TOUCHED);
    (void)fv_erase(FV_TOUCHED, image);
}
/* Unregister's scrub of the three vectors for `image`. */
static void unregister_flush_lists(uint32_t image)
{
    uint32_t p, q, end;
    int shrunk = 0;
    shrunk |= fv_erase(FV_DIRTY, image);
    shrunk |= fv_erase(FV_TOUCHED, image);
    p = q = ld32(va(ISAAC_IR_PAIRS_BEGIN_RVA));
    end = ld32(va(ISAAC_IR_PAIRS_END_RVA));
    for (; p < end; p += 8U) {
        if (ld32(p + 4U) == image) { shrunk = 1; continue; }
        st32(q, ld32(p)); st32(q + 4U, ld32(p + 4U));
        q += 8U;
    }
    if (q < end)
        st32(va(ISAAC_IR_PAIRS_END_RVA), q);
    if (shrunk) {
        ++s_vec_erases;
        if (s_walker_active && image != s_walker_image)
            ++s_walker_violations;
    }
}
#endif

/* Nested hook offers fired from inside a destroy (Image dtor releasing
 * sub-objects) and the esp-misbehaviour switch for the contract test. */
static uint32_t s_nested_image, s_nested_ctrl;
static int s_unreg_break_esp;
/* vitaGL 73dd57a defers the free of a texture drawn in the last
 * FRAME_PURGE_FREQ frames to its purge list: while set, a destroy returns no
 * bytes to the free-memory model (the headroom-pressure scenarios). */
static int s_deferred_free;
static int s_enter_break_esp, s_leave_break_esp;
/* While set, the fake Enter also raises the control block's strong count:
 * another thread's TryAddRef landing between the wrapper's unlocked
 * pre-check and its lock replay (scenario 19). */
static int s_race_bump;
/* What the guest's `mov eax,[0x7fd62c]; call eax` reaches: the wrapper under
 * test (production) or the reference observer (reference runs). */
static void (*s_observer)(CPU *);

/* A generated observer call site: push &pair, push the site's return word,
 * call the observer, add esp,4. */
static void observer_call(CPU *c, uint32_t pair, uint32_t ret)
{
    ev(EV_OBSERVER, ret, ld32(pair), ld32(pair + 4U));
    if (!s_observer)
        return;
    gpush(c, pair);
    gpush(c, ret);
    s_observer(c);
    c->esp += 4U;
}

static void destroy_image(CPU *c, uint32_t image, uint32_t ctrl);

/* SmartPointer<Image>::Release() as the erase loop and _Destroy_range use
 * it: strong-- ; 0 -> destroy the owner (no observer) ; else the observer is
 * called with pair = the SmartPointer just released. */
static void release_pair(CPU *c, uint32_t pair, uint32_t ret)
{
    uint32_t image = ld32(pair), ctrl = ld32(pair + 4U);
    uint16_t strong;
    if (ctrl == 0U)
        return;
    strong = ld16(ctrl + ISAAC_IR_CTRL_STRONG);
    if (strong == 0U)
        return;
    --strong;
    st16(ctrl + ISAAC_IR_CTRL_STRONG, strong);
    if (strong == 0U)
        destroy_image(c, image, ctrl);
    else
        observer_call(c, pair, ret);
}

/* SmartPointer<Image> copy (sub_000078a0, TryAddRef): strong++. */
static void copy_pair(uint32_t dst, uint32_t src)
{
    uint32_t ctrl = ld32(src + 4U);
    st32(dst, ld32(src));
    st32(dst + 4U, ctrl);
    if (ctrl)
        st16(ctrl + ISAAC_IR_CTRL_STRONG, (uint16_t)(ld16(ctrl + ISAAC_IR_CTRL_STRONG) + 1U));
}

/* The destroy step of strong -> 0: poison the Image, return its bytes to the
 * free-memory model, fire the nested offers. */
static void destroy_image(CPU *c, uint32_t image, uint32_t ctrl)
{
    uint32_t bytes = img_bytes(image);
    ev(EV_DESTROY, 0, image, ctrl);
    if (!s_deferred_free)
        s_free_ram += bytes;
    st32(image, 0xdead0000U);                            /* dtor ran: vtable gone */
    st32(ctrl + 0x14U, 0U);
    if (s_nested_image) {
        uint32_t ni = s_nested_image, nc = s_nested_ctrl;
        uint32_t slot = va(OFF_PAIRS) + 16U;
        s_nested_image = 0U;
        st32(slot, ni); st32(slot + 4U, nc);
        observer_call(c, slot, 0x00002987U);             /* a generated return word */
    }
}

/* ImageManager::Unregister(image) = sub_0056d220 as the PE does it: manager
 * Mutex Enter; djb2 bucket; find the entry; erase by the 0x56d3e0 loop --
 * for every entry behind the hole: temp = copy(entry) (TryAddRef), swap temp
 * with the previous slot, Release(temp) -> observer(&temp) at 0x56d429 when
 * strong stays >= 1 -- then the 0x56d441 tail: Release(last entry) ->
 * observer(&last) at 0x56d469, end -= 8; Leave.  `temp` lives in Unregister's
 * own stack frame like [ebp-0x20] does. */
static void do_unregister(CPU *c, uint32_t ret, uint32_t image)
{
    uint32_t b = bucket_of(image);
    uint32_t begin, end, p;
    if (!mgr_enter(ret))
        return;                                          /* would spin forever */
#if defined(IR_ORACLE_FLUSH)
    unregister_flush_lists(image);                       /* 0x56d280..0x56d33x */
#endif
    begin = ld32(bucket_hdr(b)); end = ld32(bucket_hdr(b) + 4U);
    for (p = begin; p < end; p += 8U) {
        if (ld32(p) == image) {
            uint32_t q, temp;
            c->esp -= 8U;                                /* Unregister's frame: temp */
            temp = c->esp;
            for (q = p + 8U; q < end; q += 8U) {
                uint32_t prev_i = ld32(q - 8U), prev_c = ld32(q - 4U);
                copy_pair(temp, q);                      /* call 0x78a0 */
                st32(q - 8U, ld32(temp)); st32(q - 4U, ld32(temp + 4U)); /* swap */
                st32(temp, prev_i); st32(temp + 4U, prev_c);
                release_pair(c, temp, ISAAC_IR_UNREGISTER_SHIFT_SITE_RVA);
            }
            c->esp += 8U;
            end = ld32(bucket_hdr(b) + 4U);              /* 0x56d43e: reloaded */
            release_pair(c, end - 8U, ISAAC_IR_UNREGISTER_TAIL_SITE_RVA);
            st32(bucket_hdr(b) + 4U, end - 8U);          /* 0x56d473: end -= 8 */
            mgr_leave(ret);
            return;
        }
    }
    ev(EV_UNREG_MISS, ret, image, 0);                    /* not found: no-op */
    mgr_leave(ret);
}

/* Bucket growth (sub_0056d9b0 under Load/Register's manager lock): copy every
 * entry to a fresh storage generation (TryAddRef each), destroy the old
 * entries in place (Release -> observer for every one that keeps strong >= 1,
 * sub_0056daf0 at 0x56db43) while the header still points at the old storage,
 * then publish the new begin/end/cap (sub_0056db70). */
static void manager_grow(CPU *c, uint32_t b, uint32_t ret)
{
    uint32_t begin = ld32(bucket_hdr(b)), end = ld32(bucket_hdr(b) + 4U);
    uint32_t n = (end - begin) / 8U, i;
    uint32_t fresh = (begin >= va(OFF_ENTRY2)) ? va(OFF_ENTRY) + b * 0x2000U
                                               : va(OFF_ENTRY2) + b * 0x2000U;
    if (!mgr_enter(ret))
        return;
    for (i = 0; i < n; ++i)
        copy_pair(fresh + i * 8U, begin + i * 8U);
    for (i = 0; i < n; ++i)
        release_pair(c, begin + i * 8U, 0x0056db45U);
    st32(bucket_hdr(b) + 0U, fresh);
    st32(bucket_hdr(b) + 4U, fresh + n * 8U);
    st32(bucket_hdr(b) + 8U, fresh + ENTRY_CAP * 8U);
    mgr_leave(ret);
}

#if defined(ISAAC_VITA_IMAGE_PRELOAD)
/* ------------------------------------------------- fake Load / Release ---- */
/* What the preload replays: ImageManager::Load (sub_0056c7e0, `ret 0x10`,
 * called as at 0x565fad), the control block's Release (vtable +0xc, 0x7af0,
 * thiscall, plain ret, al = still referenced) and the observer through the
 * hook global. */
#define OFF_FAKE_RELEASE 0x00007af0U
static uint32_t s_load_flags, s_load_pw, s_load_ph;
static int s_load_fail, s_load_orphan, s_load_break_esp, s_release_break_esp, s_hook_break_esp;
static unsigned s_load_calls, s_load_bad_args, s_release_calls, s_hook_calls;
static uint32_t s_heap_hr;
static int s_heap_terminal;
static uint64_t s_irp_now;
uint64_t isaac_irp_oracle_now_us(void) { s_irp_now += 1000U; return s_irp_now; }
uint32_t isaac_vita_image_preload_heap_headroom(void) { return s_heap_hr; }
int isaac_vita_guest_heap_terminal(void) { return s_heap_terminal; }

static int guest_str_eq(uint32_t a, const char *b)
{
    uint32_t i;
    if (a == 0U)
        return 0;
    for (i = 0; i < 0x20U; ++i) {
        if (ld8(a + i) != (uint8_t)b[i]) return 0;
        if (b[i] == '\0') return 1;
    }
    return 0;
}

static void fake_load(CPU *c)
{
    uint32_t ret = gpop(c), out = gpop(c), name = gpop(c), desc = gpop(c), flag = gpop(c);
    uint32_t b, p, image = 0U, ctrl = 0U;
    char key[0x20];
    size_t n = 0;

    ++s_load_calls;
    if (ret != ISAAC_IR_LOAD_SITE_RVA || desc != va(ISAAC_IR_LOAD_VERTEX_DESC_RVA) ||
            flag != 0U || ld32(out) != 0U || ld32(out + 4U) != 0U)
        ++s_load_bad_args;
    memcpy(key, "resources/", 10); n = 10;
    while (n < sizeof key - 1U && ld8(name + (uint32_t)(n - 10U)) != 0U) {
        key[n] = (char)ld8(name + (uint32_t)(n - 10U));
        ++n;
    }
    key[n] = '\0';
    if (!mgr_enter(ret))
        return;
    for (b = 0; b < 32U && image == 0U; ++b) {
        for (p = ld32(bucket_hdr(b)); p < ld32(bucket_hdr(b) + 4U); p += 8U) {
            if (guest_str_eq(ld32(ld32(p) + ISAAC_IR_IMG_NAME), key)) {
                image = ld32(p); ctrl = ld32(p + 4U);
                st16(ctrl + ISAAC_IR_CTRL_STRONG, (uint16_t)(ld16(ctrl + ISAAC_IR_CTRL_STRONG) + 1U));
                break;
            }
        }
    }
    if (image == 0U && !s_load_fail) {
        image = new_image(va(ISAAC_IR_VT_PNG_RVA), key, s_load_pw, s_load_ph, 4U, s_load_flags);
        ctrl = new_ctrl(s_load_orphan ? 1U : 2U);        /* the manager's + `out` */
        if (s_load_orphan)
            st32(ctrl + 0x14U, image);                   /* `out` is the only owner */
        else
            manager_register(image, ctrl);
        s_free_ram -= img_bytes(image);
    }
    mgr_leave(ret);
    st32(out, image);
    st32(out + 4U, ctrl);
    c->eax = out;
    if (s_load_break_esp)
        c->esp -= 4U;                                    /* violate `ret 0x10` */
}

static void fake_release(CPU *c)
{
    uint32_t ret = gpop(c), ctrl = c->ecx;
    uint16_t strong = ld16(ctrl + ISAAC_IR_CTRL_STRONG);

    ++s_release_calls;
    if (ret != ISAAC_IR_LOAD_RELEASE_SITE_RVA || strong == 0U)
        ++s_load_bad_args;
    --strong;
    st16(ctrl + ISAAC_IR_CTRL_STRONG, strong);
    if (strong == 0U) {
        destroy_image(c, ld32(ctrl + 0x14U), ctrl);
        c->eax = 0xabcdef00U;                            /* al = 0, garbage above */
    } else {
        c->eax = 0xabcdef01U;                            /* al = 1 */
    }
    if (s_release_break_esp)
        c->esp -= 4U;                                    /* violate `ret` */
}
#endif

/* The oracle's guest_call: dispatches the module's (and the reference
 * observer's) replays by target address and emulates the x86 callee
 * conventions (return word + argument cleanup). */
void guest_call(CPU *__restrict c, uint32_t target)
{
    uint32_t ret;
    if (target == va(OFF_FAKE_ENTER)) {                  /* thiscall Enter(-1), ret 4 */
        uint32_t arg;
        ret = gpop(c);
        arg = gpop(c);
        ev(EV_ENTER, ret, c->ecx, arg);
        st32(c->ecx + 4U, ld32(c->ecx + 4U) + 1U);
        if (s_race_bump) {
            uint32_t ctrl = c->ecx - ISAAC_IR_CTRL_LOCK;
            st16(ctrl + ISAAC_IR_CTRL_STRONG,
                 (uint16_t)(ld16(ctrl + ISAAC_IR_CTRL_STRONG) + 1U));
            s_race_bump = 0;
        }
        c->eax = 1U;
        if (s_enter_break_esp)
            c->esp -= 4U;                                /* violate `ret 4` */
    } else if (target == va(OFF_FAKE_LEAVE)) {           /* thiscall Leave(), ret */
        ret = gpop(c);
        ev(EV_LEAVE, ret, c->ecx, 0);
        st32(c->ecx + 4U, ld32(c->ecx + 4U) - 1U);
        c->eax = 0U;
        if (s_leave_break_esp)
            c->esp += 4U;                                /* violate `ret` */
    } else if (target == va(ISAAC_IR_UNREGISTER_RVA)) {  /* cdecl-arg, ret 4 */
        uint32_t image;
        ret = gpop(c);
        image = gpop(c);
        ev(EV_UNREG, ret, image, 0);
        do_unregister(c, ret, image);
        if (s_unreg_break_esp)
            c->esp -= 4U;                                /* violate `ret 4` */
#if defined(ISAAC_VITA_IMAGE_PRELOAD)
    } else if (target == va(ISAAC_IR_HOOK_RVA)) {        /* cdecl observer; it pops its own ret */
        uint32_t pair = ld32(c->esp + 4U);
        ++s_hook_calls;
        ev(EV_OBSERVER, ld32(c->esp), ld32(pair), ld32(pair + 4U));
        if (s_observer)
            s_observer(c);
        else
            (void)gpop(c);
        if (s_hook_break_esp)
            c->esp += 4U;                                /* violate cdecl */
    } else if (target == va(ISAAC_IR_LOAD_RVA)) {
        fake_load(c);
    } else if (target == va(OFF_FAKE_RELEASE)) {
        fake_release(c);
#endif
    } else {
        ++s_unknown_targets;
        ev(EV_BAD_TARGET, 0, target, 0);
    }
}

/* ------------------------------------------------------------ CPU / frame - */

static CPU s_cpu, s_cpu_foreign;
static uint8_t *s_stack;
#define STACK_BYTES 65536U

static void cpu_bind(CPU *c)
{
    uint32_t base = (uint32_t)(uintptr_t)s_stack;
    memset(c, 0, sizeof *c);
    c->stack_owner = c;
    c->stack_floor = base;
    c->stack_ceiling = base + STACK_BYTES;
    c->esp = base + STACK_BYTES / 2U;
    c->stack_low_water = c->esp;
}

/* Build the observer's cdecl frame: [esp] = return word, [esp+4] = pair*. */
static uint32_t s_pair_slot;
static void set_frame(CPU *c, uint32_t pair)
{
    cpu_bind(c);
    c->esp -= 8U;
    st32(c->esp + 0U, 0x00002987U);
    st32(c->esp + 4U, pair);
    c->stack_low_water = c->esp;
}
static uint32_t make_pair(uint32_t image, uint32_t ctrl)
{
    st32(s_pair_slot + 0U, image);
    st32(s_pair_slot + 4U, ctrl);
    return s_pair_slot;
}

/* The reference observer: sub_0056d8c0 instruction for instruction. */
static void ref_observer(CPU *c)
{
    uint32_t ebx = ld32(c->esp + 4U);                    /* mov ebx,[ebp+8] */
    uint32_t esi = ld32(ebx + 4U);                       /* mov esi,[ebx+4] */
    if (esi != 0U) {
        uint32_t eax = ld32(esi + 8U);                   /* mov eax,[esi+8] */
        uint32_t edi = esi + 8U;                         /* lea edi,[esi+8] */
        uint32_t n;
        gpush(c, 0xFFFFFFFFU);                           /* push -1 */
        c->ecx = edi;
        gpush(c, ISAAC_IR_ENTER_SITE_RVA);
        guest_call(c, ld32(eax + 0xcU));                 /* call [eax+0xc] */
        eax = ld32(edi);                                 /* mov eax,[edi] */
        c->ecx = edi;
        n = ld16(esi + 4U);                              /* movzx esi,word [esi+4] */
        gpush(c, ISAAC_IR_LEAVE_SITE_RVA);
        guest_call(c, ld32(eax + 0x10U));                /* call [eax+0x10] */
        if (n == 1U) {
            gpush(c, ld32(ebx));                         /* push [ebx] */
            gpush(c, ISAAC_IR_UNREGISTER_SITE_RVA);
            guest_call(c, va(ISAAC_IR_UNREGISTER_RVA));  /* call 0x56d220 */
        }
    }
    (void)gpop(c);                                       /* ret */
}

/* A "real" that only records being reached (for frames the real body would
 * fault on; the wrapper must have touched nothing before routing). */
static void real_probe(CPU *c)
{
    ev(EV_REAL_PROBE, 0, c->esp, 0);
    (void)gpop(c);
}

static void wrapper_entry(CPU *c) { isaac_ir_oracle_wrap(c, ref_observer); }

/* -------------------------------------------------------------- snapshots - */

typedef struct snap {
    uint8_t *arena; uint8_t *stack; CPU cpu;
    uint32_t free_ram, free_vram;
} snap;

static void snap_take(snap *s, const CPU *c)
{
    if (!s->arena) s->arena = (uint8_t *)malloc(ARENA_BYTES);
    if (!s->stack) s->stack = (uint8_t *)malloc(STACK_BYTES);
    memcpy(s->arena, s_arena, ARENA_BYTES);
    memcpy(s->stack, s_stack, STACK_BYTES);
    s->cpu = *c;
    s->free_ram = s_free_ram; s->free_vram = s_free_vram;
}
static void snap_restore(const snap *s, CPU *c)
{
    memcpy(s_arena, s->arena, ARENA_BYTES);
    memcpy(s_stack, s->stack, STACK_BYTES);
    *c = s->cpu;
    s_free_ram = s->free_ram; s_free_vram = s->free_vram;
}
static int snap_equal_arena(const snap *s)
{
    return memcmp(s->arena, s_arena, ARENA_BYTES) == 0;
}

static event s_log_a[4096];
static unsigned s_log_a_n;

/* Run the wrapper on a frame and then the reference observer on the same
 * initial state; assert identical event sequences, identical arena and
 * identical ESP.  Used for every passthrough scenario. */
static void assert_passthrough_identical(uint32_t pair)
{
    static snap before, after_wrapper;
    uint32_t esp_wrapper, esp_ref;
    unsigned i;

    set_frame(&s_cpu, pair);
    snap_take(&before, &s_cpu);
    ev_reset();
    s_observer = wrapper_entry;
    isaac_ir_oracle_wrap(&s_cpu, ref_observer);
    esp_wrapper = s_cpu.esp;
    s_log_a_n = s_event_n;
    memcpy(s_log_a, s_events, sizeof s_events);
    snap_take(&after_wrapper, &s_cpu);

    snap_restore(&before, &s_cpu);
    ev_reset();
    s_observer = ref_observer;                           /* nested sites too */
    ref_observer(&s_cpu);
    s_observer = wrapper_entry;
    esp_ref = s_cpu.esp;

    CHECK(s_log_a_n == s_event_n);
    for (i = 0; i < s_log_a_n && i < s_event_n && i < 4096; ++i) {
        CHECK(s_log_a[i].kind == s_events[i].kind &&
              s_log_a[i].ret == s_events[i].ret &&
              s_log_a[i].a == s_events[i].a &&
              s_log_a[i].b == s_events[i].b);
    }
    CHECK(esp_wrapper == esp_ref);
    CHECK(snap_equal_arena(&after_wrapper));
    /* leave the state as the wrapper left it */
    snap_restore(&after_wrapper, &s_cpu);
}

/* -------------------------------------------------------------- scenarios - */

static const isaac_ir_stats *st(void) { return isaac_ir_oracle_stats(); }

static void begin(const char *name, uint32_t budget_mb, uint32_t headroom_mb)
{
    isaac_ir_env env;
    scenario(name);
    manager_reset();
    env.image_base = s_base;
    env.buckets_ptr_va = va(ISAAC_IR_BUCKETS_PTR_RVA);
    env.hook_global_va = va(ISAAC_IR_HOOK_GLOBAL_RVA);
    env.hook_global2_va = va(ISAAC_IR_HOOK_GLOBAL2_RVA);
    env.vt_png = va(ISAAC_IR_VT_PNG_RVA);
    env.vt_pcx = va(ISAAC_IR_VT_PCX_RVA);
    env.vt_pic = va(ISAAC_IR_VT_PIC_RVA);
    env.vt_ctrl = va(ISAAC_IR_CTRL_VT_RVA);
#if !defined(IR_ORACLE_OLD_ABI)
    env.vt_mutex = va(OFF_LOCKVT);
    env.manager_lock_va = va(ISAAC_IR_MANAGER_LOCK_RVA);
#endif
    isaac_vita_image_retain_disarm();
    isaac_ir_oracle_set_env(&env);
    isaac_ir_oracle_set_budget(budget_mb << 20, headroom_mb << 20);
    s_free_ram = 160U << 20; s_free_vram = 40U << 20;
#if defined(ISAAC_VITA_IMAGE_RETAIN_HEADROOM_PHYCONT)
    s_free_phycont = 0U;
    s_clock_frozen = 0;
#endif
    s_nested_image = 0U; s_unreg_break_esp = 0;
    s_observer = wrapper_entry;
    s_deadlock = 0;
    s_deferred_free = 0; s_enter_break_esp = 0; s_leave_break_esp = 0;
    isaac_vita_image_retain_arm();
    CHECK(isaac_ir_oracle_armed() == 1);
}

/* Register `n` images into one bucket in order (the oracle's bucket function
 * is address-based, so skip Image slots until the bucket matches). */
static uint32_t mk_in_bucket(uint32_t bucket, uint32_t vt, const char *nm,
                             uint32_t pw, uint32_t ph, uint32_t bpp,
                             uint16_t strong, uint32_t *ctrl_out)
{
    uint32_t img;
    while (bucket_of(va(OFF_IMG) + s_img_n * IMG_STRIDE) != bucket)
        ++s_img_n;
    img = new_image(vt, nm, pw, ph, bpp, 0U);
    *ctrl_out = new_ctrl(strong);
    manager_register(img, *ctrl_out);
    return img;
}

static uint32_t entry_at(uint32_t bucket, uint32_t index)
{
    return ld32(bucket_hdr(bucket)) + index * 8U;
}

/* Offer {image, ctrl} through the wrapper (real = reference observer). */
static void offer(uint32_t image, uint32_t ctrl)
{
    set_frame(&s_cpu, make_pair(image, ctrl));
    ev_reset();
    isaac_ir_oracle_wrap(&s_cpu, ref_observer);
}

/* The game's own paths around a retained image. */
static void load_hit(uint32_t ctrl)          /* TryAddRef */
{ st16(ctrl + ISAAC_IR_CTRL_STRONG, (uint16_t)(ld16(ctrl + ISAAC_IR_CTRL_STRONG) + 1U)); }
static void sprite_dies(uint32_t image, uint32_t ctrl) /* Release -> true -> hook */
{
    uint16_t s = ld16(ctrl + ISAAC_IR_CTRL_STRONG);
    CHECK(s >= 2U);
    st16(ctrl + ISAAC_IR_CTRL_STRONG, (uint16_t)(s - 1U));
    offer(image, ctrl);
}
/* Floor exit / mod reload / shutdown: the game Unregisters everything itself
 * (direct sub_0056d220 calls that bypass the observer). */
static void manager_clear_behind_our_back(void)
{
    uint32_t b;
    for (b = 0; b < 32U; ++b) {
        while (ld32(bucket_hdr(b) + 4U) > ld32(bucket_hdr(b))) {
            uint32_t p = ld32(bucket_hdr(b));
            do_unregister(&s_cpu, 0x5bfc46U, ld32(p));
        }
    }
}

static uint32_t mk(uint32_t vt, const char *nm, uint32_t pw, uint32_t ph,
                   uint32_t bpp, uint16_t strong, uint32_t *ctrl_out)
{
    uint32_t img = new_image(vt, nm, pw, ph, bpp, 0U);
    uint32_t ctrl = new_ctrl(strong);
    manager_register(img, ctrl);
    *ctrl_out = ctrl;
    return img;
}

static int retained_events_only(void)
{
    return s_event_n == 2U && s_events[0].kind == EV_ENTER &&
           s_events[0].ret == ISAAC_IR_ENTER_SITE_RVA &&
           s_events[0].b == 0xFFFFFFFFU &&
           s_events[1].kind == EV_LEAVE &&
           s_events[1].ret == ISAAC_IR_LEAVE_SITE_RVA;
}

/* ---------------------------------------------- memory / hostile review --- */

static unsigned count_unreg_of(uint32_t image)
{
    unsigned i, n = 0;
    for (i = 0; i < s_event_n && i < 4096; ++i)
        if (s_events[i].kind == EV_UNREG && s_events[i].a == image) ++n;
    return n;
}

static void memory_hostile_scenarios(void)
{
    uint32_t image, ctrl, image2, ctrl2, i;
    uint32_t imgs[16], ctrls[16];

#if defined(ISAAC_VITA_IMAGE_RETAIN_HEADROOM_PHYCONT)
    /* M0. PHYCONT counts toward the headroom (the English device map: RAM+VRAM
     *     ~4 MiB free, PHYCONT ~26 MiB).  Budget 12 / headroom 16 as shipped:
     *     4 + 0 + 26 >= 16 retains; 4 + 0 + 11 < 16 hands the next candidate
     *     to the real observer after releasing the LRU tail. */
    begin("headroom-counts-phycont", 12, 16);
    s_free_ram = 4U << 20; s_free_vram = 0U; s_free_phycont = 26U << 20;
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "P1", 512, 512, 4, 1, &ctrl);
    offer(image, ctrl);
    CHECK(retained_events_only() && isaac_ir_oracle_live_records() == 1);
    CHECK(st()->headroom_pass == 0 && manager_present(image) == 1);
    s_free_phycont = 11U << 20; s_deferred_free = 1;
    image2 = mk(va(ISAAC_IR_VT_PNG_RVA), "P2", 512, 512, 4, 1, &ctrl2);
    offer(image2, ctrl2);
    CHECK(st()->headroom_pass == 1 && st()->headroom_evict == 1);
    CHECK(manager_present(image) == 0 && manager_present(image2) == 0);
    CHECK(isaac_ir_oracle_live_records() == 0);

    /* M0b. Reading cache: offers inside IR_FREE_CACHE_US reuse one reading;
     *      once the window is over the model is read live again. */
    begin("headroom-reading-cache", 12, 16);
    s_clock_frozen = 1;
    s_free_ram = 4U << 20; s_free_vram = 0U; s_free_phycont = 26U << 20;
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "C1", 512, 512, 4, 1, &ctrl);
    offer(image, ctrl);
    s_free_phycont = 0U;                                   /* 4 < 16, not yet seen */
    image2 = mk(va(ISAAC_IR_VT_PNG_RVA), "C2", 512, 512, 4, 1, &ctrl2);
    offer(image2, ctrl2);
    CHECK(isaac_ir_oracle_live_records() == 2 && st()->headroom_pass == 0);
    s_now_us += 4000U;
    imgs[0] = mk(va(ISAAC_IR_VT_PNG_RVA), "C3", 512, 512, 4, 1, &ctrls[0]);
    offer(imgs[0], ctrls[0]);
    CHECK(st()->headroom_pass == 1 && st()->headroom_evict == 2);
    CHECK(isaac_ir_oracle_live_records() == 0 && manager_present(imgs[0]) == 0);

    /* M0c. Our own eviction invalidates the reading: an unused sheet's texture
     *      is freed at once, so one tail can restore the headroom. */
    begin("headroom-evict-invalidates-cache", 12, 16);
    s_clock_frozen = 1;
    s_free_ram = 4U << 20; s_free_vram = 0U; s_free_phycont = 26U << 20;
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "E1", 512, 512, 4, 1, &ctrl);
    offer(image, ctrl);
    image2 = mk(va(ISAAC_IR_VT_PNG_RVA), "E2", 512, 512, 4, 1, &ctrl2);
    offer(image2, ctrl2);
    s_now_us += 4000U;
    s_free_phycont = 11U << 20;                            /* 15 < 16 */
    imgs[0] = mk(va(ISAAC_IR_VT_PNG_RVA), "E3", 512, 512, 4, 1, &ctrls[0]);
    offer(imgs[0], ctrls[0]);                              /* E1 freed: 16 */
    CHECK(st()->headroom_evict == 1 && st()->headroom_pass == 0);
    CHECK(manager_present(image) == 0 && manager_present(image2) == 1 &&
          manager_present(imgs[0]) == 1 && isaac_ir_oracle_live_records() == 2);
#endif

    /* M1. Headroom pressure with vitaGL's deferred frees: free memory does
     *     not rise when a texture is destroyed.  One hook call must release
     *     at most ISAAC_IR_HEADROOM_EVICT_MAX LRU tails (oldest first, the
     *     observer's Unregister return word), never retain the candidate
     *     (the real observer frees it), and leave the model consistent. */
    begin("headroom-deferred-free-bounded", 256, 64);
    for (i = 0; i < 12U; ++i) {
        char nm[8]; nm[0] = 'D'; nm[1] = (char)('0' + i); nm[2] = 0;
        imgs[i] = mk(va(ISAAC_IR_VT_PNG_RVA), nm, 1024, 1024, 4, 1, &ctrls[i]);   /* 4 MiB */
        offer(imgs[i], ctrls[i]);
    }
    CHECK(isaac_ir_oracle_live_records() == 12 && st()->evicted == 0);
    s_free_ram = 50U << 20; s_free_vram = 8U << 20;        /* 58 MiB < 64 MiB */
    s_deferred_free = 1;
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "Dnew", 1024, 1024, 4, 1, &ctrl);
    offer(image, ctrl);
    CHECK(s_free_ram + s_free_vram == 58U << 20);           /* nothing came back */
    CHECK(st()->headroom_evict == ISAAC_IR_HEADROOM_EVICT_MAX);
    CHECK(st()->headroom_pass == 1 && st()->passthrough == 1);
    CHECK(isaac_ir_oracle_live_records() == 12 - ISAAC_IR_HEADROOM_EVICT_MAX);
    CHECK(isaac_ir_oracle_retained_bytes() == (12U - ISAAC_IR_HEADROOM_EVICT_MAX) * (4U << 20));
    CHECK(ev_count(EV_UNREG) == ISAAC_IR_HEADROOM_EVICT_MAX + 1U);
    for (i = 0; i < ISAAC_IR_HEADROOM_EVICT_MAX; ++i) {     /* LRU order, our return word */
        CHECK(manager_present(imgs[i]) == 0);
        CHECK(ld32(imgs[i]) == 0xdead0000U);
    }
    {
        unsigned k, order_ok = 1, seen = 0;
        for (k = 0; k < s_event_n && k < 4096; ++k) {
            if (s_events[k].kind != EV_UNREG) continue;
            if (seen < ISAAC_IR_HEADROOM_EVICT_MAX) {
                if (s_events[k].a != imgs[seen] || s_events[k].ret != ISAAC_IR_UNREGISTER_SITE_RVA)
                    order_ok = 0;
            } else if (s_events[k].a != image || s_events[k].ret != ISAAC_IR_UNREGISTER_SITE_RVA) {
                order_ok = 0;                            /* the real observer's own call */
            }
            ++seen;
        }
        CHECK(order_ok == 1);
    }
    CHECK(manager_present(image) == 0 && ld32(image) == 0xdead0000U);   /* candidate freed as OFF */
    for (i = ISAAC_IR_HEADROOM_EVICT_MAX; i < 12U; ++i) CHECK(manager_present(imgs[i]) == 1);
    /* the next candidate under sustained pressure drains the rest, then the
       table stays empty and every candidate is simply the observer */
    image2 = mk(va(ISAAC_IR_VT_PNG_RVA), "Dnew2", 64, 64, 4, 1, &ctrl2);
    offer(image2, ctrl2);
    CHECK(isaac_ir_oracle_live_records() == 0 && isaac_ir_oracle_retained_bytes() == 0);
    CHECK(st()->headroom_evict == 12 && manager_present(image2) == 0);
    image2 = mk(va(ISAAC_IR_VT_PNG_RVA), "Dnew3", 64, 64, 4, 1, &ctrl2);
    assert_passthrough_identical(make_pair(image2, ctrl2));
    CHECK(manager_count() == 0 && st()->headroom_pass == 3);
    /* pressure gone: retention resumes */
    s_free_ram = 160U << 20;
    image2 = mk(va(ISAAC_IR_VT_PNG_RVA), "Dnew4", 64, 64, 4, 1, &ctrl2);
    offer(image2, ctrl2);
    CHECK(retained_events_only() && isaac_ir_oracle_live_records() == 1);

    /* M2. A retained sheet re-offered under pressure (Load hit, sprite died)
     *     is forgotten and freed by the real observer, on top of the bounded
     *     tail release. */
    begin("headroom-reoffer-retained", 256, 64);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "RA", 512, 512, 4, 1, &ctrl);
    image2 = mk(va(ISAAC_IR_VT_PNG_RVA), "RB", 512, 512, 4, 1, &ctrl2);
    offer(image, ctrl); offer(image2, ctrl2);
    CHECK(isaac_ir_oracle_live_records() == 2);
    load_hit(ctrl);                                          /* A strong 2 */
    s_free_ram = 1U << 20; s_free_vram = 0U; s_deferred_free = 1;
    sprite_dies(image, ctrl);                                /* A strong 1 -> hook */
    CHECK(count_unreg_of(image) == 1 && count_unreg_of(image2) == 1);
    CHECK(manager_present(image) == 0 && manager_present(image2) == 0);
    CHECK(isaac_ir_oracle_live_records() == 0 && isaac_ir_oracle_retained_bytes() == 0);
    CHECK(st()->evicted == 2 && st()->headroom_evict == 2 && st()->evicted_bytes == 2U << 20);
    CHECK(st()->reuse == 0 && st()->headroom_pass == 1);
    {
        /* the observer's own Unregister of A came last (after B's release):
           our record never let A become the evicted tail */
        unsigned k, last = 0;
        for (k = 0; k < s_event_n && k < 4096; ++k)
            if (s_events[k].kind == EV_UNREG) last = s_events[k].a;
        CHECK(last == image);
    }
    /* M2b. Transient pressure with immediate frees (textures not drawn for
     *      FRAME_PURGE_FREQ frames): the bounded release stops as soon as
     *      free memory recovers and the candidate is retained normally. */
    begin("headroom-transient-recovers", 256, 64);
    for (i = 0; i < 4U; ++i) {
        char nm[8]; nm[0] = 'T'; nm[1] = (char)('0' + i); nm[2] = 0;
        imgs[i] = mk(va(ISAAC_IR_VT_PNG_RVA), nm, 1024, 1024, 4, 1, &ctrls[i]);   /* 4 MiB */
        offer(imgs[i], ctrls[i]);
    }
    s_free_ram = 57U << 20; s_free_vram = 0U;                /* 57 < 64: two 4 MiB frees recover */
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "Tnew", 1024, 1024, 4, 1, &ctrl);
    offer(image, ctrl);
    CHECK(st()->headroom_evict == 2 && st()->headroom_pass == 0 && st()->retained_new == 5);
    CHECK(manager_present(imgs[0]) == 0 && manager_present(imgs[1]) == 0);
    CHECK(manager_present(imgs[2]) == 1 && manager_present(imgs[3]) == 1 && manager_present(image) == 1);
    CHECK(isaac_ir_oracle_live_records() == 3 && s_free_ram + s_free_vram == 65U << 20);
    /* a retained sheet re-offered in the same situation keeps its record */
    load_hit(ctrls[2]);
    s_free_ram = 57U << 20;
    sprite_dies(imgs[2], ctrls[2]);
    CHECK(st()->reuse == 1 && st()->headroom_evict == 4 && st()->headroom_pass == 0);
    CHECK(manager_present(imgs[2]) == 1 && isaac_ir_oracle_live_records() == 1);
    CHECK(count_unreg_of(imgs[2]) == 0 && manager_present(imgs[3]) == 0 && manager_present(image) == 0);

    /* M3. A sheet larger than the whole budget is never retained and the
     *     LRU is left untouched (previously it flushed every record). */
    begin("oversize-preserves-lru", 4, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "O0", 512, 512, 4, 1, &ctrl);       /* 1 MiB */
    image2 = mk(va(ISAAC_IR_VT_PNG_RVA), "O1", 512, 512, 4, 1, &ctrl2);
    offer(image, ctrl); offer(image2, ctrl2);
    CHECK(isaac_ir_oracle_live_records() == 2);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "Obig", 1024, 2048, 4, 1, &ctrl);   /* 8 MiB > 4 MiB */
    assert_passthrough_identical(make_pair(image, ctrl));
    CHECK(manager_present(image) == 0 && st()->oversize == 1 && st()->ineligible == 1);
    CHECK(isaac_ir_oracle_live_records() == 2 && isaac_ir_oracle_retained_bytes() == 2U << 20);
    CHECK(st()->evicted == 0 && manager_count() == 2);

    /* M4. Hostile bucket vectors: every malformed shape falls closed
     *     identically to the observer and records nothing; a retained record
     *     whose bucket turns hostile is dropped without a guest call. */
    {
        static const char *names[] = { "end<begin", "cap<end", "odd-len", "table-null", "table-misaligned", "begin-misaligned", "scan-ceiling" };
        uint32_t k;
        for (k = 0; k < 7U; ++k) {
            uint32_t b, hdr;
            begin(names[k], 64, 0);
            if (k == 6) {
                /* the candidate sits in the last bucket; 31 x 1024 dummies
                   before it exceed the 4,096-entry scan ceiling first */
                uint32_t bb, n;
                do {
                    image = new_image(va(ISAAC_IR_VT_PNG_RVA), "hb.png", 64, 64, 4, 0);
                } while (bucket_of(image) != 31U);
                ctrl = new_ctrl(1);
                manager_register(image, ctrl);
                for (bb = 0; bb < 31U; ++bb)
                    for (n = 0; n < 1024U; ++n) {
                        uint32_t e = ld32(bucket_hdr(bb) + 4U);
                        st32(e, 0xdead0000U + n); st32(e + 4U, 0xbeef0000U);
                        st32(bucket_hdr(bb) + 4U, e + 8U);
                    }
            } else {
                image = mk(va(ISAAC_IR_VT_PNG_RVA), "hb.png", 64, 64, 4, 1, &ctrl);
            }
            b = bucket_of(image); hdr = bucket_hdr(b);
            switch (k) {
            case 0: st32(hdr + 4U, ld32(hdr) - 8U); break;            /* end < begin */
            case 1: st32(hdr + 8U, ld32(hdr + 4U) - 4U); break;       /* cap < end */
            case 2: st32(hdr + 4U, ld32(hdr + 4U) + 4U); break;       /* (end-begin) & 7 */
            case 3: st32(va(ISAAC_IR_BUCKETS_PTR_RVA), 0U); break;
            case 4: st32(va(ISAAC_IR_BUCKETS_PTR_RVA), va(OFF_BUCKETS) | 2U); break;
            case 5: st32(hdr, ld32(hdr) | 2U); break;                 /* bucket skipped */
            default: break;                                           /* 6: prepared above */
            }
            if (k == 0 || k == 1 || k == 2 || k == 5) {
                /* the fake Unregister walks the same header: emulate its view */
                assert_passthrough_identical(make_pair(image, ctrl));
            } else {
                assert_passthrough_identical(make_pair(image, ctrl));
                CHECK(manager_present(image) == 0);
            }
            CHECK(isaac_ir_oracle_live_records() == 0);
            CHECK(st()->offered == 1 && st()->absent == 1 && st()->passthrough == 1);
            CHECK(st()->retained_new == 0);
        }
    }
    begin("hostile-bucket-on-eviction", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "hv0", 1024, 1024, 4, 1, &ctrl);      /* 4 MiB */
    offer(image, ctrl);
    CHECK(isaac_ir_oracle_live_records() == 1);
    {
        uint32_t hdr = bucket_hdr(bucket_of(image)), end = ld32(hdr + 4U);
        st32(hdr + 4U, ld32(hdr) - 8U);                       /* end < begin */
        isaac_ir_oracle_set_budget(1U << 20, 0U);
        do {                                                  /* a candidate in another bucket */
            image2 = new_image(va(ISAAC_IR_VT_PNG_RVA), "hv1", 64, 64, 4, 0);
        } while (bucket_of(image2) == bucket_of(image));
        ctrl2 = new_ctrl(1); manager_register(image2, ctrl2);
        /* while any bucket is malformed the scan fails closed for every
           candidate: nothing new is retained, nothing is evicted, the record
           is frozen (the manager still owns the sheet) */
        assert_passthrough_identical(make_pair(image2, ctrl2));
        CHECK(count_unreg_of(image) == 0 && st()->dropped_stale == 0 && st()->absent == 1);
        CHECK(isaac_ir_oracle_live_records() == 1 && isaac_ir_oracle_retained_bytes() == 4U << 20);
        st32(hdr + 4U, end);                                  /* restore: still the game's */
        CHECK(manager_present(image) == 1 && ld16(ctrl + ISAAC_IR_CTRL_STRONG) == 1);
        /* shape restored: the frozen record is evictable again */
        image2 = mk(va(ISAAC_IR_VT_PNG_RVA), "hv2", 64, 64, 4, 1, &ctrl2);
        offer(image2, ctrl2);
        CHECK(count_unreg_of(image) == 1 && manager_present(image) == 0);
        CHECK(isaac_ir_oracle_live_records() == 1 && isaac_ir_oracle_retained_bytes() == 64U * 64U * 4U);
    }

    /* M5. The destructor run by our eviction offers a sheet that is itself
     *     retained: the nested offer passes through (the real observer
     *     Unregisters it), our record goes stale and is later dropped
     *     without a second Unregister or any dereference of the dead Image. */
    begin("nested-offer-of-evicted-image", 8, 0);
    image2 = mk(va(ISAAC_IR_VT_PNG_RVA), "nB", 64, 64, 4, 1, &ctrl2);       /* small, retained first */
    offer(image2, ctrl2);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "nA", 1024, 1024, 4, 1, &ctrl);      /* 4 MiB */
    offer(image, ctrl);
    CHECK(isaac_ir_oracle_live_records() == 2);
    s_nested_image = image2; s_nested_ctrl = ctrl2;      /* the first destroy re-offers B */
    {
        uint32_t c3, i3 = mk(va(ISAAC_IR_VT_PNG_RVA), "nC", 1024, 1024, 4, 1, &c3);
        offer(i3, c3);
        /* LRU: B (oldest), A, C(head).  8 MiB + 16 KiB > 8 MiB: evict B; B's
           own destroy fires the nested offer of B (ctrl strong now 0): the
           real observer's Enter/Leave no-op, no Unregister, no miss. */
        CHECK(count_unreg_of(image2) == 1 && ev_count(EV_UNREG_MISS) == 0);
        CHECK(ev_count(EV_DESTROY) == 1 && st()->nested == 0 && st()->evicted == 1);
        CHECK(st()->passthrough == 1 && count_unreg_of(image2) == 1);
        CHECK(manager_present(image) == 1 && manager_present(i3) == 1);
        CHECK(isaac_ir_oracle_live_records() == 2 && isaac_ir_oracle_retained_bytes() == 8U << 20);
        CHECK(manager_count() == 2 && isaac_ir_oracle_in_evict() == 0);
    }
    /* the reverse order: the nested offer hits a sheet that is still retained */
    begin("nested-offer-of-live-retained-image", 8, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "nA", 1024, 1024, 4, 1, &ctrl);      /* 4 MiB, oldest */
    offer(image, ctrl);
    image2 = mk(va(ISAAC_IR_VT_PNG_RVA), "nB", 64, 64, 4, 1, &ctrl2);       /* retained after A */
    offer(image2, ctrl2);
    s_nested_image = image2; s_nested_ctrl = ctrl2;
    {
        uint32_t c3, i3 = mk(va(ISAAC_IR_VT_PNG_RVA), "nC", 1024, 1024, 4, 1, &c3);
        offer(i3, c3);                                       /* 8 MiB + 16 KiB > 8 MiB: evict A */
        /* A's dtor offers B (retained, strong 1) while in_evict: B is kept
           (never __real: Unregister would re-enter the manager Mutex), its
           record and the manager's reference stay intact. */
        CHECK(count_unreg_of(image) == 1 && count_unreg_of(image2) == 0);
        CHECK(manager_present(image2) == 1 && ld32(image2) == va(ISAAC_IR_VT_PNG_RVA));
        CHECK(ld16(ctrl2 + ISAAC_IR_CTRL_STRONG) == 1 && ld32(ctrl2 + 0xcU) == 0U);
        CHECK(st()->nested == 1 && st()->evicted == 1 && st()->dropped_stale == 0);
        CHECK(isaac_ir_oracle_live_records() == 2);          /* B and C, both live */
        CHECK(isaac_ir_oracle_retained_bytes() == (4U << 20) + 64U * 64U * 4U);
        /* B is Unregistered exactly once, by our eviction, later */
        isaac_ir_oracle_set_budget(1U << 20, 0U);
        image = mk(va(ISAAC_IR_VT_PNG_RVA), "nD", 64, 64, 4, 1, &ctrl);
        offer(image, ctrl);                                  /* 4 MiB + 32 KiB > 1 MiB: evict B, then C */
        CHECK(count_unreg_of(image2) == 1 && ld32(image2) == 0xdead0000U && manager_present(image2) == 0);
        CHECK(count_unreg_of(i3) == 1 && isaac_ir_oracle_live_records() == 1 && st()->dropped_stale == 0);
        CHECK(isaac_ir_oracle_retained_bytes() == 64U * 64U * 4U && manager_count() == 1);
        CHECK(s_deadlock == 0 && ev_count(EV_UNREG_MISS) == 0);
    }

    /* M6. Enter / Leave stack-contract breaches: fault path, nothing
     *     retained, in_evict clear, nothing evicted. */
    begin("enter-esp-contract", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "e0", 64, 64, 4, 1, &ctrl);
    s_enter_break_esp = 1;
    offer(image, ctrl);
    s_enter_break_esp = 0;
    CHECK(st()->fault == 1 && s_fault_hit == 1);
    CHECK(strcmp(s_fault_what, "image retain: lock Enter stack contract") == 0);
    CHECK(isaac_ir_oracle_live_records() == 0 && isaac_ir_oracle_in_evict() == 0);
    CHECK(ev_count(EV_UNREG) == 0 && manager_present(image) == 1);
    begin("leave-esp-contract", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "l0", 64, 64, 4, 1, &ctrl);
    s_leave_break_esp = 1;
    offer(image, ctrl);
    s_leave_break_esp = 0;
    CHECK(st()->fault == 1 && strcmp(s_fault_what, "image retain: lock Leave stack contract") == 0);
    CHECK(isaac_ir_oracle_live_records() == 0 && ev_count(EV_UNREG) == 0);

    /* M7. Same Image *and* control-block addresses reused by the allocator
     *     after a behind-our-back Unregister: the record is refreshed as a
     *     reuse, accounting follows the new sheet, and eviction Unregisters
     *     the live sheet exactly once. */
    begin("same-image-same-ctrl-reuse", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "sr", 64, 64, 4, 1, &ctrl);
    offer(image, ctrl);
    do_unregister(&s_cpu, 0x5bfc46U, image);                 /* dies behind our back */
    CHECK(ld32(image) == 0xdead0000U && ld16(ctrl + ISAAC_IR_CTRL_STRONG) == 0);
    st32(image, va(ISAAC_IR_VT_PNG_RVA));                    /* same addresses, new sheet */
    st32(image + ISAAC_IR_IMG_TEX0, 0x555U);
    st16(image + ISAAC_IR_IMG_PW, 256); st16(image + ISAAC_IR_IMG_PH, 256);
    st16(ctrl + ISAAC_IR_CTRL_STRONG, 1);
    manager_register(image, ctrl);
    offer(image, ctrl);
    CHECK(retained_events_only());
    CHECK(st()->reuse == 1 && st()->retained_new == 1 && st()->dropped_stale == 0);
    CHECK(isaac_ir_oracle_live_records() == 1 && isaac_ir_oracle_retained_bytes() == 256U * 256U * 4U);
    isaac_ir_oracle_set_budget(2048U, 0U);                   /* admits the 1 KiB probe, forces the eviction */
    image2 = mk(va(ISAAC_IR_VT_PNG_RVA), "sr2", 16, 16, 4, 1, &ctrl2);
    offer(image2, ctrl2);
    CHECK(count_unreg_of(image) == 1 && manager_present(image) == 0 && ld32(image) == 0xdead0000U);
    CHECK(count_unreg_of(image2) == 0 && manager_present(image2) == 1 && manager_count() == 1);

    /* M8. Receipt lines fit isaac_vita_log's 384-byte body at u32 max. */
    begin("receipt-worst-case-length", 64, 64);
    isaac_ir_oracle_saturate_stats();
    s_free_ram = 0xFFFFFFFFu; s_free_vram = 0xFFFFFFFFu;
    s_max_log_len = 0;
    isaac_vita_image_retain_report("loading-complete");
    CHECK(strlen(s_last_receipt) < 384U && strlen(s_last_mem) < 384U);
    CHECK(s_max_log_len < 384U);
    CHECK(strstr(s_last_receipt, " why=loading-complete") != NULL);
    CHECK(strstr(s_last_mem, " why=loading-complete") != NULL);
    isaac_vita_image_retain_disarm();
}

#if defined(IR_ORACLE_FLUSH)
/* ------------------------------------------ batch-flush flags (0x04/0x10) - */
#if defined(ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED)
#define IRT(f) isaac_ir_oracle_touched(ISAAC_IRT_F_##f)
#endif
#if defined(ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED) && ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED == 2
#define IRT_ON 1
#else
#define IRT_ON 0
#endif

/* An Image a flush has drawn and nothing else references: flags 0x11, in the
 * TOUCHED vector once, no pending pair. */
static uint32_t mk_touched(const char *nm, uint32_t *ctrl_out)
{
    uint32_t image = mk(va(ISAAC_IR_VT_PNG_RVA), nm, 512, 512, 4, 1, ctrl_out);
    st32(image + ISAAC_IR_IMG_FLAGS, 0x01U);
    touched_add(image);
    return image;
}

static void flush_scenarios(void)
{
    uint32_t a, ac, b, bc;
    const uint32_t mib = 512U * 512U * 4U;

    (void)mib;
    /* F1. The H2 case: a sheet with flags 0x11 whose last sprite dies.
     *     ON keeps it (and its TOUCHED entry); OFF / OBSERVE Unregister it,
     *     which erases the entry -- OBSERVE counts what ON would keep. */
    begin("touched-admit", 64, 0);
    a = mk_touched("t1.png", &ac);
    offer(a, ac);
#if IRT_ON
    CHECK(retained_events_only());
    CHECK(manager_present(a) == 1 && ld16(ac + ISAAC_IR_CTRL_STRONG) == 1);
    CHECK(fv_count(FV_TOUCHED, a) == 1 && ld32(a + ISAAC_IR_IMG_FLAGS) == 0x11U && s_vec_erases == 0);
    CHECK(st()->retained_new == 1 && st()->ineligible == 0 && isaac_ir_oracle_retained_bytes() == mib);
    CHECK(IRT(OFFERS) == 1 && IRT(ADMIT) == 1 && IRT(ADMIT_BYTES) == mib);
    CHECK(IRT(DIRTY) == 0 && IRT(VEC) == 0 && IRT(PAIR) == 0);
    /* the next user takes it from the manager and lets go again: a reuse */
    load_hit(ac);
    sprite_dies(a, ac);
    CHECK(retained_events_only() && st()->reuse == 1 && IRT(ADMIT) == 2 && IRT(ADMIT_BYTES) == mib);
    /* the game's own sweep drops the entry; a later eviction is an ordinary
     * Unregister that finds nothing in the vector */
    fake_sweep(a);
    CHECK(fv_count(FV_TOUCHED, a) == 0 && ld32(a + ISAAC_IR_IMG_FLAGS) == 0x01U);
    isaac_ir_oracle_set_budget(mib, 0U);
    b = mk(va(ISAAC_IR_VT_PNG_RVA), "t1b.png", 512, 512, 4, 1, &bc);
    offer(b, bc);
    CHECK(count_unreg_of(a) == 1 && manager_present(a) == 0 && manager_present(b) == 1);
    CHECK(s_vec_erases == 0 && s_walker_violations == 0 && IRT(PINNED_SKIP) == 0);
#else
    CHECK(count_unreg_of(a) == 1 && manager_present(a) == 0 && ld32(a) == 0xdead0000U);
    CHECK(fv_count(FV_TOUCHED, a) == 0 && s_vec_erases == 1);
    CHECK(st()->retained_new == 0 && st()->ineligible == 1 && isaac_ir_oracle_live_records() == 0);
# if defined(ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED)
    CHECK(IRT(OFFERS) == 1 && IRT(ADMIT) == 1 && IRT(ADMIT_BYTES) == mib);
    CHECK(IRT(DIRTY) == 0 && IRT(VEC) == 0 && IRT(PAIR) == 0);
# endif
    (void)b; (void)bc;
#endif

    /* F2. T1: quads still queued (0x04) -- refused in every mode; Unregister
     *     drops them from the DIRTY vector as the OFF build does. */
    begin("touched-dirty-refused", 64, 0);
    a = mk_touched("t2.png", &ac);
    dirty_add(a);                                        /* flags 0x15 */
    offer(a, ac);
    CHECK(count_unreg_of(a) == 1 && manager_present(a) == 0 && st()->retained_new == 0);
    CHECK(fv_count(FV_DIRTY, a) == 0 && fv_count(FV_TOUCHED, a) == 0);
    a = mk(va(ISAAC_IR_VT_PNG_RVA), "t2b.png", 512, 512, 4, 1, &ac);
    st32(a + ISAAC_IR_IMG_FLAGS, 0x01U);
    dirty_add(a);                                        /* flags 0x05 */
    offer(a, ac);
    CHECK(count_unreg_of(a) == 1 && manager_present(a) == 0 && st()->ineligible == 2);
#if defined(ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED)
    CHECK(IRT(OFFERS) == 2 && IRT(DIRTY) == 2 && IRT(ADMIT) == 0 && IRT(ADMIT_BYTES) == 0);
#endif

    /* F3. T2: the flags and the vectors must agree. */
    begin("touched-vector-mismatch", 64, 0);
    a = mk(va(ISAAC_IR_VT_PNG_RVA), "t3a.png", 512, 512, 4, 1, &ac);
    st32(a + ISAAC_IR_IMG_FLAGS, 0x11U);                 /* bit without an entry */
    offer(a, ac);
    CHECK(count_unreg_of(a) == 1 && manager_present(a) == 0);
    a = mk_touched("t3b.png", &ac);
    fv_push(FV_TOUCHED, a);                              /* two entries */
    offer(a, ac);
    CHECK(count_unreg_of(a) == 1 && manager_present(a) == 0);
    (void)fv_erase(FV_TOUCHED, a);
    a = mk_touched("t3c.png", &ac);
    fv_push(FV_DIRTY, a);                                /* DIRTY entry without the bit */
    offer(a, ac);
    CHECK(count_unreg_of(a) == 1 && manager_present(a) == 0);
    a = mk_touched("t3d.png", &ac);
    st32(va(ISAAC_IR_TOUCHED_END_RVA), ld32(va(ISAAC_IR_TOUCHED_BEGIN_RVA)) - 4U);   /* end < begin */
    offer(a, ac);
    CHECK(count_unreg_of(a) == 1 && manager_present(a) == 0);
    st32(va(ISAAC_IR_TOUCHED_END_RVA), ld32(va(ISAAC_IR_TOUCHED_BEGIN_RVA)));
    a = mk_touched("t3e.png", &ac);
    st32(va(ISAAC_IR_TOUCHED_END_RVA), ld32(va(ISAAC_IR_TOUCHED_END_RVA)) + 2U);     /* unaligned end */
    offer(a, ac);
    CHECK(count_unreg_of(a) == 1 && manager_present(a) == 0);
    st32(va(ISAAC_IR_TOUCHED_END_RVA), ld32(va(ISAAC_IR_TOUCHED_BEGIN_RVA)));
    CHECK(st()->retained_new == 0 && st()->ineligible == 5 && isaac_ir_oracle_live_records() == 0);
#if defined(ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED)
    CHECK(IRT(OFFERS) == 5 && IRT(VEC) == 5 && IRT(ADMIT) == 0 && IRT(DIRTY) == 0 && IRT(PAIR) == 0);
#endif

    /* F4. T3: a pending {batch, Image} pair, or the Image latched as the most
     *     recent pair's -- Unregister would have dropped those draws. */
    begin("touched-pending-pair", 64, 0);
    a = mk_touched("t4a.png", &ac);
    pair_add(0x1234U, a);
    st32(va(ISAAC_IR_LAST_PAIR_IMAGE_RVA), 0U);          /* the pair alone */
    offer(a, ac);
    CHECK(count_unreg_of(a) == 1 && manager_present(a) == 0);
    CHECK(ld32(va(ISAAC_IR_PAIRS_END_RVA)) == ld32(va(ISAAC_IR_PAIRS_BEGIN_RVA)));
    a = mk_touched("t4b.png", &ac);
    st32(va(ISAAC_IR_LAST_PAIR_IMAGE_RVA), a);           /* the latch alone */
    offer(a, ac);
    CHECK(count_unreg_of(a) == 1 && manager_present(a) == 0);
    st32(va(ISAAC_IR_LAST_PAIR_IMAGE_RVA), 0U);
    a = mk_touched("t4c.png", &ac);
    pair_add(0x1234U, a + IMG_STRIDE);                   /* another Image's pair does not count */
    st32(va(ISAAC_IR_LAST_PAIR_IMAGE_RVA), a + IMG_STRIDE);
    offer(a, ac);
#if IRT_ON
    CHECK(retained_events_only() && manager_present(a) == 1 && st()->retained_new == 1);
#else
    CHECK(count_unreg_of(a) == 1 && manager_present(a) == 0 && st()->retained_new == 0);
#endif
#if defined(ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED)
    CHECK(IRT(OFFERS) == 3 && IRT(PAIR) == 2 && IRT(ADMIT) == 1 && IRT(VEC) == 0);
#endif

    /* F5. The base module's behaviour for an untouched Image with a pending
     *     pair is unchanged in every mode (retained); the new counter names
     *     how often that happens. */
    begin("pair-untouched-counted", 64, 0);
    a = mk(va(ISAAC_IR_VT_PNG_RVA), "t5.png", 512, 512, 4, 1, &ac);
    st32(a + ISAAC_IR_IMG_FLAGS, 0x01U);
    pair_add(0x1234U, a);
    offer(a, ac);
    CHECK(retained_events_only() && manager_present(a) == 1 && st()->retained_new == 1);
#if defined(ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED)
    CHECK(IRT(PAIR_UNTOUCHED) == 1 && IRT(OFFERS) == 0);
#endif

#if IRT_ON
    /* F6. Rule E.  While a walk of the TOUCHED vector is on the stack, an
     *     eviction must not Unregister a different touched Image (the OFF
     *     build never shrinks the vector there).  The pinned record is passed
     *     over; the budget stays a hard cap. */
    begin("touched-rule-e", 64, 0);
    isaac_ir_oracle_set_budget(mib + mib / 2U, 0U);
    a = mk_touched("t6a.png", &ac);
    offer(a, ac);
    CHECK(manager_present(a) == 1 && isaac_ir_oracle_retained_bytes() == mib);
    s_walker_active = 1; s_walker_image = 0U;
    b = mk(va(ISAAC_IR_VT_PNG_RVA), "t6b.png", 512, 512, 4, 1, &bc);
    offer(b, bc);                                        /* over budget: `a` is the LRU tail */
    CHECK(count_unreg_of(a) == 0 && manager_present(a) == 1 && fv_count(FV_TOUCHED, a) == 1);
    CHECK(count_unreg_of(b) == 1 && manager_present(b) == 0);
    CHECK(s_walker_violations == 0 && s_vec_erases == 0 && IRT(PINNED_SKIP) == 1);
    CHECK(isaac_ir_oracle_retained_bytes() == mib && isaac_ir_oracle_live_records() == 1);
    /* a touched candidate: every record an eviction can reach is pinned ->
     * the candidate itself is Unregistered at once (its own release: the one
     * shrink the OFF build performs here too) */
    b = mk_touched("t6c.png", &bc);
    s_walker_image = b;
    offer(b, bc);
    CHECK(count_unreg_of(b) == 1 && manager_present(b) == 0 && fv_count(FV_TOUCHED, b) == 0);
    CHECK(count_unreg_of(a) == 0 && manager_present(a) == 1 && fv_count(FV_TOUCHED, a) == 1);
    CHECK(s_walker_violations == 0 && s_vec_erases == 1);
    CHECK(IRT(EVICT_BLOCKED) == 1 && IRT(BUDGET_REFUSE) == 1 && IRT(PINNED_SKIP) == 3);
    CHECK(isaac_ir_oracle_retained_bytes() == mib && isaac_ir_oracle_live_records() == 1);
    CHECK(isaac_ir_oracle_retained_bytes() <= mib + mib / 2U && s_deadlock == 0 && s_fault_hit == 0);
    /* the sweep unpins it; the next over-budget candidate evicts it */
    s_walker_active = 0;
    fake_sweep(a);
    b = mk(va(ISAAC_IR_VT_PNG_RVA), "t6d.png", 512, 512, 4, 1, &bc);
    offer(b, bc);
    CHECK(count_unreg_of(a) == 1 && manager_present(a) == 0 && manager_present(b) == 1);
    CHECK(IRT(PINNED_SKIP) == 3 && IRT(EVICT_BLOCKED) == 1 && isaac_ir_oracle_retained_bytes() == mib);

    /* F7. A stale record is never pinned (the game Unregistered the Image
     *     behind our back and its memory still reads flags 0x11). */
    begin("touched-stale-not-pinned", 64, 0);
    isaac_ir_oracle_set_budget(mib, 0U);
    a = mk_touched("t7a.png", &ac);
    offer(a, ac);
    do_unregister(&s_cpu, 0x5bfc46U, a);
    st32(a + ISAAC_IR_IMG_FLAGS, 0x11U);
    b = mk(va(ISAAC_IR_VT_PNG_RVA), "t7b.png", 512, 512, 4, 1, &bc);
    offer(b, bc);
    CHECK(manager_present(b) == 1 && IRT(PINNED_SKIP) == 0 && st()->dropped_stale == 1);
    CHECK(isaac_ir_oracle_live_records() == 1 && isaac_ir_oracle_retained_bytes() == mib);
#endif
}
#endif

#if defined(ISAAC_VITA_IMAGE_PRELOAD)
/* -------------------------------------------------------------- preload --- */
/* The runner builds these legs with
 *   ISAAC_VITA_IMAGE_PRELOAD_LIST = "gfx/BulletAtlas.png,gfx/b.png"
 *   ISAAC_VITA_IMAGE_PRELOAD_MAX_KB = 2048, _RESERVE_MB = 8 or 4 (P_NEED_KIB below). */
#define IRP(f) isaac_ir_oracle_preload(ISAAC_IRP_F_##f)
#define DP(f) (IRP(f) - s_p0[ISAAC_IRP_F_##f])
static uint32_t s_p0[ISAAC_IRP_F_ENTRIES + 1];
static void p_begin(const char *name, uint32_t budget_mb, uint32_t headroom_mb)
{
    unsigned k;
#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN)
    s_pin_n = 0U;                        /* keeps the PIN armed line of this begin */
    memset(s_pin_log, 0, sizeof s_pin_log);
#endif
    begin(name, budget_mb, headroom_mb);
    for (k = 0; k <= ISAAC_IRP_F_ENTRIES; ++k)
        s_p0[k] = isaac_ir_oracle_preload(k);
    s_load_flags = 0x01U; s_load_pw = 1024U; s_load_ph = 512U;
    s_load_fail = 0; s_load_orphan = 0; s_load_break_esp = 0; s_release_break_esp = 0; s_hook_break_esp = 0;
    s_load_calls = 0; s_load_bad_args = 0; s_release_calls = 0; s_hook_calls = 0;
    s_heap_hr = 20U << 20; s_heap_terminal = 0;
    st32(va(ISAAC_IR_CTRL_VT_RVA) + ISAAC_IR_CTRL_VT_RELEASE, va(OFF_FAKE_RELEASE));
}
/* Level::Init has returned: the SFX floor seam calls the preload with the
 * caller's ESP; whatever happens, ESP comes back. */
static void level(CPU *c)
{
    uint32_t esp0;
    cpu_bind(c);
    esp0 = c->esp;
    ev_reset();
    isaac_vita_image_preload_level_init(c);
    CHECK(c->esp == esp0);
}
static uint32_t last_image(uint32_t back) { return va(OFF_IMG) + (s_img_n - back) * IMG_STRIDE; }
static uint32_t last_ctrl(uint32_t back) { return va(OFF_CTRL) + (s_ctrl_n - back) * CTRL_STRIDE; }

/* The `h` gate in KiB (RESERVE_MB + 2 x MAX_KB with MAX_KB 2048), written out
 * per reserve the runner builds, and which bench-route column applies. */
#if ISAAC_VITA_IMAGE_PRELOAD_MAX_KB != 2048
#error the preload scenarios are written for MAX_KB 2048
#endif
#if ISAAC_VITA_IMAGE_PRELOAD_RESERVE_MB == 8
#define P_NEED_KIB 12288U
#define P_NEED_TEXT " need_kb=12288 "
#define P_ROUTE_RUN(r) ((r).run8)
#elif ISAAC_VITA_IMAGE_PRELOAD_RESERVE_MB == 4
#define P_NEED_KIB 8192U
#define P_NEED_TEXT " need_kb=8192 "
#define P_ROUTE_RUN(r) ((r).run4)
#else
#error the preload scenarios know RESERVE_MB 8 and 4
#endif

#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN)
/* ---------------------------------------------------------- preload pin --- */
/* ISAAC_VITA_IMAGE_PRELOAD_PIN = 1 (OBSERVE) or 2 (ON).  Every scenario runs
 * in both modes: OBSERVE must take the OFF decisions (the expectations of
 * the #else arms are OFF's) and only print; ON pins.  The PIN counters are
 * reset by each arm (p_begin), so they are read absolutely. */
#define PP(f) isaac_ir_oracle_preload_pin(ISAAC_IRPP_F_##f)
#if ISAAC_VITA_IMAGE_PRELOAD_PIN == 2
#define PIN_ON 1
#define PIN_MODE " mode=on "
#else
#define PIN_ON 0
#define PIN_MODE " mode=observe "
#endif
#define PIN_STATE_WAITING 1U   /* IRP_WAITING */
#define PIN_STATE_ON_FREE 3U   /* IRP_ON_FREE = IRP_USED + 1 */
static int pin_has(const char *needle)
{
    unsigned k;
    const unsigned n = s_pin_n < PIN_KEEP ? s_pin_n : PIN_KEEP;

    for (k = 0; k < n; ++k)
        if (strstr(s_pin_log[k], needle) != NULL)
            return 1;
    return 0;
}
/* `n` (<= 4) other 2 MiB sheets the game decoded and let go of (strong 1
 * offers, in order).  Returns the first; the events are the last offer's. */
static uint32_t pin_others(unsigned n)
{
    static const char *const names[] = { "o1.png", "o2.png", "o3.png", "o4.png" };
    uint32_t first = 0U, img, ctrl;
    unsigned k;

    for (k = 0; k < n && k < 4U; ++k) {
        img = mk(va(ISAAC_IR_VT_PNG_RVA), names[k], 1024, 512, 4, 1, &ctrl);
        if (k == 0U)
            first = img;
        offer(img, ctrl);
    }
    return first;
}

/* The image of the n-th (from 0) Unregister among the last offer's events. */
static uint32_t pin_unreg_nth(unsigned n)
{
    unsigned i, k = 0U;

    for (i = 0; i < s_event_n && i < 4096U; ++i)
        if (s_events[i].kind == EV_UNREG && k++ == n)
            return s_events[i].a;
    return 0U;
}

static void pin_scenarios(void)
{
    uint32_t a, ac, b, o1, s, sc;
    const uint32_t sheet = 1024U * 512U * 4U;

    /* Q1. "Present" is another key that ends in the list name (a mod's copy,
     *     held by the game).  OBSERVE prints it and does nothing, as OFF;
     *     ON replays Load, which decodes the game's own key (alias). */
    p_begin("pin-present-alias", 12, 0);
    CHECK(pin_has("KAGE VITA IMAGE PRELOAD PIN: armed" PIN_MODE "share=1/2 set_max=4 lines_max=64 build="));
    a = mk(va(ISAAC_IR_VT_PNG_RVA), "mods/x/gfx/BulletAtlas.png", 1024, 512, 4, 2, &ac);
    level(&s_cpu);
    CHECK(DP(PRESENT) == 1 && ld16(ac + ISAAC_IR_CTRL_STRONG) == 2 && manager_present(a) == 1);
    CHECK(pin_has("KAGE VITA IMAGE PRELOAD PIN: present name=gfx/BulletAtlas.png level="));
    CHECK(pin_has(" k=1/1 image="));
    CHECK(pin_has(" strong=2 flags=00000000 bytes=2097152 rec=0 own=0 key=mods/x/gfx/BulletAtlas.png"));
#if PIN_ON
    CHECK(s_load_calls == 2 && DP(LOADED) == 2 && DP(RETAINED) == 2 && manager_count() == 3);
    CHECK(PP(ALIAS) == 1 && PP(EXACT) == 0 && PP(HELD) == 0);
    CHECK(PP(PINNED) == 2 && PP(PINS) == 2 && PP(PINNED_BYTES) == 2U * sheet && PP(REFUSED) == 0);
    CHECK(pin_has(" mode=on pins=2 pinned_bytes=4194304 share=6291456 released=0 pinned=2 own=0 exact=0 alias=1 held=0"));
#else
    CHECK(s_load_calls == 1 && DP(LOADED) == 1 && manager_count() == 2);
    CHECK(PP(ALIAS) == 0 && PP(PINNED) == 0 && PP(PINS) == 0);
    CHECK(pin_has(" mode=observe pins=0 pinned_bytes=0 share=6291456 released=0 pinned=0 own=0 exact=0 alias=0 held=0"));
#endif

    /* Q2. Budget pressure before the fight: the eviction walks past the
     *     pinned sheets (ON) and takes the oldest unpinned one; the fight's
     *     Load then hits, and letting go of it releases the pin (`used`). */
    p_begin("pin-pressure-hit", 8, 0);
    level(&s_cpu);
    a = last_image(2); ac = last_ctrl(2);
    b = last_image(1);
    CHECK(DP(RETAINED) == 2 && PP(PINNED) == (PIN_ON ? 2U : 0U) && PP(REFUSED) == 0);
    o1 = pin_others(3);
    CHECK(isaac_ir_oracle_retained_bytes() <= (8U << 20) && PP(YIELD) == 0);
#if PIN_ON
    CHECK(count_unreg_of(o1) == 1 && count_unreg_of(a) == 0 && PP(SKIP) == 2);
    CHECK(manager_present(a) == 1 && manager_present(b) == 1 && DP(EVICTED_UNUSED) == 0);
    load_hit(ac);
    sprite_dies(a, ac);
    CHECK(DP(HIT) == 1 && DP(MISS) == 0 && PP(USED) == 1 && PP(PIN0) == 0 && PP(PIN1) == 1);
    CHECK(PP(PINNED_BYTES) == sheet && isaac_ir_oracle_retained_bytes() <= (8U << 20));
#else
    CHECK(count_unreg_of(a) == 1 && count_unreg_of(o1) == 0 && manager_present(a) == 0);
    CHECK(manager_present(b) == 1 && DP(EVICTED_UNUSED) == 1 && PP(SKIP) == 0);
#endif

    /* Q3. The exact key is present and the game holds it (strong 2).  ON's
     *     replay is a strcmp hit on it (`exact`, nothing decoded) and marks
     *     it held; when the room lets go, the retention seam records it and
     *     it is pinned (`on_free`); pressure walks past it; the fight's
     *     Load hits it.  OBSERVE / OFF: the same release is a miss. */
    p_begin("pin-held-freed-reloaded", 8, 0);
    a = mk(va(ISAAC_IR_VT_PNG_RVA), "resources/gfx/BulletAtlas.png", 1024, 512, 4, 2, &ac);
    level(&s_cpu);
    CHECK(DP(PRESENT) == 1 && ld16(ac + ISAAC_IR_CTRL_STRONG) == 2 && manager_present(a) == 1);
    CHECK(pin_has(" strong=2 flags=00000000 bytes=2097152 rec=0 own=0 key=resources/gfx/BulletAtlas.png"));
#if PIN_ON
    CHECK(s_load_calls == 2 && PP(EXACT) == 1 && PP(ALIAS) == 0 && PP(HELD) == 1);
    CHECK(PP(STATE0) == PIN_STATE_ON_FREE && PP(PIN0) == 0 && PP(PIN1) == 1);
    CHECK(DP(DROPPED) == 0 && DP(RETAINED) == 1 && DP(LOADED) == 2);
    CHECK(pin_has("KAGE VITA IMAGE PRELOAD: image name=gfx/BulletAtlas.png result=held image="));
#else
    CHECK(s_load_calls == 1 && PP(EXACT) == 0 && PP(HELD) == 0 && PP(STATE0) == 0);
#endif
    sprite_dies(a, ac);                          /* the room's last holder lets go */
    CHECK(isaac_ir_oracle_live_records() == 2 && manager_present(a) == 1);
#if PIN_ON
    CHECK(PP(ON_FREE) == 1 && PP(PIN0) == 1 && PP(STATE0) == PIN_STATE_WAITING);
    CHECK(PP(LOST) == 0 && DP(MISS) == 0 && PP(PINNED_BYTES) == 2U * sheet);
#else
    CHECK(DP(MISS) == 1 && PP(ON_FREE) == 0);
#endif
    o1 = pin_others(3);
    CHECK(isaac_ir_oracle_retained_bytes() <= (8U << 20));
#if PIN_ON
    CHECK(count_unreg_of(o1) == 1 && count_unreg_of(a) == 0 && PP(SKIP) == 2);
    CHECK(manager_present(a) == 1 && DP(EVICTED_UNUSED) == 0);
    load_hit(ac);                                /* the fight's Load: no decode */
    sprite_dies(a, ac);
    CHECK(DP(HIT) == 1 && PP(USED) == 1 && PP(PIN0) == 0 && s_load_calls == 2);
#else
    CHECK(DP(EVICTED_UNUSED) == 1 && manager_present(a) == 1 && count_unreg_of(o1) == 0);
    load_hit(ac);
    sprite_dies(a, ac);
    CHECK(DP(HIT) == 0 && DP(MISS) == 2 && s_load_calls == 1);
#endif

    /* Q4. No pin outlives its floor: the next Level::Init releases every pin
     *     before its walk (here the walk cannot run: manager Mutex held), and
     *     the LRU is free to take the unused sheets again. */
    p_begin("pin-floor-release", 8, 0);
    level(&s_cpu);
    a = last_image(2);
    CHECK(PP(PINS) == (PIN_ON ? 2U : 0U) && PP(PINNED_BYTES) == (PIN_ON ? 2U * sheet : 0U));
    st8(va(OFF_MGR_CS) + ISAAC_IR_CS_HELD, 1U);
    level(&s_cpu);
    st8(va(OFF_MGR_CS) + ISAAC_IR_CS_HELD, 0U);
    CHECK(DP(SKIP_LOCK) == 2 && PP(PINS) == 0 && PP(PINNED_BYTES) == 0);
    CHECK(PP(FLOOR) == (PIN_ON ? 2U : 0U));
    CHECK(PP(STATE0) == PIN_STATE_WAITING && PP(STATE1) == PIN_STATE_WAITING);
    CHECK(pin_has(PIN_MODE "pins=0 pinned_bytes=0 share=4194304 released="));
#if PIN_ON
    CHECK(pin_has(" mode=on pins=0 pinned_bytes=0 share=4194304 released=2 pinned=0 own=0 exact=0 alias=0 held=0"));
#endif
    o1 = pin_others(3);
    CHECK(count_unreg_of(a) == 1 && count_unreg_of(o1) == 0 && manager_present(a) == 0);
    CHECK(DP(EVICTED_UNUSED) == 1 && PP(SKIP) == 0 && isaac_ir_oracle_retained_bytes() <= (8U << 20));

    /* Q5. The sheet an earlier floor left retained and unused is present
     *     as itself (own=1 in both modes); ON re-pins it without a Load. */
    p_begin("pin-own-repin", 12, 0);
    level(&s_cpu);
    level(&s_cpu);
    CHECK(s_load_calls == 2 && DP(PRESENT) == 2 && DP(LOADED) == 2);
    CHECK(pin_has(" rec=1 own=1 key=resources/gfx/BulletAtlas.png"));
    CHECK(pin_has(" rec=1 own=1 key=resources/gfx/b.png"));
#if PIN_ON
    CHECK(PP(OWN) == 2 && PP(FLOOR) == 2 && PP(PINNED) == 4 && PP(PINS) == 2);
    CHECK(PP(PINNED_BYTES) == 2U * sheet && PP(EXACT) == 0 && PP(ALIAS) == 0);
    CHECK(pin_has(" mode=on pins=2 pinned_bytes=4194304 share=6291456 released=2 pinned=2 own=2 exact=0 alias=0 held=0"));
#else
    CHECK(PP(OWN) == 0 && PP(PINNED) == 0 && PP(PINS) == 0);
#endif

    /* Q6. A pin never blocks an eviction: free memory under the headroom,
     *     every record pinned -> the oldest pin yields and goes. */
    p_begin("pin-yield-headroom", 12, 16);
    level(&s_cpu);
    a = last_image(2); b = last_image(1);
    CHECK(DP(RETAINED) == 2 && PP(PINS) == (PIN_ON ? 2U : 0U));
    s_free_ram = 15U << 20; s_free_vram = 0U;    /* 15 < 16 */
    s = mk(va(ISAAC_IR_VT_PNG_RVA), "small.png", 64, 64, 4, 1, &sc);
    offer(s, sc);
    CHECK(count_unreg_of(a) == 1 && count_unreg_of(b) == 0 && manager_present(b) == 1);
    CHECK(DP(EVICTED_UNUSED) == 1 && st()->headroom_evict >= 1U);
#if PIN_ON
    CHECK(PP(YIELD) == 1 && PP(SKIP) == 2 && PP(PIN0) == 0 && PP(PIN1) == 1);
    CHECK(PP(PINNED_BYTES) == sheet && PP(DROPPED) == 0);
#else
    CHECK(PP(YIELD) == 0 && PP(SKIP) == 0);
#endif

    /* Q7. A pinned sheet Unregistered behind our back (a foreign Clear):
     *     the pin is released as `stale` and the record dropped without a
     *     guest Unregister; the budget still holds. */
    p_begin("pin-stale", 8, 0);
    level(&s_cpu);
    a = last_image(2);
    do_unregister(&s_cpu, 0x5bfc46U, a);
    CHECK(manager_present(a) == 0 && PP(PINS) == (PIN_ON ? 2U : 0U));
    o1 = pin_others(3);
    CHECK(count_unreg_of(a) == 0 && count_unreg_of(o1) == 0 && PP(DROPPED) == 0);
    CHECK(PP(STALE) == (PIN_ON ? 1U : 0U) && PP(PINS) == (PIN_ON ? 1U : 0U));
    CHECK(isaac_ir_oracle_retained_bytes() == 4U * sheet);

    /* Q7b. ... and its address reused by a new Image before any eviction
     *      looked: the retention seam drops the dead record on the new
     *      offer, and the pin goes with it (`dropped`). */
    p_begin("pin-dropped-address-reused", 8, 0);
    level(&s_cpu);
    a = last_image(2);
    do_unregister(&s_cpu, 0x5bfc46U, a);
    {
        const uint32_t keep_n = s_img_n;

        s_img_n = (a - va(OFF_IMG)) / IMG_STRIDE;
        s = mk(va(ISAAC_IR_VT_PNG_RVA), "reused.png", 64, 64, 4, 1, &sc);
        s_img_n = keep_n;
    }
    CHECK(s == a && manager_present(a) == 1);
    offer(s, sc);
    CHECK(PP(DROPPED) == (PIN_ON ? 1U : 0U) && PP(STALE) == 0);
    CHECK(PP(PINS) == (PIN_ON ? 1U : 0U) && PP(PINNED_BYTES) == (PIN_ON ? sheet : 0U));
    CHECK(isaac_ir_oracle_live_records() == 2 && isaac_ir_oracle_retained_bytes() == sheet + 64U * 64U * 4U);

    /* Q8. The held exact sheet carries flag 0x10 (drawn: on the TOUCHED
     *     flush list).  Without ADMIT_TOUCHED ON the retention seam refuses
     *     it, the observer frees it, and the next offer settles it as `lost`
     *     (Load will decode it again); with ADMIT_TOUCHED ON it is admitted
     *     and pinned like Q3. */
    p_begin("pin-held-lost", 8, 0);
    a = mk(va(ISAAC_IR_VT_PNG_RVA), "resources/gfx/BulletAtlas.png", 1024, 512, 4, 2, &ac);
#if defined(IR_ORACLE_FLUSH)
    touched_add(a);
#else
    st32(a + ISAAC_IR_IMG_FLAGS, 0x10U);
#endif
    level(&s_cpu);
    CHECK(pin_has(" strong=2 flags=00000010 bytes=2097152 rec=0 own=0 key=resources/gfx/BulletAtlas.png"));
    sprite_dies(a, ac);
    CHECK(PP(LOST) == 0);
    s = mk(va(ISAAC_IR_VT_PNG_RVA), "small.png", 64, 64, 4, 1, &sc);
    offer(s, sc);
#if PIN_ON && defined(IR_ORACLE_FLUSH) && IRT_ON
    CHECK(PP(HELD) == 1 && PP(ON_FREE) == 1 && PP(LOST) == 0 && PP(PIN0) == 1);
    CHECK(manager_present(a) == 1 && PP(STATE0) == PIN_STATE_WAITING && DP(MISS) == 0);
#elif PIN_ON
    CHECK(PP(HELD) == 1 && PP(LOST) == 1 && PP(ON_FREE) == 0 && PP(PIN0) == 0);
    CHECK(manager_present(a) == 0 && PP(STATE0) == 0 && DP(MISS) == 0);
    CHECK(pin_has("KAGE VITA IMAGE PRELOAD PIN: lost name=gfx/BulletAtlas.png level="));
    CHECK(pin_has(" flags=00000010 lost=1 exit=ineligible"));
#else
    CHECK(PP(LOST) == 0 && PP(HELD) == 0 && DP(MISS) == 1);
#endif

    /* Q9. Q3's held sheet, but the room's last holder lets go while the
     *     manager Mutex is held: ir_keep keeps it at the LRU tail (irp_offer
     *     does not run).  ON still pins it there (review F-BA item 4);
     *     nothing is `lost` in either mode, the sheet is the manager's. */
    p_begin("pin-held-freed-under-lock", 8, 0);
    a = mk(va(ISAAC_IR_VT_PNG_RVA), "resources/gfx/BulletAtlas.png", 1024, 512, 4, 2, &ac);
    level(&s_cpu);
#if PIN_ON
    CHECK(PP(HELD) == 1 && PP(STATE0) == PIN_STATE_ON_FREE && PP(PIN0) == 0 && PP(PIN1) == 1);
#endif
    st8(va(OFF_MGR_CS) + ISAAC_IR_CS_HELD, 1U);
    sprite_dies(a, ac);
    st8(va(OFF_MGR_CS) + ISAAC_IR_CS_HELD, 0U);
    CHECK(manager_present(a) == 1 && count_unreg_of(a) == 0 && isaac_ir_oracle_live_records() == 2);
    CHECK(PP(LOST) == 0 && DP(MISS) == 0 && st()->held_keep == 1);
#if PIN_ON
    CHECK(PP(ON_FREE) == 1 && PP(PIN0) == 1 && PP(STATE0) == PIN_STATE_WAITING);
    CHECK(PP(PINNED_BYTES) == 2U * sheet);
#else
    CHECK(PP(ON_FREE) == 0 && PP(PIN0) == 0);
#endif
    o1 = pin_others(3);
    CHECK(isaac_ir_oracle_retained_bytes() <= (8U << 20) && PP(LOST) == 0);
#if PIN_ON
    CHECK(count_unreg_of(o1) == 1 && count_unreg_of(a) == 0 && manager_present(a) == 1);
    CHECK(PP(SKIP) == 2 && PP(YIELD) == 0);
#else
    CHECK(count_unreg_of(a) == 1 && count_unreg_of(o1) == 0 && manager_present(a) == 0);
#endif

    /* Q10. The held sheet's release is taken back between the wrapper's
     *      unlocked pre-check and its lock replay (the observer's
     *      `reacquired` no-op): the sheet is still the game's, so it is not
     *      `lost` and stays held; the release that follows pins it (review
     *      F-BA item 5).  OBSERVE / OFF: two misses, as before. */
    p_begin("pin-held-reacquired", 8, 0);
    a = mk(va(ISAAC_IR_VT_PNG_RVA), "resources/gfx/BulletAtlas.png", 1024, 512, 4, 2, &ac);
    level(&s_cpu);
    s_race_bump = 1;
    sprite_dies(a, ac);
    CHECK(s_race_bump == 0 && ld16(ac + ISAAC_IR_CTRL_STRONG) == 2 && manager_present(a) == 1);
    CHECK(count_unreg_of(a) == 0 && isaac_ir_oracle_live_records() == 1);
    s = mk(va(ISAAC_IR_VT_PNG_RVA), "small.png", 64, 64, 4, 1, &sc);
    offer(s, sc);
    CHECK(PP(LOST) == 0 && PP(ON_FREE) == 0);
#if PIN_ON
    CHECK(PP(HELD) == 1 && PP(STATE0) == PIN_STATE_ON_FREE && PP(PIN0) == 0 && DP(MISS) == 0);
#else
    CHECK(DP(MISS) == 1);
#endif
    sprite_dies(a, ac);
    CHECK(manager_present(a) == 1 && isaac_ir_oracle_live_records() == 3 && PP(LOST) == 0);
#if PIN_ON
    CHECK(PP(ON_FREE) == 1 && PP(PIN0) == 1 && PP(STATE0) == PIN_STATE_WAITING && DP(MISS) == 0);
    CHECK(PP(PINNED_BYTES) == 2U * sheet);
#else
    CHECK(PP(ON_FREE) == 0 && PP(PIN0) == 0 && DP(MISS) == 2);
#endif

    /* R1. A retained sheet (x) the game held is let go under headroom
     *     pressure while every other record is pinned (review F-BA item 1).
     *     ir_wrap moves x's record to the head and releases tails; the walk
     *     must not take x's own record (that Unregisters the Image the
     *     observer's caller holds and leaves a stale index): the oldest pin
     *     yields, as OFF's tail walk takes the tail.  Immediate frees: one
     *     tail recovers the headroom and x is retained. */
    p_begin("pin-candidate-own-record-headroom", 12, 16);
    level(&s_cpu);
    a = last_image(2); b = last_image(1);
    CHECK(PP(PINS) == (PIN_ON ? 2U : 0U));
    s = mk(va(ISAAC_IR_VT_PNG_RVA), "x.png", 64, 64, 4, 1, &sc);
    offer(s, sc);
    CHECK(isaac_ir_oracle_live_records() == 3);
    load_hit(sc);
    s_free_ram = 15U << 20; s_free_vram = 0U;    /* 15 < 16 */
    sprite_dies(s, sc);
    CHECK(count_unreg_of(s) == 0 && count_unreg_of(a) == 1 && count_unreg_of(b) == 0);
    CHECK(manager_present(s) == 1 && manager_present(b) == 1 && manager_present(a) == 0);
    CHECK(isaac_ir_oracle_live_records() == 2 && isaac_ir_oracle_retained_bytes() == sheet + 64U * 64U * 4U);
    CHECK(st()->headroom_pass == 0 && DP(EVICTED_UNUSED) == 1);
#if PIN_ON
    CHECK(PP(YIELD) == 1 && PP(CAND_SKIP) == 1 && PP(SKIP) == 2);
    CHECK(PP(PINS) == 1 && PP(PIN0) == 0 && PP(PIN1) == 1 && PP(PINNED_BYTES) == sheet);
#else
    CHECK(PP(YIELD) == 0 && PP(CAND_SKIP) == 0 && PP(SKIP) == 0);
#endif

    /* R1b. ... and vitaGL defers the frees (s_deferred_free): no tail
     *      recovers the headroom.  Both pins yield in turn, x's record is
     *      forgotten and the real observer Unregisters x last, exactly as
     *      OFF: the table ends empty and consistent. */
    p_begin("pin-candidate-own-record-deferred", 12, 16);
    level(&s_cpu);
    a = last_image(2); b = last_image(1);
    s = mk(va(ISAAC_IR_VT_PNG_RVA), "x.png", 64, 64, 4, 1, &sc);
    offer(s, sc);
    load_hit(sc);
    s_free_ram = 15U << 20; s_free_vram = 0U;
    s_deferred_free = 1;
    sprite_dies(s, sc);
    CHECK(ev_count(EV_UNREG) == 3 && pin_unreg_nth(0) == a && pin_unreg_nth(1) == b && pin_unreg_nth(2) == s);
    CHECK(manager_present(s) == 0 && manager_present(b) == 0 && manager_present(a) == 0);
    CHECK(isaac_ir_oracle_live_records() == 0 && isaac_ir_oracle_retained_bytes() == 0);
    CHECK(st()->headroom_pass == 1 && st()->headroom_evict == 3);
#if PIN_ON
    CHECK(PP(YIELD) == 2 && PP(CAND_SKIP) == 2 && PP(SKIP) == 3 && PP(PINS) == 0);
#else
    CHECK(PP(YIELD) == 0 && PP(CAND_SKIP) == 0);
#endif

    /* R2. Budget path (review F-BA item 2): a 6 MiB sheet the game let go
     *     of is recorded at the head and the table is 2 MiB over budget
     *     while both 2 MiB pins hold.  The walk must not evict the sheet it
     *     just recorded while a pin can yield: the oldest pin yields, OFF's
     *     choice.  (The sheet's own release stays the last resort of a
     *     budget eviction; touched-rule-e's on+pin leg reaches it.) */
    p_begin("pin-candidate-budget", 8, 0);
    level(&s_cpu);
    a = last_image(2); b = last_image(1);
    CHECK(PP(PINS) == (PIN_ON ? 2U : 0U));
    s = mk(va(ISAAC_IR_VT_PNG_RVA), "big.png", 1024, 1536, 4, 1, &sc);
    offer(s, sc);
    CHECK(count_unreg_of(s) == 0 && count_unreg_of(a) == 1 && count_unreg_of(b) == 0);
    CHECK(manager_present(s) == 1 && manager_present(b) == 1);
    CHECK(isaac_ir_oracle_live_records() == 2 && isaac_ir_oracle_retained_bytes() == (8U << 20));
#if PIN_ON
    CHECK(PP(YIELD) == 1 && PP(CAND_SKIP) == 1 && PP(PINS) == 1 && PP(PIN1) == 1);
#else
    CHECK(PP(YIELD) == 0 && PP(CAND_SKIP) == 0);
#endif

#if PIN_ON && defined(IR_ORACLE_FLUSH) && IRT_ON
    /* R3. R1 with ADMIT_TOUCHED ON and no pin at all: the record older than
     *     x is rule E's (a touched sheet on a flush list), which no eviction
     *     may take and which cannot yield.  The headroom walk passes x's own
     *     record and finds nothing (evict_blocked); x's record is forgotten
     *     and the real observer frees x, as OFF.  (The IMG lane's own walker,
     *     used without PIN ON, has no barrier: review F-BA item 3.) */
    p_begin("pin-candidate-rule-e-headroom", 12, 16);
    a = mk_touched("t9.png", &ac);
    offer(a, ac);
    s = mk(va(ISAAC_IR_VT_PNG_RVA), "x.png", 64, 64, 4, 1, &sc);
    offer(s, sc);
    CHECK(isaac_ir_oracle_live_records() == 2 && manager_present(a) == 1 && manager_present(s) == 1);
    load_hit(sc);
    s_free_ram = 15U << 20; s_free_vram = 0U;    /* 15 < 16 */
    sprite_dies(s, sc);
    CHECK(count_unreg_of(a) == 0 && manager_present(a) == 1 && fv_count(FV_TOUCHED, a) == 1);
    CHECK(count_unreg_of(s) == 1 && manager_present(s) == 0 && ev_count(EV_UNREG) == 1);
    CHECK(isaac_ir_oracle_live_records() == 1 && isaac_ir_oracle_retained_bytes() == 512U * 512U * 4U);
    CHECK(st()->headroom_pass == 1 && st()->headroom_evict == 1);
    CHECK(PP(CAND_SKIP) == 1 && PP(YIELD) == 0 && PP(PINS) == 0);
    CHECK(IRT(PINNED_SKIP) == 1 && IRT(EVICT_BLOCKED) == 1 && IRT(BUDGET_REFUSE) == 0);
#endif
}
#endif

static void preload_scenarios(void)
{
    uint32_t a, ac, b, bc;
    const uint32_t sheet = 1024U * 512U * 4U;

    /* P1. Both list sheets are decoded at floor load and end up retained:
     *     strong 1 (the manager's), a record each, the caller's stack back. */
    p_begin("preload-retained", 12, 0);
    CHECK(IRP(ENTRIES) == 2);
    level(&s_cpu);
    CHECK(s_load_calls == 2 && s_load_bad_args == 0 && s_release_calls == 2 && s_hook_calls == 2);
    CHECK(s_deadlock == 0 && s_fault_hit == 0 && s_unknown_targets == 0 && ev_count(EV_UNREG) == 0);
    CHECK(DP(LEVELS) == 1 && DP(LOADED) == 2 && DP(RETAINED) == 2 && DP(DROPPED) == 0);
    CHECK(DP(FAIL) == 0 && DP(PRESENT) == 0 && DP(HIT) == 0 && DP(MISS) == 0 && DP(BREACH) == 0);
    CHECK(DP(LOADED_BYTES) == 2U * sheet && isaac_ir_oracle_live_records() == 2);
    CHECK(isaac_ir_oracle_retained_bytes() == 2U * sheet && manager_count() == 2);
    CHECK(strncmp(s_last_log, "KAGE VITA IMAGE PRELOAD: level=", 31) == 0);
    CHECK(strstr(s_last_log, " n=2 loaded=2 retained=2 dropped=0 present=0 fail=0 skipped=0 bytes=4194304 ") != NULL);
    CHECK(strstr(s_last_log, " hr_kb=20480..20480" P_NEED_TEXT "fault=0") != NULL);
    a = last_image(2); ac = last_ctrl(2);
    b = last_image(1); bc = last_ctrl(1);
    CHECK(manager_present(a) == 1 && ld16(ac + ISAAC_IR_CTRL_STRONG) == 1);
    CHECK(manager_present(b) == 1 && ld16(bc + ISAAC_IR_CTRL_STRONG) == 1);
    CHECK(guest_str_eq(ld32(a + ISAAC_IR_IMG_NAME), "resources/gfx/BulletAtlas.png"));
    CHECK(guest_str_eq(ld32(b + ISAAC_IR_IMG_NAME), "resources/gfx/b.png"));
    /* the next floor finds them in the manager: no Load */
    level(&s_cpu);
    CHECK(DP(LEVELS) == 2 && DP(PRESENT) == 2 && DP(LOADED) == 2 && s_load_calls == 2);
    CHECK(strstr(s_last_log, " n=2 loaded=0 retained=0 dropped=0 present=2 fail=0 skipped=0 bytes=0 ") != NULL);
    /* the fight takes the sheet from the manager (no decode) and lets go:
     * one hit per preloaded sheet, however often it is reused afterwards */
    load_hit(ac);
    sprite_dies(a, ac);
    CHECK(retained_events_only() && DP(HIT) == 1 && DP(MISS) == 0 && st()->reuse == 1);
    load_hit(ac);
    sprite_dies(a, ac);
    CHECK(DP(HIT) == 1 && DP(MISS) == 0 && st()->reuse == 2);

    /* P2. A list sheet the game decoded itself is a miss; the name match is
     *     a whole path component, ASCII case and slash direction folded. */
    p_begin("preload-miss", 12, 0);
    a = mk(va(ISAAC_IR_VT_PNG_RVA), "resources/gfx/b.png", 64, 64, 4, 1, &ac);
    offer(a, ac);
    CHECK(DP(MISS) == 1 && DP(HIT) == 0);
    a = mk(va(ISAAC_IR_VT_PNG_RVA), "resources/gfx/xb.png", 64, 64, 4, 1, &ac);
    offer(a, ac);
    a = mk(va(ISAAC_IR_VT_PNG_RVA), "xgfx/b.png", 64, 64, 4, 1, &ac);
    offer(a, ac);
    a = mk(va(ISAAC_IR_VT_PNG_RVA), "fx/b.png", 64, 64, 4, 1, &ac);
    offer(a, ac);
    CHECK(DP(MISS) == 1);
    a = mk(va(ISAAC_IR_VT_PNG_RVA), "RESOURCES\\GFX\\B.PNG", 64, 64, 4, 1, &ac);
    offer(a, ac);
    a = mk(va(ISAAC_IR_VT_PNG_RVA), "gfx/b.png", 64, 64, 4, 1, &ac);
    offer(a, ac);
    CHECK(DP(MISS) == 3 && DP(HIT) == 0 && DP(LEVELS) == 0);

    /* P3. The retention seam refuses the sheet (quads queued): the real
     *     observer frees it again -- dropped, nothing leaks. */
    p_begin("preload-dropped", 12, 0);
    s_load_flags = 0x05U;
    level(&s_cpu);
    CHECK(DP(LOADED) == 2 && DP(DROPPED) == 2 && DP(RETAINED) == 0 && DP(MISS) == 0);
    CHECK(manager_count() == 0 && isaac_ir_oracle_live_records() == 0 && ev_count(EV_DESTROY) == 2);
    CHECK(strstr(s_last_log, " n=2 loaded=2 retained=0 dropped=2 present=0 fail=0 ") != NULL);

    /* P4. Load produced no Image (file missing): nothing to release. */
    p_begin("preload-fail", 12, 0);
    s_load_fail = 1;
    level(&s_cpu);
    CHECK(s_load_calls == 2 && s_release_calls == 0 && s_hook_calls == 0);
    CHECK(DP(FAIL) == 2 && DP(LOADED) == 0 && DP(RETAINED) == 0 && manager_count() == 0);

    /* P4b. Release reports "last reference gone" (al = 0 under garbage upper
     *      bytes): the Image is already destroyed, the observer must not see it. */
    p_begin("preload-release-last", 12, 0);
    s_load_orphan = 1;
    level(&s_cpu);
    CHECK(s_load_calls == 2 && s_release_calls == 2 && s_hook_calls == 0 && s_load_bad_args == 0);
    CHECK(DP(LOADED) == 2 && DP(DROPPED) == 2 && DP(RETAINED) == 0 && ev_count(EV_DESTROY) == 2);
    CHECK(isaac_ir_oracle_live_records() == 0 && s_fault_hit == 0);

    /* P5. The gates, one at a time; none of them reaches Load. */
    p_begin("preload-skip-budget0", 0, 0);
    level(&s_cpu);
    CHECK(DP(SKIP_STATE) == 2 && s_load_calls == 0);
    p_begin("preload-skip-lock", 12, 0);
    st8(va(OFF_MGR_CS) + ISAAC_IR_CS_HELD, 1U);
    level(&s_cpu);
    st8(va(OFF_MGR_CS) + ISAAC_IR_CS_HELD, 0U);
    CHECK(DP(SKIP_LOCK) == 2 && s_load_calls == 0 && s_deadlock == 0);
    p_begin("preload-skip-terminal", 12, 0);
    s_heap_terminal = 1;
    level(&s_cpu);
    CHECK(DP(SKIP_TERMINAL) == 2 && s_load_calls == 0);
    p_begin("preload-skip-vmem-budget", 1, 0);
    level(&s_cpu);
    CHECK(DP(SKIP_VMEM) == 2 && s_load_calls == 0);
    p_begin("preload-skip-vmem-headroom", 12, 16);
    s_free_ram = 17U << 20; s_free_vram = 0U;            /* 17 < 16 + 2 */
    level(&s_cpu);
    CHECK(DP(SKIP_VMEM) == 2 && s_load_calls == 0);
    p_begin("preload-skip-heap", 12, 0);
    s_heap_hr = (P_NEED_KIB << 10) - 1U;                 /* reserve + 2 x 2 MiB, one byte short */
    level(&s_cpu);
    CHECK(DP(SKIP_HEAP) == 2 && s_load_calls == 0);
    CHECK(strstr(s_last_log, " loaded=0 retained=0 dropped=0 present=0 fail=0 skipped=2 ") != NULL);
    CHECK(strstr(s_last_log, P_NEED_TEXT) != NULL);      /* the level line names the gate */
    s_heap_hr = P_NEED_KIB << 10;                        /* exactly enough */
    level(&s_cpu);
    CHECK(DP(SKIP_HEAP) == 2 && DP(LOADED) == 2 && DP(RETAINED) == 2 && s_load_calls == 2);
    /* ... and a call on another CPU than the retention seam's */
    cpu_bind(&s_cpu_foreign);
    isaac_vita_image_preload_level_init(&s_cpu_foreign);
    CHECK(DP(SKIP_STATE) == 2 && s_load_calls == 2);
    /* The bench route (review B1).  Guest heap headroom in KiB as the device
     * logged it, native.log of bench run H2 (11.11): :291 level 1
     * before the fight sounds 14427, :315 after them 11807, :379 level 2
     * 11651, :4306 level 3 7484; and the extremes of 19 fight-ON runs after
     * the fight sounds (img.review.md:33): level 1 11469..11849, level 2
     * 11323..11691.  The preload reads the headroom after the fight sounds,
     * so RESERVE_MB 8 decodes on none of the three floors and 4 on the first
     * two.  The expectations are a table, not the gate's formula. */
    {
        static const struct { uint32_t kib; int run8, run4; } route[] = {
            { 14427U, 1, 1 }, { 11807U, 0, 1 }, { 11651U, 0, 1 }, { 7484U, 0, 0 },
            { 11469U, 0, 1 }, { 11849U, 0, 1 }, { 11323U, 0, 1 }, { 11691U, 0, 1 },
        };
        unsigned r;
        for (r = 0; r < sizeof route / sizeof route[0]; ++r) {
            p_begin("preload-heap-gate-bench-route", 12, 0);
            s_heap_hr = route[r].kib << 10;
            level(&s_cpu);
            if (P_ROUTE_RUN(route[r]))
                CHECK(DP(SKIP_HEAP) == 0 && DP(LOADED) == 2 && DP(RETAINED) == 2 && s_load_calls == 2);
            else
                CHECK(DP(SKIP_HEAP) == 2 && DP(LOADED) == 0 && s_load_calls == 0);
            CHECK(strstr(s_last_log, P_NEED_TEXT) != NULL);
        }
    }
    p_begin("preload-skip-stack", 12, 0);
    cpu_bind(&s_cpu);
    s_cpu.esp = s_cpu.stack_floor + 0x40U;
    isaac_vita_image_preload_level_init(&s_cpu);
    CHECK(s_cpu.esp == s_cpu.stack_floor + 0x40U && DP(SKIP_STACK) == 2 && s_load_calls == 0);

    /* P6. A replay that does not honour its stack contract stops the walk
     *     with the caller's ESP restored. */
    p_begin("preload-breach-load", 12, 0);
    s_load_break_esp = 1;
    level(&s_cpu);
    CHECK(DP(BREACH) == 1 && s_load_calls == 1 && s_release_calls == 0 && DP(LOADED) == 0);
    p_begin("preload-breach-release", 12, 0);
    s_release_break_esp = 1;
    level(&s_cpu);
    CHECK(DP(BREACH) == 1 && s_load_calls == 1 && s_release_calls == 1 && s_hook_calls == 0);
    p_begin("preload-breach-observer", 12, 0);
    s_hook_break_esp = 1;
    level(&s_cpu);
    CHECK(DP(BREACH) == 1 && s_load_calls == 1 && s_hook_calls == 1 && DP(LOADED) == 0);

    /* P7. The LRU may evict a preloaded sheet before anything used it: that
     *     is counted, and the game's own later decode is a miss. */
    p_begin("preload-evicted-unused", 4, 0);
    level(&s_cpu);
    CHECK(DP(RETAINED) == 2 && isaac_ir_oracle_retained_bytes() == 2U * sheet);
    a = last_image(2);
#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN) && ISAAC_VITA_IMAGE_PRELOAD_PIN == 2
    {
        /* PIN ON: the share cap (budget 4 MiB / 2) pins the first sheet
         * only; the eviction walks past it and takes the second one. */
        const uint32_t second = last_image(1);

        CHECK(PP(PINNED) == 1 && PP(REFUSED) == 1 && PP(PIN0) == 1 && PP(PIN1) == 0);
        b = mk(va(ISAAC_IR_VT_PNG_RVA), "other.png", 1024, 512, 4, 1, &bc);
        offer(b, bc);
        CHECK(count_unreg_of(second) == 1 && manager_present(a) == 1 && DP(EVICTED_UNUSED) == 1);
        CHECK(PP(SKIP) == 1 && PP(YIELD) == 0 && isaac_ir_oracle_retained_bytes() == 2U * sheet);
        /* a second copy of the pinned key, registered by some other Load:
         * a miss, and the receipt says the name is in the manager twice */
        a = mk(va(ISAAC_IR_VT_PNG_RVA), "resources/gfx/BulletAtlas.png", 1024, 512, 4, 1, &ac);
        offer(a, ac);
        CHECK(DP(MISS) == 1 && DP(HIT) == 0 && PP(TWINS) == 1 && PP(PIN0) == 1);
        CHECK(pin_has("KAGE VITA IMAGE PRELOAD PIN: miss name=gfx/BulletAtlas.png level="));
        CHECK(pin_has(" state=1 pinned=1 twins=2 key=resources/gfx/BulletAtlas.png"));
        CHECK(isaac_ir_oracle_retained_bytes() <= (4U << 20));
    }
#else
    b = mk(va(ISAAC_IR_VT_PNG_RVA), "other.png", 1024, 512, 4, 1, &bc);
    offer(b, bc);
    CHECK(count_unreg_of(a) == 1 && manager_present(a) == 0 && DP(EVICTED_UNUSED) == 1);
    a = mk(va(ISAAC_IR_VT_PNG_RVA), "resources/gfx/BulletAtlas.png", 1024, 512, 4, 1, &ac);
    offer(a, ac);
    CHECK(DP(MISS) == 1 && DP(HIT) == 0);
#endif
#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN)
    pin_scenarios();
#endif

    /* P8. The receipts fit isaac_vita_log's 384-byte body at u32 max. */
    p_begin("preload-receipt-length", 12, 0);
    isaac_ir_oracle_saturate_stats();
    s_max_log_len = 0; s_totals_seen = 0;
#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN)
    s_pin_totals_seen = 0;
#endif
    isaac_vita_image_retain_report("loading-complete");
    CHECK(s_max_log_len < 384U);
    CHECK(s_totals_seen == 1);
#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN)
    CHECK(s_pin_totals_seen == 1);
#endif
    isaac_vita_image_retain_disarm();
}
#endif

#if defined(ISAAC_VITA_IMAGE_RETAIN_BIG_TRACE)
/* ------------------------------------------------------------ big trace --- */
/* ISAAC_VITA_IMAGE_RETAIN_BIG_TRACE (built with MIN_KB=1024): one line per
 * decision about a sheet of >= 1 MiB, per-reason ineligible totals.  The
 * scenarios above already ran with the trace compiled in, so the trace is
 * also shown not to change any retain/evict/passthrough outcome. */
#if defined(IR_ORACLE_FLUSH) && IRT_ON
#define BIG_INEL_DIRTY " what=inel-dirty "
#else
#define BIG_INEL_DIRTY " what=inel-rt "
#endif
static int big_has(unsigned i, const char *needle)
{
    return i < BIG_KEEP && i < s_big_n && strstr(s_big[i], needle) != NULL;
}

static const char *big_name_of(unsigned i)
{
    const char *p = (i < BIG_KEEP && i < s_big_n) ? strstr(s_big[i], " name=") : NULL;
    return p ? p + 6 : "<no name field>";
}

/* A name longer than the name arena's 0x20 stride: takes nine slots. */
static void big_set_name(uint32_t image, const char *name)
{
    uint32_t p = va(OFF_NAME) + s_name_n * 0x20U;
    s_name_n += 9U;
    memcpy((char *)(uintptr_t)p, name, strlen(name) + 1U);
    st32(image + ISAAC_IR_IMG_NAME, p);
}

static void big_totals(unsigned t[8])
{
    unsigned n, k;
    for (k = 0; k < 8U; ++k) t[k] = 0xdeadU;
    big_reset();
    isaac_vita_image_retain_report("big");
    n = s_big_n;
    CHECK(n == 1U);
    {
        static const char head[] =
            "KAGE VITA IMAGE RETAIN BIG TOTALS: inel(vt/rt/tex/name/bytes)=";
        static const char *const key[3] = { " last_foreign_vt=", " lines=", " suppressed=" };
        const char *q = s_big[0] + sizeof head - 1U;
        char *end = NULL;
        int ok = strncmp(s_big[0], head, sizeof head - 1U) == 0;

        for (k = 0; ok && k < 5U; ++k) {
            t[k] = (unsigned)strtoul(q, &end, 10);
            ok = end != q && *end == (k < 4U ? '/' : ' ');
            q = end + 1;
        }
        for (k = 0; ok && k < 3U; ++k) {
            q = strstr(s_big[0], key[k]);
            ok = q != NULL;
            if (ok) {
                q += strlen(key[k]);
                t[5U + k] = (unsigned)strtoul(q, &end, k == 0U ? 16 : 10);
                ok = end != q && *end == ' ';
            }
        }
        CHECK(ok);
    }
    CHECK(strstr(s_big[0], " min_bytes=1048576 why=big") != NULL);
    CHECK(strlen(s_big[0]) < 384U);
}

static void big_trace_scenarios(void)
{
    uint32_t image, ctrl, image2, ctrl2, image3, ctrl3, i;
    unsigned t[8];
    char line[160];

    /* B1. Banner with the resolved threshold; a new big sheet; the size
     *     boundary (1 MiB reports, one row less does not). */
    big_reset();
    begin("big-new-and-threshold", 12, 0);
    CHECK(s_big_n == 1U && big_has(0, "KAGE VITA IMAGE RETAIN BIG TRACE: on min_bytes=1048576 max_lines=512 "));
    big_reset();
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "gfx/big_sheet.png", 1024, 512, 4, 1, &ctrl);
    offer(image, ctrl);
    CHECK(s_big_n == 1U && st()->retained_new == 1);
    snprintf(line, sizeof line,
             "KAGE VITA IMAGE RETAIN BIG: what=new image=%08x bytes=2097152 dim=1024x512x4 flags=00000000 tex0=%08x live=0/0 n=1 name=gfx/big_sheet.png",
             (unsigned)image, (unsigned)ld32(image + ISAAC_IR_IMG_TEX0));
    CHECK(strcmp(s_big[0], line) == 0);
    big_reset();
    image2 = mk(va(ISAAC_IR_VT_PNG_RVA), "small", 512, 256, 4, 1, &ctrl2);   /* 512 KiB */
    offer(image2, ctrl2);
    image2 = mk(va(ISAAC_IR_VT_PCX_RVA), "under", 512, 511, 4, 1, &ctrl2);   /* 1 MiB - 2048 */
    offer(image2, ctrl2);
    CHECK(s_big_n == 0U && st()->retained_new == 3);
    image2 = mk(va(ISAAC_IR_VT_PIC_RVA), "edge", 512, 512, 4, 1, &ctrl2);    /* exactly 1 MiB */
    offer(image2, ctrl2);
    CHECK(s_big_n == 1U && big_has(0, " what=new ") && big_has(0, " bytes=1048576 dim=512x512x4 "));
    CHECK(strcmp(big_name_of(0), "edge") == 0);
    /* reuse: the game takes the sheet again and drops it again */
    big_reset();
    load_hit(ctrl);
    sprite_dies(image, ctrl);
    CHECK(s_big_n == 1U && big_has(0, " what=reuse ") && st()->reuse == 1);
    CHECK(strcmp(big_name_of(0), "gfx/big_sheet.png") == 0);

    /* B2. Every ineligible reason is counted; big ones are named; the five
     *     counts add up to the module's own `ineligible` total. */
    big_reset();
    begin("big-ineligible-reasons", 12, 0);
    big_reset();
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "rt", 1024, 512, 4, 1, &ctrl);
    st32(image + ISAAC_IR_IMG_FLAGS, 0x4U);
    offer(image, ctrl);
    CHECK(s_big_n == 1U && big_has(0, BIG_INEL_DIRTY) && big_has(0, " flags=00000004 "));
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "rt2", 64, 64, 4, 1, &ctrl);          /* small: counted, not named */
    st32(image + ISAAC_IR_IMG_FLAGS, 0x10U);
    offer(image, ctrl);
    CHECK(s_big_n == 1U);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "tex0", 1024, 512, 4, 1, &ctrl);
    st32(image + ISAAC_IR_IMG_TEX0, 0U);
    offer(image, ctrl);
    CHECK(s_big_n == 2U && big_has(1, " what=inel-tex ") && big_has(1, " tex0=00000000 "));
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "texu", 1024, 512, 4, 1, &ctrl);
    st32(image + ISAAC_IR_IMG_TEX0, ISAAC_IR_TEX_UNSET);
    offer(image, ctrl);
    CHECK(s_big_n == 3U && big_has(2, " what=inel-tex ") && big_has(2, " tex0=ffffffff "));
    image = mk(va(ISAAC_IR_VT_PNG_RVA), NULL, 1024, 512, 4, 1, &ctrl);
    offer(image, ctrl);
    CHECK(s_big_n == 4U && big_has(3, " what=inel-name ") && strcmp(big_name_of(3), "") == 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "zero", 1024, 512, 0, 1, &ctrl);      /* bytes 0: counted, not named */
    offer(image, ctrl);
    CHECK(s_big_n == 4U);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "huge", 4096, 2048, 4, 1, &ctrl);     /* 32 MiB > ISAAC_IR_BYTES_CAP */
    offer(image, ctrl);
    CHECK(s_big_n == 5U && big_has(4, " what=inel-bytes ") && big_has(4, " bytes=33554432 dim=4096x2048x4 "));
    image = mk(va(ISAAC_IR_VT_PNG_RVA) + 0x40U, "foreign", 1024, 512, 4, 1, &ctrl);
    offer(image, ctrl);
    CHECK(s_big_n == 5U);                                                    /* foreign class: fields not read */
    CHECK(st()->ineligible == 8 && st()->oversize == 0 && st()->retained_new == 0);
    big_totals(t);
    CHECK(t[0] == 1U && t[1] == 2U && t[2] == 2U && t[3] == 1U && t[4] == 2U);
    CHECK(t[0] + t[1] + t[2] + t[3] + t[4] == st()->ineligible - st()->oversize);
    CHECK(t[5] == va(ISAAC_IR_VT_PNG_RVA) + 0x40U && t[6] == 5U && t[7] == 0U);

    /* B3. Fail-closed exits: budget 0, oversize, absent, headroom. */
    big_reset();
    begin("big-pass-budget0", 0, 0);
    big_reset();
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "b0", 1024, 512, 4, 1, &ctrl);
    offer(image, ctrl);
    CHECK(s_big_n == 1U && big_has(0, " what=pass-budget0 ") && manager_present(image) == 0);
    begin("big-oversize-absent", 4, 0);
    big_reset();
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "Obig", 1024, 2048, 4, 1, &ctrl);     /* 8 MiB > 4 MiB */
    offer(image, ctrl);
    CHECK(s_big_n == 1U && big_has(0, " what=oversize ") && st()->oversize == 1);
    image = new_image(va(ISAAC_IR_VT_PNG_RVA), "ghost", 1024, 512, 4, 0U);    /* never registered */
    ctrl = new_ctrl(1);
    offer(image, ctrl);
    CHECK(s_big_n == 2U && big_has(1, " what=absent ") && st()->absent == 1);
    CHECK(strcmp(big_name_of(1), "ghost") == 0);
    big_totals(t);
    CHECK(t[0] + t[1] + t[2] + t[3] + t[4] == 0U && st()->ineligible == 1 && t[6] == 2U);
    begin("big-headroom-pass", 12, 64);
    big_reset();
    s_free_ram = 4U << 20; s_free_vram = 0U; s_deferred_free = 1;
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "hp", 1024, 512, 4, 1, &ctrl);
    offer(image, ctrl);
    CHECK(s_big_n == 1U && big_has(0, " what=headroom-pass ") && st()->headroom_pass == 1);
    CHECK(manager_present(image) == 0);

    /* B4. A retained big sheet leaves: stale (the game cleared the manager
     *     behind the module) and budget eviction, each before the fact. */
    begin("big-stale-and-evict", 4, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "A", 1024, 512, 4, 1, &ctrl);
    offer(image, ctrl);
    manager_clear_behind_our_back();
    image2 = mk(va(ISAAC_IR_VT_PNG_RVA), "B", 1024, 512, 4, 1, &ctrl2);
    offer(image2, ctrl2);
    big_reset();
    image3 = mk(va(ISAAC_IR_VT_PNG_RVA), "C", 1024, 512, 4, 1, &ctrl3);
    offer(image3, ctrl3);                    /* 6 MiB > 4 MiB: the tail (A) goes, and it is dead */
    CHECK(s_big_n == 2U && big_has(0, " what=new ") && strcmp(big_name_of(0), "C") == 0);
    snprintf(line, sizeof line,
             "KAGE VITA IMAGE RETAIN BIG: what=stale image=%08x bytes=2097152 live=2/4194304 n=4",
             (unsigned)image);
    CHECK(strcmp(s_big[1], line) == 0);
    CHECK(st()->dropped_stale == 1 && st()->budget_evict == 0);
    big_reset();
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "D", 1024, 512, 4, 1, &ctrl);
    offer(image, ctrl);                      /* now B is the tail and it is alive */
    CHECK(s_big_n == 2U && big_has(0, " what=new ") && strcmp(big_name_of(0), "D") == 0);
    CHECK(big_has(1, " what=evict-budget ") && strcmp(big_name_of(1), "B") == 0);
    CHECK(st()->budget_evict == 1 && manager_present(image2) == 0);

    /* B5. Name handling and line length: the tail of a long name is kept,
     *     bytes outside 0x21..0x7e print as '?', the worst line fits the
     *     384-byte log body. */
    begin("big-name-and-length", 12, 0);
    big_reset();
    {
        char longname[131], expect[97];
        for (i = 0; i < 130U; ++i) longname[i] = (char)('a' + (i % 26U));
        longname[100] = (char)0x01; longname[101] = ' '; longname[102] = (char)0x7f;
        longname[130] = '\0';
        for (i = 0; i < 96U; ++i) expect[i] = longname[34U + i];
        expect[66] = '?'; expect[67] = '?'; expect[68] = '?';
        expect[96] = '\0';
        image = mk(va(ISAAC_IR_VT_PNG_RVA), "x", 65535, 65535, 0xffffffffU, 1, &ctrl);
        big_set_name(image, longname);
        st32(image + ISAAC_IR_IMG_FLAGS, 0xffffffffU);
        st32(image + ISAAC_IR_IMG_TEX0, 0xfffffffeU);
        offer(image, ctrl);
        CHECK(s_big_n == 1U && big_has(0, BIG_INEL_DIRTY) && big_has(0, " bytes=4294967295 dim=65535x65535x4294967295 "));
        CHECK(strcmp(big_name_of(0), expect) == 0);
        CHECK(strlen(s_big[0]) < 384U);
    }

    /* B6. The line cap: 512 lines, the rest only counted. */
    big_reset();
    begin("big-line-cap", 12, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "cap", 1024, 512, 4, 1, &ctrl);
    offer(image, ctrl);
    for (i = 0; i < 519U; ++i) {
        load_hit(ctrl);
        sprite_dies(image, ctrl);
    }
    CHECK(s_big_n == 1U + 512U + 520U / ISAAC_IR_REPORT_EVERY && st()->reuse == 519);   /* + the periodic TOTALS lines */
    big_totals(t);
    CHECK(t[6] == 512U && t[7] == 8U);
    isaac_vita_image_retain_disarm();
}
#endif

int main(void)
{
    uint32_t image, ctrl, image2, ctrl2, image3, ctrl3, i;
    uint32_t esp0;

    s_arena = (uint8_t *)calloc(1, ARENA_BYTES);
    s_stack = (uint8_t *)calloc(1, STACK_BYTES);
    if (!s_arena || !s_stack) { fprintf(stderr, "arena alloc\n"); return 2; }
    s_base = (uint32_t)(uintptr_t)s_arena;
    s_pair_slot = va(OFF_PAIRS);

    /* 0. Arming refuses when the observer globals do not hold the hook. */
    scenario("arm-global-guard");
    manager_reset();
    {
        isaac_ir_env env;
        env.image_base = s_base;
        env.buckets_ptr_va = va(ISAAC_IR_BUCKETS_PTR_RVA);
        env.hook_global_va = va(ISAAC_IR_HOOK_GLOBAL_RVA);
        env.hook_global2_va = va(ISAAC_IR_HOOK_GLOBAL2_RVA);
        env.vt_png = va(ISAAC_IR_VT_PNG_RVA); env.vt_pcx = va(ISAAC_IR_VT_PCX_RVA);
        env.vt_pic = va(ISAAC_IR_VT_PIC_RVA); env.vt_ctrl = va(ISAAC_IR_CTRL_VT_RVA);
#if !defined(IR_ORACLE_OLD_ABI)
        env.vt_mutex = va(OFF_LOCKVT);
        env.manager_lock_va = va(ISAAC_IR_MANAGER_LOCK_RVA);
#endif
        isaac_vita_image_retain_disarm();
        isaac_ir_oracle_set_env(&env);
        st32(va(ISAAC_IR_HOOK_GLOBAL2_RVA), 0U);
        isaac_vita_image_retain_arm();
        CHECK(isaac_ir_oracle_armed() == 0);
        {
            static const char banner[] =
                "KAGE VITA IMAGE RETAIN ARM: armed=0 reason=hook-global";
            CHECK(strncmp(s_last_log, banner, sizeof banner - 1U) == 0);
        }
        /* The manager lock must be the frozen, constructed Mutex. */
        scenario("arm-manager-lock-guard");
        manager_reset();
        st32(va(ISAAC_IR_MANAGER_LOCK_RVA), va(OFF_LOCKVT) + 0x40U);
        isaac_vita_image_retain_disarm();
        isaac_vita_image_retain_arm();
        CHECK(isaac_ir_oracle_armed() == 0);
        CHECK(strstr(s_last_log, "armed=0 reason=manager-lock") != NULL);
        manager_reset();
        st8(va(ISAAC_IR_MANAGER_LOCK_RVA) + ISAAC_IR_MUTEX_INIT, 0U);
        isaac_vita_image_retain_disarm();
        isaac_vita_image_retain_arm();
        CHECK(isaac_ir_oracle_armed() == 0);
        CHECK(strstr(s_last_log, "reason=manager-lock") != NULL);
        manager_reset();
        isaac_vita_image_retain_disarm();
        isaac_vita_image_retain_arm();
        CHECK(isaac_ir_oracle_armed() == 1);
        CHECK(strstr(s_last_log, " lock_held=0 global_ok=1") != NULL);
    }

    /* 1. Disarmed: the wrapper is the real observer (Unregister happens). */
    begin("disarmed-passthrough", 64, 0);
    isaac_vita_image_retain_disarm();
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "a.png", 256, 256, 4, 1, &ctrl);
    assert_passthrough_identical(make_pair(image, ctrl));
    CHECK(manager_present(image) == 0);
    CHECK(ev_count(EV_UNREG) == 1 && ev_count(EV_DESTROY) == 1);

    /* 2. Hot path: strong != 1 -> identical to the observer (Enter/Leave). */
    begin("strong-not-one", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "b.png", 256, 256, 4, 3, &ctrl);
    assert_passthrough_identical(make_pair(image, ctrl));
    CHECK(manager_present(image) == 1 && ev_count(EV_UNREG) == 0);
    CHECK(st()->passthrough == 1 && st()->offered == 0 && st()->retained_new == 0);
    CHECK(isaac_ir_oracle_live_records() == 0);

    /* 3. Ineligible candidates are freed by the real observer exactly. */
    begin("ineligible-rt-flag-4", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "rt4.png", 256, 256, 4, 1, &ctrl);
    st32(image + ISAAC_IR_IMG_FLAGS, 0x4U);
    assert_passthrough_identical(make_pair(image, ctrl));
    CHECK(manager_present(image) == 0 && st()->ineligible == 1 && st()->offered == 1);
    begin("ineligible-rt-flag-10", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "rt10.png", 256, 256, 4, 1, &ctrl);
    st32(image + ISAAC_IR_IMG_FLAGS, 0x10U);
    assert_passthrough_identical(make_pair(image, ctrl));
    CHECK(manager_present(image) == 0 && st()->ineligible == 1);
    begin("ineligible-tex0-unset", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "notex.png", 256, 256, 4, 1, &ctrl);
    st32(image + ISAAC_IR_IMG_TEX0, ISAAC_IR_TEX_UNSET);   /* base ctor value */
    assert_passthrough_identical(make_pair(image, ctrl));
    CHECK(manager_present(image) == 0 && st()->ineligible == 1);
    begin("ineligible-tex0-zero", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "notex0.png", 256, 256, 4, 1, &ctrl);
    st32(image + ISAAC_IR_IMG_TEX0, 0U);
    assert_passthrough_identical(make_pair(image, ctrl));
    CHECK(manager_present(image) == 0 && st()->ineligible == 1);
    begin("ineligible-foreign-image-vtable", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA) + 0x40U, "foreign.png", 256, 256, 4, 1, &ctrl);
    assert_passthrough_identical(make_pair(image, ctrl));
    CHECK(manager_present(image) == 0 && st()->ineligible == 1);
    begin("ineligible-no-name", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "x", 256, 256, 4, 1, &ctrl);
    st32(image + ISAAC_IR_IMG_NAME, 0U);
    assert_passthrough_identical(make_pair(image, ctrl));
    CHECK(manager_present(image) == 0 && st()->ineligible == 1);
    begin("ineligible-zero-bytes", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "z.png", 0, 256, 4, 1, &ctrl);
    assert_passthrough_identical(make_pair(image, ctrl));
    CHECK(manager_present(image) == 0 && st()->ineligible == 1);
    begin("ineligible-over-cap", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "huge.png", 4096, 4096, 4, 1, &ctrl); /* 64 MiB */
    assert_passthrough_identical(make_pair(image, ctrl));
    CHECK(manager_present(image) == 0 && st()->ineligible == 1);
    begin("foreign-ctrl-vtable", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "fc.png", 256, 256, 4, 1, &ctrl);
    st32(ctrl, va(ISAAC_IR_CTRL_VT_RVA) + 0x10U);
    assert_passthrough_identical(make_pair(image, ctrl));
    CHECK(manager_present(image) == 0 && st()->offered == 0 && st()->passthrough == 1);

    /* 4. Eligible but not in the manager: the real Unregister is a no-op. */
    begin("absent-from-manager", 64, 0);
    image = new_image(va(ISAAC_IR_VT_PNG_RVA), "np.png", 64, 64, 4, 0);
    ctrl = new_ctrl(1);
    assert_passthrough_identical(make_pair(image, ctrl));
    CHECK(ev_count(EV_UNREG) == 1 && ev_count(EV_UNREG_MISS) == 1);
    CHECK(st()->absent == 1 && isaac_ir_oracle_live_records() == 0);

    /* 5. Retain: PNG, PCX and PIC.  Enter/Leave once each with the observer's
     *    return words, no Unregister, entry kept at strong == 1, exact bytes,
     *    ESP = entry + 4 (the observer's ret), lock depth balanced. */
    begin("retain-three-vtables", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "p.png", 512, 512, 4, 1, &ctrl);
    image2 = mk(va(ISAAC_IR_VT_PCX_RVA), "q.pcx", 128, 256, 3, 1, &ctrl2);
    image3 = mk(va(ISAAC_IR_VT_PIC_RVA), "r.pic", 64, 64, 1, 1, &ctrl3);
    set_frame(&s_cpu, make_pair(image, ctrl));
    esp0 = s_cpu.esp;
    ev_reset();
    isaac_ir_oracle_wrap(&s_cpu, ref_observer);
    CHECK(retained_events_only());
    CHECK(s_events[0].a == ctrl + ISAAC_IR_CTRL_LOCK);   /* this = ctrl+8 */
    CHECK(s_cpu.esp == esp0 + 4U);
    CHECK(manager_present(image) == 1 && ld16(ctrl + ISAAC_IR_CTRL_STRONG) == 1);
    CHECK(ld32(ctrl + 0xcU) == 0U);                      /* Enter/Leave balanced */
    CHECK(isaac_ir_oracle_live_records() == 1);
    CHECK(isaac_ir_oracle_retained_bytes() == 512U * 512U * 4U);
    offer(image2, ctrl2);
    CHECK(retained_events_only() && manager_present(image2) == 1);
    offer(image3, ctrl3);
    CHECK(retained_events_only() && manager_present(image3) == 1);
    CHECK(isaac_ir_oracle_live_records() == 3);
    CHECK(isaac_ir_oracle_retained_bytes() ==
          512U * 512U * 4U + 128U * 256U * 3U + 64U * 64U * 1U);
    CHECK(st()->retained_new == 3 && st()->reuse == 0 && st()->evicted == 0);
    CHECK(st()->peak_bytes == isaac_ir_oracle_retained_bytes());

    /* 6. Load hit + sprite death: the game's own TryAddRef/Release, the
     *    observer re-offers the sheet, the module counts a reuse. */
    begin("load-hit-reuse", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "keep.png", 256, 256, 4, 1, &ctrl);
    offer(image, ctrl);
    CHECK(retained_events_only() && isaac_ir_oracle_live_records() == 1);
    load_hit(ctrl);                                      /* Load hit: strong 2 */
    CHECK(ld16(ctrl + ISAAC_IR_CTRL_STRONG) == 2);
    sprite_dies(image, ctrl);                            /* strong 1 -> hook */
    CHECK(retained_events_only());
    CHECK(st()->reuse == 1 && st()->retained_new == 1 && isaac_ir_oracle_live_records() == 1);
    CHECK(manager_present(image) == 1 && ld16(ctrl + ISAAC_IR_CTRL_STRONG) == 1);
    /* a hook call while another sprite still holds it is the hot path */
    load_hit(ctrl); load_hit(ctrl);                      /* strong 3 */
    st16(ctrl + ISAAC_IR_CTRL_STRONG, 2);                /* one died: Release -> hook */
    assert_passthrough_identical(make_pair(image, ctrl));
    CHECK(st()->reuse == 1 && ev_count(EV_UNREG) == 0);

    /* 7. LRU order: the least recently offered sheet is evicted first. */
    begin("lru-order-budget", 8, 0);
    {
        uint32_t imgs[4], ctrls[4];
        for (i = 0; i < 4U; ++i) {
            char nm[8]; nm[0] = 'L'; nm[1] = (char)('0' + i); nm[2] = 0;
            imgs[i] = mk(va(ISAAC_IR_VT_PNG_RVA), nm, 1024, 512, 4, 1, &ctrls[i]); /* 2 MiB */
        }
        for (i = 0; i < 4U; ++i) offer(imgs[i], ctrls[i]);   /* 8 MiB = budget */
        CHECK(isaac_ir_oracle_live_records() == 4 && st()->evicted == 0);
        offer(imgs[0], ctrls[0]);                            /* touch L0 -> MRU */
        CHECK(st()->reuse == 1);
        image = mk(va(ISAAC_IR_VT_PNG_RVA), "L4", 1024, 512, 4, 1, &ctrl);
        offer(image, ctrl);                                  /* 10 MiB -> evict LRU = L1 */
        CHECK(st()->evicted == 1 && st()->budget_evict == 1);
        CHECK(s_event_n == 6 && s_events[2].kind == EV_UNREG &&
              s_events[2].ret == ISAAC_IR_UNREGISTER_SITE_RVA && s_events[2].a == imgs[1] &&
              s_events[4].kind == EV_DESTROY);
        CHECK(manager_present(imgs[1]) == 0 && manager_present(imgs[0]) == 1);
        CHECK(ld32(imgs[1]) == 0xdead0000U);                 /* destroyed by the game */
        CHECK(isaac_ir_oracle_retained_bytes() == 8U << 20 && isaac_ir_oracle_live_records() == 4);
        CHECK(manager_count() == 4);
    }

    /* 8. Budget: a single sheet above the budget is retained then evicted at
     *    once = exactly the observer's Enter/Leave/Unregister(image). */
    begin("budget-oversize-single", 1, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "big.png", 1024, 1024, 4, 1, &ctrl); /* 4 MiB */
    offer(image, ctrl);
    CHECK(s_event_n >= 3 && s_events[0].kind == EV_ENTER && s_events[1].kind == EV_LEAVE &&
          s_events[2].kind == EV_UNREG && s_events[2].a == image &&
          s_events[2].ret == ISAAC_IR_UNREGISTER_SITE_RVA);
    CHECK(manager_present(image) == 0 && isaac_ir_oracle_live_records() == 0);
    /* review: a sheet above the whole budget is never retained (oversize),
       the real observer frees it -- same events, no eviction accounting */
    CHECK(st()->oversize == 1 && st()->ineligible == 1 && st()->evicted == 0 &&
          isaac_ir_oracle_retained_bytes() == 0);

    /* 9. Headroom: free memory below the headroom evicts tails until the
     *    model's free memory (which grows by the evicted bytes) recovers. */
    begin("headroom-evict", 256, 64);
    {
        uint32_t imgs[3], ctrls[3];
        for (i = 0; i < 3U; ++i) {
            char nm[8]; nm[0] = 'H'; nm[1] = (char)('0' + i); nm[2] = 0;
            imgs[i] = mk(va(ISAAC_IR_VT_PNG_RVA), nm, 1024, 1024, 4, 1, &ctrls[i]); /* 4 MiB */
        }
        for (i = 0; i < 3U; ++i) offer(imgs[i], ctrls[i]);
        CHECK(isaac_ir_oracle_live_records() == 3 && st()->headroom_evict == 0);
        s_free_ram = 50U << 20; s_free_vram = 8U << 20;    /* 58 MiB < 64 MiB */
        image = mk(va(ISAAC_IR_VT_PNG_RVA), "H3", 1024, 1024, 4, 1, &ctrl);
        offer(image, ctrl);                                  /* evicts H0, H1 (+8 -> 66 MiB) */
        CHECK(st()->headroom_evict == 2 && st()->evicted == 2);
        CHECK(manager_present(imgs[0]) == 0 && manager_present(imgs[1]) == 0);
        CHECK(manager_present(imgs[2]) == 1 && manager_present(image) == 1);
        CHECK(isaac_ir_oracle_live_records() == 2);
        CHECK(s_free_ram + s_free_vram >= 64U << 20);
        /* headroom 0 disables the rule */
        isaac_ir_oracle_set_budget(256U << 20, 0U);
        s_free_ram = 1U << 20; s_free_vram = 0U;
        image2 = mk(va(ISAAC_IR_VT_PNG_RVA), "H4", 64, 64, 4, 1, &ctrl2);
        offer(image2, ctrl2);
        CHECK(st()->headroom_evict == 2 && isaac_ir_oracle_live_records() == 3);
    }

    /* 10. Table full: ISAAC_IR_RECORDS + 1 distinct sheets keep the cap and
     *     evict the oldest through the guest Unregister (reason table_full). */
    begin("table-full", 1024, 0);
    {
        uint32_t first = 0, first_ctrl = 0;
        for (i = 0; i < ISAAC_IR_RECORDS + 1U; ++i) {
            image = new_image(va(ISAAC_IR_VT_PNG_RVA), NULL, 16, 16, 4, 0);
            st32(image + ISAAC_IR_IMG_NAME, va(OFF_NAME));
            ctrl = new_ctrl(1);
            manager_register(image, ctrl);
            if (i == 0) { first = image; first_ctrl = ctrl; }
            offer(image, ctrl);
        }
        CHECK(isaac_ir_oracle_live_records() == ISAAC_IR_RECORDS);
        CHECK(st()->table_full == 1 && st()->evicted == 1);
        CHECK(manager_present(first) == 0 && ld32(first) == 0xdead0000U);
        CHECK(ld16(first_ctrl + ISAAC_IR_CTRL_STRONG) == 0);
        CHECK(manager_count() == ISAAC_IR_RECORDS);
        CHECK(st()->retained_new == ISAAC_IR_RECORDS + 1U);
    }

    /* 11. Stale: the game Unregistered a retained sheet behind our back; the
     *     record is dropped without any guest call. */
    begin("stale-behind-our-back", 4, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "s0.png", 1024, 1024, 4, 1, &ctrl);   /* 4 MiB */
    offer(image, ctrl);
    CHECK(isaac_ir_oracle_live_records() == 1);
    do_unregister(&s_cpu, 0x5bfc46U, image);             /* direct sub_0056d220 */
    CHECK(manager_present(image) == 0);
    image2 = mk(va(ISAAC_IR_VT_PNG_RVA), "s1.png", 1024, 1024, 4, 1, &ctrl2);
    offer(image2, ctrl2);                                /* 8 > 4 MiB: evict tail (stale) */
    CHECK(retained_events_only());                       /* no Unregister replay */
    CHECK(st()->dropped_stale == 1 && st()->evicted == 0);
    CHECK(isaac_ir_oracle_live_records() == 1 && isaac_ir_oracle_retained_bytes() == 4U << 20);

    /* 12. Floor exit: the game clears every entry itself; every record goes
     *     stale; the next offers drop them silently and retain the new floor. */
    begin("floor-exit-clear", 64, 0);
    {
        uint32_t imgs[8], ctrls[8];
        for (i = 0; i < 8U; ++i) {
            char nm[8]; nm[0] = 'F'; nm[1] = (char)('0' + i); nm[2] = 0;
            imgs[i] = mk(va(ISAAC_IR_VT_PNG_RVA), nm, 512, 512, 4, 1, &ctrls[i]);   /* 1 MiB */
            offer(imgs[i], ctrls[i]);
        }
        CHECK(isaac_ir_oracle_live_records() == 8);
        manager_clear_behind_our_back();
        CHECK(manager_count() == 0);
        for (i = 0; i < 8U; ++i) CHECK(ld32(imgs[i]) == 0xdead0000U);
        /* the new floor: force evictions through a shrunk budget */
        isaac_ir_oracle_set_budget(2U << 20, 0U);
        for (i = 0; i < 4U; ++i) {
            char nm[8]; nm[0] = 'G'; nm[1] = (char)('0' + i); nm[2] = 0;
            image = mk(va(ISAAC_IR_VT_PNG_RVA), nm, 512, 512, 4, 1, &ctrl);
            offer(image, ctrl);
            CHECK(ev_count(EV_UNREG) == 0 || i >= 2U);   /* stale drops make no guest calls */
        }
        CHECK(st()->dropped_stale == 8 && st()->evicted == 2);
        CHECK(isaac_ir_oracle_live_records() == 2 && isaac_ir_oracle_retained_bytes() == 2U << 20);
        CHECK(manager_count() == 2);
    }

    /* 13. Address reuse: a new Image at a dead Image's address with a new
     *     control block must not be mistaken for the retained one. */
    begin("address-reuse", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "ar.png", 64, 64, 4, 1, &ctrl);
    offer(image, ctrl);
    do_unregister(&s_cpu, 0x5bfc46U, image);             /* dies behind our back */
    /* the allocator hands out the same address for a new sheet */
    st32(image, va(ISAAC_IR_VT_PNG_RVA));
    st32(image + ISAAC_IR_IMG_TEX0, 0x777U);
    ctrl2 = new_ctrl(1);
    manager_register(image, ctrl2);
    offer(image, ctrl2);
    CHECK(retained_events_only());
    CHECK(st()->dropped_stale == 1 && st()->retained_new == 2 && st()->reuse == 0);
    CHECK(isaac_ir_oracle_live_records() == 1);

    /* 14. Re-entrancy: the Image destructor run by our eviction releases a
     *     sub-object with strong == 1 while our Unregister holds the manager
     *     Mutex.  The real observer would Unregister it = re-enter the Mutex
     *     = hang; the wrapper must keep it (Enter/Leave replay, record it, no
     *     Unregister). */
    begin("in-evict-reentrancy", 5, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "re0.png", 1024, 1024, 4, 1, &ctrl);
    offer(image, ctrl);
    image3 = mk(va(ISAAC_IR_VT_PNG_RVA), "re2.png", 64, 64, 4, 1, &ctrl3);   /* the nested one */
    s_nested_image = image3; s_nested_ctrl = ctrl3;
    image2 = mk(va(ISAAC_IR_VT_PNG_RVA), "re1.png", 1024, 1024, 4, 1, &ctrl2);
    offer(image2, ctrl2);                                /* 8 > 5: evicts re0 -> dtor -> nested hook(re2) */
    CHECK(s_deadlock == 0);
    CHECK(st()->nested == 1 && st()->evicted == 1 && st()->orphan == 0);
    CHECK(ev_count(EV_UNREG) == 1 && ev_count(EV_MGR_ENTER) == 1);
    CHECK(manager_present(image3) == 1 && ld16(ctrl3 + ISAAC_IR_CTRL_STRONG) == 1);
    CHECK(manager_present(image) == 0 && manager_present(image2) == 1);
    CHECK(isaac_ir_oracle_live_records() == 2 && isaac_ir_oracle_in_evict() == 0);
    CHECK(isaac_ir_oracle_retained_bytes() == (4U << 20) + 64U * 64U * 4U);
    {
        /* the nested offer replayed the observer's Enter/Leave on re2's lock */
        unsigned k, enter = 0, leave = 0;
        for (k = 0; k < s_event_n; ++k) {
            if (s_events[k].kind == EV_ENTER && s_events[k].a == ctrl3 + ISAAC_IR_CTRL_LOCK &&
                s_events[k].ret == ISAAC_IR_ENTER_SITE_RVA) ++enter;
            if (s_events[k].kind == EV_LEAVE && s_events[k].a == ctrl3 + ISAAC_IR_CTRL_LOCK &&
                s_events[k].ret == ISAAC_IR_LEAVE_SITE_RVA) ++leave;
        }
        CHECK(enter == 1 && leave == 1);
    }
    /* re2 was recorded at the LRU tail (kept, never seen used): the next
     * squeeze evicts it first, then re1 */
    isaac_ir_oracle_set_budget(1U << 20, 0U);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "re3.png", 16, 16, 4, 1, &ctrl);
    offer(image, ctrl);
    CHECK(s_deadlock == 0 && manager_present(image2) == 0 && manager_present(image3) == 0);
    CHECK(s_events[2].kind == EV_UNREG && s_events[2].a == image3);
    CHECK(isaac_ir_oracle_live_records() == 1 && manager_present(image) == 1);

    /* 15. Unregister `ret 4` contract breach -> fault path, record dropped,
     *     in_evict cleared, nothing else touched. */
    begin("unregister-esp-contract", 4, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "f0.png", 1024, 1024, 4, 1, &ctrl);
    offer(image, ctrl);
    image2 = mk(va(ISAAC_IR_VT_PNG_RVA), "f1.png", 1024, 1024, 4, 1, &ctrl2);
    s_unreg_break_esp = 1;
    set_frame(&s_cpu, make_pair(image2, ctrl2));
    esp0 = s_cpu.esp;
    ev_reset();
    isaac_ir_oracle_wrap(&s_cpu, ref_observer);
    s_unreg_break_esp = 0;
    CHECK(st()->fault == 1 && s_fault_hit == 1);
    CHECK(strcmp(s_fault_what, "image retain: Unregister stack contract") == 0);
    CHECK(isaac_ir_oracle_in_evict() == 0);
    CHECK(s_cpu.esp == esp0 + 4U);                       /* esp restored, ret emulated */
    CHECK(isaac_ir_oracle_live_records() == 1 && manager_present(image2) == 1);

    /* 16. Hostile frames fall closed before touching anything. */
    begin("hostile-pair-null", 64, 0);
    set_frame(&s_cpu, 0U);
    ev_reset();
    isaac_ir_oracle_wrap(&s_cpu, real_probe);
    CHECK(s_event_n == 1 && s_events[0].kind == EV_REAL_PROBE && st()->passthrough == 1);
    begin("hostile-pair-misaligned", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "hm.png", 64, 64, 4, 1, &ctrl);
    set_frame(&s_cpu, make_pair(image, ctrl) | 2U);
    ev_reset();
    isaac_ir_oracle_wrap(&s_cpu, real_probe);
    CHECK(s_event_n == 1 && s_events[0].kind == EV_REAL_PROBE && isaac_ir_oracle_live_records() == 0);
    begin("hostile-image-misaligned", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "hi.png", 64, 64, 4, 1, &ctrl);
    set_frame(&s_cpu, make_pair(image | 1U, ctrl));
    ev_reset();
    isaac_ir_oracle_wrap(&s_cpu, real_probe);
    CHECK(s_event_n == 1 && s_events[0].kind == EV_REAL_PROBE && isaac_ir_oracle_live_records() == 0);
    begin("hostile-ctrl-misaligned", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "hc.png", 64, 64, 4, 1, &ctrl);
    set_frame(&s_cpu, make_pair(image, ctrl | 2U));
    ev_reset();
    isaac_ir_oracle_wrap(&s_cpu, real_probe);
    CHECK(s_event_n == 1 && s_events[0].kind == EV_REAL_PROBE && isaac_ir_oracle_live_records() == 0);
    begin("hostile-ctrl-null", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "hn.png", 64, 64, 4, 1, &ctrl);
    assert_passthrough_identical(make_pair(image, 0U));  /* observer: esi==0 -> ret */
    CHECK(s_event_n == 0 && isaac_ir_oracle_live_records() == 0);
    begin("hostile-esp-outside-stack", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "he.png", 64, 64, 4, 1, &ctrl);
    cpu_bind(&s_cpu);
    s_cpu.esp = s_cpu.stack_ceiling - 4U;                /* [esp+4] is past the ceiling */
    ev_reset();
    isaac_ir_oracle_wrap(&s_cpu, real_probe);
    CHECK(s_event_n == 1 && s_events[0].kind == EV_REAL_PROBE);
    CHECK(s_stack_violations == 0);                      /* the wrapper raised nothing */
    begin("hostile-unbound-cpu", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "hu.png", 64, 64, 4, 1, &ctrl);
    set_frame(&s_cpu, make_pair(image, ctrl));
    s_cpu.stack_owner = NULL;                            /* not a bound stack */
    ev_reset();
    isaac_ir_oracle_wrap(&s_cpu, real_probe);
    CHECK(s_event_n == 1 && s_events[0].kind == EV_REAL_PROBE && isaac_ir_oracle_live_records() == 0);
    begin("hostile-lock-vtable", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "hl.png", 64, 64, 4, 1, &ctrl);
    st32(ctrl + ISAAC_IR_CTRL_LOCK, 0U);                 /* no lock vtable */
    set_frame(&s_cpu, make_pair(image, ctrl));
    ev_reset();
    isaac_ir_oracle_wrap(&s_cpu, real_probe);
    CHECK(s_event_n == 1 && s_events[0].kind == EV_REAL_PROBE && isaac_ir_oracle_live_records() == 0);
    CHECK(st()->offered == 1 && st()->passthrough == 1);

    /* 17. A foreign CPU falls closed (identical to the observer). */
    begin("foreign-cpu", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "cpu0.png", 64, 64, 4, 1, &ctrl);
    offer(image, ctrl);                                  /* pins s_cpu */
    CHECK(isaac_ir_oracle_live_records() == 1);
    image2 = mk(va(ISAAC_IR_VT_PNG_RVA), "cpu1.png", 64, 64, 4, 1, &ctrl2);
    {
        static snap before;
        set_frame(&s_cpu_foreign, make_pair(image2, ctrl2));
        snap_take(&before, &s_cpu_foreign);
        ev_reset();
        isaac_ir_oracle_wrap(&s_cpu_foreign, ref_observer);
        CHECK(st()->foreign_cpu == 1 && ev_count(EV_UNREG) == 1);
        CHECK(manager_present(image2) == 0 && isaac_ir_oracle_live_records() == 1);
    }

    /* 18. Budget 0 (stage-2a measurement / kill switch): candidates counted,
     *     behaviour identical to the observer. */
    begin("budget-zero-measurement", 0, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "m.png", 64, 64, 4, 1, &ctrl);
    assert_passthrough_identical(make_pair(image, ctrl));
    CHECK(st()->offered == 1 && st()->retained_new == 0 && st()->passthrough == 1);
    CHECK(manager_present(image) == 0);

    /* 19. Strong changes between the unlocked pre-check and the lock (another
     *     thread's TryAddRef): the observer's own no-op tail -- Enter/Leave,
     *     no Unregister, no record, the observer's ret. */
    begin("strong-raced-under-lock", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "race.png", 64, 64, 4, 1, &ctrl);
    s_race_bump = 1;
    set_frame(&s_cpu, make_pair(image, ctrl));
    esp0 = s_cpu.esp;
    ev_reset();
    isaac_ir_oracle_wrap(&s_cpu, ref_observer);
    CHECK(s_race_bump == 0);
    CHECK(retained_events_only());
    CHECK(s_cpu.esp == esp0 + 4U);
    CHECK(isaac_ir_oracle_live_records() == 0 && st()->passthrough == 1 && st()->offered == 1);
    CHECK(manager_present(image) == 1 && ld16(ctrl + ISAAC_IR_CTRL_STRONG) == 2);

    /* 20. Disarm mid-session: records cleared, later offers are the observer. */
    begin("disarm-mid-session", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "d0.png", 64, 64, 4, 1, &ctrl);
    offer(image, ctrl);
    CHECK(isaac_ir_oracle_live_records() == 1);
    isaac_vita_image_retain_disarm();
    CHECK(isaac_ir_oracle_live_records() == 0 && isaac_ir_oracle_armed() == 0);
    image2 = mk(va(ISAAC_IR_VT_PNG_RVA), "d1.png", 64, 64, 4, 1, &ctrl2);
    assert_passthrough_identical(make_pair(image2, ctrl2));
    CHECK(manager_present(image2) == 0 && ev_count(EV_UNREG) == 1);
    /* the abandoned record's image is still the game's (strong 1, present) */
    CHECK(manager_present(image) == 1 && ld16(ctrl + ISAAC_IR_CTRL_STRONG) == 1);

    /* 21. Receipt: field order and the periodic cadence (every 64 offers). */
    begin("receipt-format", 64, 0);
    s_receipt_calls = 0; s_mem_calls = 0;
    isaac_vita_image_retain_report("loading-complete");
    CHECK(s_receipt_calls == 1);
    {
        static const char prefix[] =
            "KAGE VITA IMAGE RETAIN: offered=0 retained=0 reuse=0 evicted=0 "
            "evicted_bytes=0 retained_bytes=0 budget=67108864 passthrough=0 "
            "ineligible=0 stale=0 table_full=0 headroom_evict=0 ";
        CHECK(strncmp(s_last_receipt, prefix, sizeof prefix - 1U) == 0);
    }
    CHECK(strstr(s_last_receipt, " why=loading-complete") != NULL);
    CHECK(s_mem_calls == 1 && strncmp(s_last_mem, "KAGE VITA IMAGE RETAIN MEM: free=", 33) == 0);
    CHECK(strstr(s_last_mem, " vf_ram=") != NULL && strstr(s_last_mem, " why=loading-complete") != NULL);
    for (i = 0; i < 64U; ++i) {
        image = mk(va(ISAAC_IR_VT_PNG_RVA), "r", 16, 16, 4, 1, &ctrl);
        offer(image, ctrl);
    }
    CHECK(s_receipt_calls == 2 && strstr(s_last_receipt, "offered=64 retained=63 ") != NULL);
    CHECK(strstr(s_last_receipt, " why=periodic") != NULL);
    {
        isaac_ir_window w;
        isaac_vita_image_retain_window(&w);
        CHECK(w.live_records == 64 && w.armed == 1 && w.budget_bytes == (64U << 20));
        CHECK(w.vf_ram == s_free_ram && w.vf_vram == s_free_vram);
        CHECK(w.totals.offered == 64 && w.totals.retained_bytes == 64U * 16U * 16U * 4U);
    }

    /* 22. Eviction of T from a bucket where retained entries sit behind it:
     *     the game's erase loop re-offers each of them (strong 1) with the
     *     manager Mutex held.  HEAD 57fc40b passed those to __real ->
     *     Unregister -> Mutex re-entry -> the game hangs.  Now: kept, records
     *     and LRU order untouched, one Unregister, no deadlock. */
    begin("evict-shifts-retained-neighbours", 8, 0);
    {
        uint32_t t, tc, r1, r1c, l, lc, r3, r3c, n, nc, n2, n2c;
        t  = mk_in_bucket(5, va(ISAAC_IR_VT_PNG_RVA), "T",  1024, 1024, 4, 1, &tc);  /* 4 MiB */
        r1 = mk_in_bucket(5, va(ISAAC_IR_VT_PNG_RVA), "R1", 512, 512, 4, 1, &r1c);   /* 1 MiB */
        l  = mk_in_bucket(5, va(ISAAC_IR_VT_PNG_RVA), "L",  512, 512, 4, 2, &lc);    /* live */
        r3 = mk_in_bucket(5, va(ISAAC_IR_VT_PNG_RVA), "R3", 512, 512, 4, 1, &r3c);   /* 1 MiB */
        CHECK(ld32(entry_at(5, 0)) == t && ld32(entry_at(5, 3)) == r3);
        offer(t, tc); offer(r1, r1c); offer(r3, r3c);
        CHECK(isaac_ir_oracle_live_records() == 3 && st()->evicted == 0);
        n = mk(va(ISAAC_IR_VT_PNG_RVA), "N", 1024, 1024, 4, 1, &nc);          /* +4 MiB -> 10 > 8 */
        offer(n, nc);
        CHECK(s_deadlock == 0 && ev_count(EV_MGR_DEADLOCK) == 0);
        CHECK(st()->evicted == 1 && ev_count(EV_UNREG) == 1 && ev_count(EV_MGR_ENTER) == 1);
        CHECK(ld32(t) == 0xdead0000U && manager_present(t) == 0);
        CHECK(st()->nested == 2);                        /* R1 (loop temp) + R3 (tail entry) */
        CHECK(st()->orphan == 0 && st()->retained_new == 4 && st()->reuse == 0);
        CHECK(manager_present(r1) == 1 && ld16(r1c + ISAAC_IR_CTRL_STRONG) == 1);
        CHECK(manager_present(r3) == 1 && ld16(r3c + ISAAC_IR_CTRL_STRONG) == 1);
        CHECK(manager_present(l) == 1 && ld16(lc + ISAAC_IR_CTRL_STRONG) == 2);
        CHECK(ld32(entry_at(5, 0)) == r1 && ld32(entry_at(5, 1)) == l && ld32(entry_at(5, 2)) == r3);
        CHECK((ld32(bucket_hdr(5) + 4U) - ld32(bucket_hdr(5))) / 8U == 3U);
        CHECK(isaac_ir_oracle_live_records() == 3 && isaac_ir_oracle_retained_bytes() == 6U << 20);
        CHECK(ld32(r1c + 0xcU) == 0U && ld32(r3c + 0xcU) == 0U); /* lock depth balanced */
        /* LRU order is unchanged by the kept offers: R1 goes before R3 */
        n2 = mk(va(ISAAC_IR_VT_PNG_RVA), "N2", 1024, 768, 4, 1, &n2c);   /* 3 MiB */
        offer(n2, n2c);                                  /* 9 > 8 -> evict R1 only */
        CHECK(s_deadlock == 0 && ev_count(EV_UNREG) == 1 && s_events[2].kind == EV_UNREG &&
              s_events[2].a == r1);
        CHECK(manager_present(r1) == 0 && manager_present(r3) == 1 && manager_present(n) == 1);
        CHECK(st()->nested == 3);                        /* R3 re-offered once more (tail) */
    }

    /* 23. The game's own Unregister (an ineligible sheet through __real) with
     *     retained entries behind it while free memory is below the headroom:
     *     the shifted retained entries must be kept and NOT evicted under the
     *     Mutex; the eviction happens at the next unlocked candidate. */
    begin("foreign-unregister-defers-eviction", 256, 64);
    {
        uint32_t x, xc, r1, r1c, r2, r2c, n, nc;
        x  = mk_in_bucket(7, va(ISAAC_IR_VT_PNG_RVA), "X",  512, 512, 4, 1, &xc);
        r1 = mk_in_bucket(7, va(ISAAC_IR_VT_PNG_RVA), "R1", 1024, 1024, 4, 1, &r1c);
        r2 = mk_in_bucket(7, va(ISAAC_IR_VT_PNG_RVA), "R2", 1024, 1024, 4, 1, &r2c);
        offer(r1, r1c); offer(r2, r2c);
        CHECK(isaac_ir_oracle_live_records() == 2);
        s_free_ram = 20U << 20; s_free_vram = 8U << 20;  /* 28 MiB < 64 MiB headroom */
        st32(x + ISAAC_IR_IMG_FLAGS, 0x4U);              /* X: render-target list -> ineligible */
        offer(x, xc);                                    /* __real -> Unregister(X) */
        CHECK(s_deadlock == 0);
        CHECK(st()->ineligible == 1 && ev_count(EV_UNREG) == 1 && ev_count(EV_MGR_ENTER) == 1);
        CHECK(ld32(x) == 0xdead0000U);
        CHECK(st()->held_keep == 2 && st()->evicted == 0 && st()->deferred >= 1);
        CHECK(manager_present(r1) == 1 && manager_present(r2) == 1);
        CHECK(ld16(r1c + ISAAC_IR_CTRL_STRONG) == 1 && ld16(r2c + ISAAC_IR_CTRL_STRONG) == 1);
        CHECK(isaac_ir_oracle_live_records() == 2 && !mgr_held());
        /* next unlocked candidate: the headroom rule now evicts (R1 first) */
        n = mk(va(ISAAC_IR_VT_PNG_RVA), "N", 64, 64, 4, 1, &nc);
        offer(n, nc);
        CHECK(s_deadlock == 0 && st()->headroom_evict >= 1 && manager_present(r1) == 0);
        CHECK(first_unreg() == r1 && count_unreg_of(r1) == 1);   /* LRU order: R1 before R2 */
        CHECK(manager_present(r2) == 0 && st()->headroom_pass == 1 && manager_present(n) == 0);
        CHECK(isaac_ir_oracle_live_records() == 0 && st()->deferred >= 1);
    }

    /* 24. Bucket growth under Load's manager lock: every old entry is
     *     destroyed (observer for each) while the header still shows the old
     *     storage; retained ones must be kept with their records valid across
     *     the storage move, and a later eviction must find them in the new
     *     storage. */
    begin("register-growth-under-lock", 8, 0);
    {
        uint32_t r1, r1c, l, lc, r2, r2c, n, nc;
        r1 = mk_in_bucket(9, va(ISAAC_IR_VT_PNG_RVA), "R1", 1024, 1024, 4, 1, &r1c);
        l  = mk_in_bucket(9, va(ISAAC_IR_VT_PNG_RVA), "L",  512, 512, 4, 3, &lc);
        r2 = mk_in_bucket(9, va(ISAAC_IR_VT_PNG_RVA), "R2", 1024, 1024, 4, 1, &r2c);
        offer(r1, r1c); offer(r2, r2c);
        ev_reset();
        manager_grow(&s_cpu, 9, 0x56d1cdU);
        CHECK(s_deadlock == 0 && ev_count(EV_OBSERVER) == 3 && ev_count(EV_UNREG) == 0);
        CHECK(st()->held_keep == 2 && st()->evicted == 0 && st()->orphan == 0);
        CHECK(ld32(bucket_hdr(9)) == va(OFF_ENTRY2) + 9U * 0x2000U);
        CHECK(manager_present(r1) == 1 && manager_present(l) == 1 && manager_present(r2) == 1);
        CHECK(ld16(r1c + ISAAC_IR_CTRL_STRONG) == 1 && ld16(lc + ISAAC_IR_CTRL_STRONG) == 3);
        CHECK(isaac_ir_oracle_live_records() == 2 && isaac_ir_oracle_retained_bytes() == 8U << 20);
        n = mk(va(ISAAC_IR_VT_PNG_RVA), "N", 512, 512, 4, 1, &nc);   /* 9 > 8 -> evict R1 */
        offer(n, nc);
        CHECK(s_deadlock == 0 && st()->evicted == 1 && ld32(r1) == 0xdead0000U);
        CHECK(manager_present(r2) == 1 && manager_present(n) == 1 && manager_present(r1) == 0);
        CHECK(st()->dropped_stale == 0);
    }

    /* 25. Under the held Mutex nothing may reach __real for a strong == 1
     *     candidate, eligible or not, present or not: Enter/Leave only. */
    begin("held-ineligible-never-real", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "hi.png", 256, 256, 4, 1, &ctrl);
    st32(image + ISAAC_IR_IMG_FLAGS, 0x10U);
    st8(va(OFF_MGR_CS) + ISAAC_IR_CS_HELD, 1U);          /* inside Load/Unregister */
    offer(image, ctrl);
    CHECK(s_deadlock == 0 && retained_events_only());
    CHECK(st()->held_keep == 1 && st()->orphan == 1 && st()->ineligible == 0);
    CHECK(manager_present(image) == 1 && isaac_ir_oracle_live_records() == 0);
    st8(va(OFF_MGR_CS) + ISAAC_IR_CS_HELD, 0U);
    assert_passthrough_identical(make_pair(image, ctrl));  /* unlocked: real frees it */
    CHECK(manager_present(image) == 0 && st()->ineligible == 1);
    begin("held-absent-never-real", 64, 0);
    image = new_image(va(ISAAC_IR_VT_PNG_RVA), "ab.png", 64, 64, 4, 0);
    ctrl = new_ctrl(1);
    st8(va(OFF_MGR_CS) + ISAAC_IR_CS_HELD, 1U);
    offer(image, ctrl);
    CHECK(s_deadlock == 0 && retained_events_only() && st()->orphan == 1 && st()->absent == 0);
    st8(va(OFF_MGR_CS) + ISAAC_IR_CS_HELD, 0U);
    begin("held-eligible-gets-record", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "hr.png", 256, 256, 4, 1, &ctrl);
    st8(va(OFF_MGR_CS) + ISAAC_IR_CS_HELD, 1U);
    offer(image, ctrl);
    CHECK(s_deadlock == 0 && retained_events_only());
    CHECK(st()->held_keep == 1 && st()->retained_new == 1 && st()->orphan == 0);
    CHECK(isaac_ir_oracle_live_records() == 1 && manager_present(image) == 1);
    offer(image, ctrl);                                  /* re-offer under the lock: no reuse count */
    CHECK(st()->held_keep == 2 && st()->reuse == 0 && isaac_ir_oracle_live_records() == 1);
    st8(va(OFF_MGR_CS) + ISAAC_IR_CS_HELD, 0U);
    /* the same pre-check in a normal (unlocked) offer is a Load-hit reuse */
    load_hit(ctrl); sprite_dies(image, ctrl);
    CHECK(st()->reuse == 1);

    /* 26. The reference observer itself, offered a retained entry under the
     *     held Mutex, deadlocks -- the detector must see it (negative
     *     control for scenarios 22-25). */
    begin("reference-observer-deadlocks-under-lock", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "dl.png", 64, 64, 4, 1, &ctrl);
    st8(va(OFF_MGR_CS) + ISAAC_IR_CS_HELD, 1U);
    set_frame(&s_cpu, make_pair(image, ctrl));
    ev_reset();
    ref_observer(&s_cpu);
    CHECK(s_deadlock == 1 && ev_count(EV_MGR_DEADLOCK) == 1 && manager_present(image) == 1);
    s_deadlock = 0;
    st8(va(OFF_MGR_CS) + ISAAC_IR_CS_HELD, 0U);

    /* 27. A control block whose embedded lock is not the frozen Mutex (but
     *     has plausible slots) must fall closed instead of being replayed. */
    begin("hostile-ctrl-lock-foreign-mutex", 64, 0);
    image = mk(va(ISAAC_IR_VT_PNG_RVA), "fm.png", 64, 64, 4, 1, &ctrl);
    st32(va(OFF_LOCKVT) + 0x100U + ISAAC_IR_LOCK_VT_ENTER, va(OFF_FAKE_ENTER));
    st32(va(OFF_LOCKVT) + 0x100U + ISAAC_IR_LOCK_VT_LEAVE, va(OFF_FAKE_LEAVE));
    st32(ctrl + ISAAC_IR_CTRL_LOCK, va(OFF_LOCKVT) + 0x100U);
    set_frame(&s_cpu, make_pair(image, ctrl));
    ev_reset();
    isaac_ir_oracle_wrap(&s_cpu, real_probe);
    CHECK(s_event_n == 1 && s_events[0].kind == EV_REAL_PROBE && isaac_ir_oracle_live_records() == 0);
    CHECK(st()->offered == 1 && st()->passthrough == 1);

    /* 28. Floor exit through the game's own Unregisters with several retained
     *     entries per bucket: every erase re-offers the survivors under the
     *     Mutex; nothing may deadlock and the manager must end empty. */
    begin("floor-exit-dense-buckets", 256, 0);
    {
        uint32_t imgs[12], ctrls[12], k;
        for (k = 0; k < 12U; ++k) {
            char nm[8]; nm[0] = 'D'; nm[1] = (char)('0' + k / 4U); nm[2] = (char)('0' + k % 4U); nm[3] = 0;
            imgs[k] = mk_in_bucket(3U + k / 4U, va(ISAAC_IR_VT_PNG_RVA), nm, 256, 256, 4, 1, &ctrls[k]);
            offer(imgs[k], ctrls[k]);
        }
        CHECK(isaac_ir_oracle_live_records() == 12);
        ev_reset();
        manager_clear_behind_our_back();
        CHECK(s_deadlock == 0 && manager_count() == 0 && ev_count(EV_UNREG_MISS) == 0);
        for (k = 0; k < 12U; ++k) CHECK(ld32(imgs[k]) == 0xdead0000U);
        CHECK(st()->held_keep == 3U * (3U + 2U + 1U) && st()->evicted == 0);
        CHECK(st()->orphan == 0 && st()->reuse == 0);
        /* the 12 records are stale; the next squeeze drops them without a
           guest call (a 1 MiB sheet fits the 1 MiB budget exactly; larger
           would be oversize = the real observer's, no squeeze) */
        isaac_ir_oracle_set_budget(1U << 20, 0U);
        ev_reset();
        image = mk(va(ISAAC_IR_VT_PNG_RVA), "after", 512, 512, 4, 1, &ctrl);    /* 1 MiB == budget */
        offer(image, ctrl);
        CHECK(s_deadlock == 0 && st()->dropped_stale == 12 && st()->evicted == 0);
        CHECK(ev_count(EV_UNREG) == 0 && ev_count(EV_UNREG_MISS) == 0 && st()->oversize == 0);
        CHECK(isaac_ir_oracle_live_records() == 1 && manager_count() == 1 && manager_present(image) == 1);
        CHECK(isaac_ir_oracle_retained_bytes() == 1U << 20);
    }

    /* 33. Disarm with retained entries still in the manager (entry_vita.c's
     *     teardown, or any future mid-session disarm): a foreign Unregister's
     *     shift loop re-offers them under the held manager Mutex; the
     *     disarmed wrapper must keep them (never __real), while every
     *     unlocked offer is the observer's. */
    begin("disarm-guard-under-lock", 64, 0);
    {
        uint32_t x, xc, r1, r1c, r2, r2c;
        x  = mk_in_bucket(11, va(ISAAC_IR_VT_PNG_RVA), "X",  64, 64, 4, 1, &xc);
        r1 = mk_in_bucket(11, va(ISAAC_IR_VT_PNG_RVA), "R1", 64, 64, 4, 1, &r1c);
        r2 = mk_in_bucket(11, va(ISAAC_IR_VT_PNG_RVA), "R2", 64, 64, 4, 1, &r2c);
        offer(r1, r1c); offer(r2, r2c);
        CHECK(isaac_ir_oracle_live_records() == 2);
        isaac_vita_image_retain_disarm();
        CHECK(isaac_ir_oracle_armed() == 0 && isaac_ir_oracle_live_records() == 0);
        st32(x + ISAAC_IR_IMG_FLAGS, 0x4U);             /* X ineligible -> __real */
        offer(x, xc);                                   /* Unregister(X): shifts R1, R2 under the lock */
        CHECK(s_deadlock == 0 && ev_count(EV_UNREG) == 1 && ev_count(EV_MGR_ENTER) == 1);
        CHECK(manager_present(x) == 0 && manager_present(r1) == 1 && manager_present(r2) == 1);
        CHECK(ld16(r1c + ISAAC_IR_CTRL_STRONG) == 1 && ld16(r2c + ISAAC_IR_CTRL_STRONG) == 1);
        CHECK(st()->held_keep == 2 && st()->retained_new == 2 && !mgr_held());
        /* unlocked Unregister(R1) via __real: its shift re-offers R2 under the
         * lock -> kept again (the reference observer would deadlock here) */
        offer(r1, r1c);
        CHECK(manager_present(r1) == 0 && ev_count(EV_UNREG) == 1 && s_deadlock == 0);
        CHECK(manager_present(r2) == 1 && st()->held_keep == 3 && isaac_ir_oracle_live_records() == 0);
        /* unlocked, no retained neighbours: the disarmed wrapper is the
         * observer, byte for byte */
        {
            uint32_t y, yc;
            y = mk_in_bucket(12, va(ISAAC_IR_VT_PNG_RVA), "Y", 64, 64, 4, 1, &yc);
            assert_passthrough_identical(make_pair(y, yc));
            CHECK(manager_present(y) == 0 && ev_count(EV_UNREG) == 1 && s_deadlock == 0);
        }
    }

    memory_hostile_scenarios();
#if defined(IR_ORACLE_FLUSH)
    flush_scenarios();
#endif
#if defined(ISAAC_VITA_IMAGE_PRELOAD)
    preload_scenarios();
#endif
#if defined(ISAAC_VITA_IMAGE_RETAIN_BIG_TRACE)
    big_trace_scenarios();
#endif

    CHECK(s_unknown_targets == 0);
    CHECK(s_deadlock == 0);
#if defined(IR_ORACLE_FLUSH)
    CHECK(s_walker_violations == 0);
#endif

    if (s_fail == 0)
        printf("IMAGE RETAIN ORACLE PASS: checks=%u scenarios=%u records=%u\n",
               s_checks, s_scenarios, (unsigned)ISAAC_IR_RECORDS);
    else
        printf("IMAGE RETAIN ORACLE FAIL: checks=%u failures=%u scenarios=%u\n",
               s_checks, s_fail, s_scenarios);
    free(s_arena);
    free(s_stack);
    return s_fail ? 1 : 0;
}
