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
    if (strncmp(s_last_log, "KAGE VITA IMAGE RETAIN MEM: ", 28) == 0) {
        memcpy(s_last_mem, s_last_log, sizeof s_last_mem);
        ++s_mem_calls;
    }
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

    CHECK(s_unknown_targets == 0);
    CHECK(s_deadlock == 0);

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
