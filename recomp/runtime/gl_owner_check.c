/* Render-thread ownership check at the vitaGL link boundary
 * (ISAAC_VITA_GL_OWNER_CHECK, needs ISAAC_VITA_GL_THREAD_WORKER).
 *
 * Every vitaGL entry the game links against is --wrap'ed by a signature-free
 * Thumb-2 trampoline: compare TPIDRURO (the per-thread TLS register vitaGL's
 * own DEBUG_THREAD_SAFENESS reads) with the owner's value; equal or unclaimed
 * tail-calls __real_<sym> untouched.  Otherwise the call is COUNTED per entry
 * (first 16 logged with their thread; a per-entry table every 10 s on the
 * render thread) and then still made: unlike THREAD_SAFE(), nothing is dropped.
 * vitaGL-internal calls are wrapped too (they run on the caller's thread).
 * No allocation; one TU so the raw allocator gate sees an ordinary owner. */
#include <stdint.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#if !defined(ISAAC_VITA_GL_OWNER_CHECK) || !ISAAC_VITA_GL_OWNER_CHECK
#error Compile the owner check only when explicitly enabled
#endif

void isaac_vita_log(const char *format, ...);

uint32_t isaac_gl_owner_tls;            /* 0 = unclaimed */
static volatile uint32_t s_violations;

/* Per-entry evidence: one slot per violating vitaGL entry (at most the 79
 * wrapped ones), claimed lock-free by name pointer. */
#define GL_OWNER_SLOTS 96u
typedef struct GlOwnerSlot {
    const char *volatile entry;
    volatile uint32_t count;
    volatile uint32_t last_tid;
    uint32_t reported;                  /* render thread only */
} GlOwnerSlot;
static GlOwnerSlot s_slots[GL_OWNER_SLOTS];
static volatile uint32_t s_slot_overflow;
static uint64_t s_last_report_us;
#define GL_OWNER_REPORT_INTERVAL_US UINT64_C(10000000)

static uint32_t gl_owner_tpidruro(void)
{
    uint32_t value;
    __asm__ __volatile__("mrc p15, 0, %0, c13, c0, 3" : "=r"(value));
    return value;
}

void isaac_gl_owner_claim(void)
{
    __atomic_store_n(&isaac_gl_owner_tls, gl_owner_tpidruro(), __ATOMIC_RELEASE);
    s_last_report_us = sceKernelGetProcessTimeWide();
    isaac_vita_log("KAGE VITA GL OWNER: claimed tid=0x%08x tls=0x%08x",
                   (unsigned)sceKernelGetThreadId(), (unsigned)isaac_gl_owner_tls);
}

static GlOwnerSlot *gl_owner_slot(const char *entry)
{
    uint32_t i;
    for (i = 0u; i < GL_OWNER_SLOTS; ++i) {
        const char *seen = __atomic_load_n(&s_slots[i].entry, __ATOMIC_ACQUIRE);
        if (seen == entry)
            return &s_slots[i];
        if (!seen) {
            const char *expected = NULL;
            if (__atomic_compare_exchange_n(&s_slots[i].entry, &expected, entry, 0,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ||
                    expected == entry)
                return &s_slots[i];
        }
    }
    return NULL;
}

/* Called from a trampoline with r0-r3/lr saved; must not touch vitaGL. */
void isaac_gl_owner_violation(const char *entry)
{
    uint32_t n = __atomic_add_fetch(&s_violations, 1u, __ATOMIC_ACQ_REL);
    uint32_t tid = (uint32_t)sceKernelGetThreadId();
    GlOwnerSlot *slot = gl_owner_slot(entry);
    if (slot) {
        __atomic_add_fetch(&slot->count, 1u, __ATOMIC_ACQ_REL);
        __atomic_store_n(&slot->last_tid, tid, __ATOMIC_RELEASE);
    } else {
        __atomic_add_fetch(&s_slot_overflow, 1u, __ATOMIC_ACQ_REL);
    }
    /* Timeline context for the first ones; the table below names them all. */
    if (n <= 16u)
        isaac_vita_log("KAGE VITA GL OWNER VIOLATION: n=%u entry=%s tid=0x%08x",
                       (unsigned)n, entry, (unsigned)tid);
}

/* Render thread, once per present: every 10 s, one line per entry whose
 * count changed since the last table (nothing when nothing changed). */
void isaac_gl_owner_report(void)
{
    uint64_t now = sceKernelGetProcessTimeWide();
    uint32_t i, changed = 0u;
    if (now - s_last_report_us < GL_OWNER_REPORT_INTERVAL_US)
        return;
    s_last_report_us = now;
    for (i = 0u; i < GL_OWNER_SLOTS; ++i) {
        const char *entry = __atomic_load_n(&s_slots[i].entry, __ATOMIC_ACQUIRE);
        uint32_t count;
        if (!entry)
            break;
        count = __atomic_load_n(&s_slots[i].count, __ATOMIC_ACQUIRE);
        if (count == s_slots[i].reported)
            continue;
        isaac_vita_log("KAGE VITA GL OWNER TABLE: entry=%s count=%u new=%u last_tid=0x%08x",
                       entry, (unsigned)count, (unsigned)(count - s_slots[i].reported),
                       (unsigned)__atomic_load_n(&s_slots[i].last_tid, __ATOMIC_ACQUIRE));
        s_slots[i].reported = count;
        ++changed;
    }
    if (changed)
        isaac_vita_log("KAGE VITA GL OWNER TABLE: total=%u entries_changed=%u overflow=%u",
                       (unsigned)__atomic_load_n(&s_violations, __ATOMIC_ACQUIRE),
                       (unsigned)changed,
                       (unsigned)__atomic_load_n(&s_slot_overflow, __ATOMIC_ACQUIRE));
}

uint32_t isaac_gl_owner_violations(void)
{
    return __atomic_load_n(&s_violations, __ATOMIC_ACQUIRE);
}

#define ISAAC_GL_OWNER_ENTRY(sym) __asm__(                                     \
    "  .pushsection .rodata.isaac_gl_owner_names,\"a\",%progbits\n"            \
    ".Lisaac_gl_owner_name_" #sym ":\n"                                        \
    "  .asciz \"" #sym "\"\n"                                                  \
    "  .popsection\n"                                                          \
    "  .pushsection .text.__wrap_" #sym ",\"ax\",%progbits\n"                  \
    "  .syntax unified\n"                                                      \
    "  .thumb\n"                                                               \
    "  .align 2\n"                                                             \
    "  .global __wrap_" #sym "\n"                                              \
    "  .type __wrap_" #sym ", %function\n"                                     \
    "  .thumb_func\n"                                                          \
    "__wrap_" #sym ":\n"                                                       \
    "  push {r0, lr}\n"                                                        \
    "  mrc p15, 0, r0, c13, c0, 3\n"                                           \
    "  ldr r12, =isaac_gl_owner_tls\n"                                         \
    "  ldr r12, [r12]\n"                                                       \
    "  cmp r12, #0\n"                                                          \
    "  it ne\n"                                                                \
    "  cmpne r12, r0\n"                                                        \
    "  pop {r0, lr}\n"                                                         \
    "  bne 1f\n"                                                               \
    "  b.w __real_" #sym "\n"                                                  \
    "1:\n"                                                                     \
    "  push {r0, r1, r2, r3, r4, lr}\n"                                        \
    "  ldr r0, =.Lisaac_gl_owner_name_" #sym "\n"                              \
    "  bl isaac_gl_owner_violation\n"                                          \
    "  pop {r0, r1, r2, r3, r4, lr}\n"                                         \
    "  b.w __real_" #sym "\n"                                                  \
    "  .ltorg\n"                                                               \
    "  .size __wrap_" #sym ", . - __wrap_" #sym "\n"                           \
    "  .popsection\n");
#include "gl_owner_check_entries.h"
#undef ISAAC_GL_OWNER_ENTRY
