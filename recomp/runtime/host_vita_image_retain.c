/* ImageManager sprite-sheet retention seam.  See host_vita_image_retain.h for
 * the design, the frozen RVAs and the exactness argument.
 *
 * Compiled into the eboot when ISAAC_VITA_IMAGE_RETAIN is ON (with
 * ISAAC_VITA_IMAGE_RETAIN_WRAP=1 and the link's --wrap=sub_0056d8c0) and,
 * from the same source, into the host oracle (ISAAC_VITA_IMAGE_RETAIN_ORACLE)
 * that drives the whole module on a fake identity-mapped guest heap through
 * its own guest_call. */
#include "host_vita_image_retain.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if !defined(ISAAC_VITA_IMAGE_RETAIN)
#error The image-retain seam must only be compiled for its opt-in feature
#endif

#if !defined(ISAAC_VITA_IMAGE_RETAIN_ORACLE)
#include <vitaGL.h>
# if defined(ISAAC_VITA_IMAGE_RETAIN_HEADROOM_PHYCONT)
#  include <psp2/kernel/processmgr.h>
# endif
# if defined(ISAAC_VITA_IMAGE_PRELOAD)
#  include <psp2/kernel/processmgr.h>
# endif
#endif

#if defined(ISAAC_VITA_IMAGE_RETAIN_GHOST)
# if ISAAC_VITA_IMAGE_RETAIN_BUDGET_MB != 0
#  error ISAAC_VITA_IMAGE_RETAIN_GHOST measures the OFF behaviour: BUDGET_MB must be 0
# endif
# if !defined(ISAAC_VITA_IMAGE_RETAIN_ORACLE)
#  include <psp2/kernel/processmgr.h>
#  include <psp2/kernel/sysmem.h>
# endif
static isaac_irg s_irg;
#endif

/* recomp/vita/platform.h; declared here so the host oracle can stub it. */
void isaac_vita_log(const char *format, ...);

#ifndef ISAAC_VITA_IMAGE_RETAIN_BUILD_ID
#define ISAAC_VITA_IMAGE_RETAIN_BUILD_ID "image-retain:unstamped"
#endif
#ifndef ISAAC_VITA_IMAGE_RETAIN_BUDGET_MB
#define ISAAC_VITA_IMAGE_RETAIN_BUDGET_MB 48U
#endif
#ifndef ISAAC_VITA_IMAGE_RETAIN_HEADROOM_MB
#define ISAAC_VITA_IMAGE_RETAIN_HEADROOM_MB 64U
#endif

enum ir_reason {
    IR_REASON_BUDGET = 0,
    IR_REASON_HEADROOM,
    IR_REASON_TABLE_FULL
};

typedef struct ir_record {
    uint32_t image;
    uint32_t ctrl;
    uint32_t bytes;
    uint16_t prev;
    uint16_t next;
    uint16_t hash_next;
    uint16_t in_use;
} ir_record;

static struct {
    ir_record rec[ISAAC_IR_RECORDS];
    uint16_t  hash[ISAAC_IR_HASH];
    uint16_t  head;        /* most-recently-used */
    uint16_t  tail;        /* least-recently-used (evicted first) */
    uint16_t  free_list;
    uint32_t  count;

    uint32_t  image_base;
    uint32_t  buckets_ptr_va;
    uint32_t  hook_global_va, hook_global2_va;
    uint32_t  vt_png, vt_pcx, vt_pic, vt_ctrl;
    uint32_t  vt_mutex;        /* the lock class every replay is pinned to */
    uint32_t  manager_lock_va; /* the manager Mutex object (held-byte probe) */
    uint32_t  budget_bytes;
    uint32_t  headroom_bytes;
    uint32_t  retained_bytes;

    CPU      *cpu;         /* the single game CPU, latched on the first armed call */
    uint32_t  candidate;   /* image the outermost wrapper call is retaining (0 = none) */
    uint32_t  guard;       /* disarmed after an arm: held-lock strong==1 offers still kept */
    uint8_t   armed;
    uint8_t   in_evict;
    uint8_t   env_ready;

    isaac_ir_stats st;
} s_ir;

#define IR_VA(rva) ((uint32_t)(s_ir.image_base + (uint32_t)(rva)))

/* ------------------------------------------------------------- primitives - */

#if defined(ISAAC_VITA_IMAGE_RETAIN_ORACLE)
static uint32_t ir_free_ram(void)  { return isaac_ir_oracle_free_ram(); }
static uint32_t ir_free_vram(void) { return isaac_ir_oracle_free_vram(); }
# if defined(ISAAC_VITA_IMAGE_RETAIN_HEADROOM_PHYCONT)
static uint32_t ir_free_phycont(void) { return isaac_ir_oracle_free_phycont(); }
# endif
#else
#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
/* vitaGL belongs to the GL render thread: its published copy (<= 1 frame). */
#include "kage_vita_backend.h"
static uint32_t ir_free_ram(void)
{ return kage_vita_backend_vgl_mem(KAGE_VITA_VGL_SNAPSHOT_RAM_FREE); }
static uint32_t ir_free_vram(void)
{ return kage_vita_backend_vgl_mem(KAGE_VITA_VGL_SNAPSHOT_VRAM_FREE); }
# if defined(ISAAC_VITA_IMAGE_RETAIN_HEADROOM_PHYCONT)
/* Same published copy for PHYCONT (the slot the ghost sampler below reads);
 * vglMemFree may only run on the render thread under the worker. */
static uint32_t ir_free_phycont(void)
{ return kage_vita_backend_vgl_mem(KAGE_VITA_VGL_SNAPSHOT_PHYCONT_FREE); }
# endif
#else
static uint32_t ir_free_ram(void)  { return (uint32_t)vglMemFree(VGL_MEM_RAM); }
static uint32_t ir_free_vram(void) { return (uint32_t)vglMemFree(VGL_MEM_VRAM); }
# if defined(ISAAC_VITA_IMAGE_RETAIN_HEADROOM_PHYCONT)
/* vitaGL 73dd57a allocates textures VRAM -> RAM -> PHYCONT, so a free PHYCONT
 * byte is as usable for the next room's sheets as a RAM one.  Ordinal after
 * RAM = VGL_MEM_PHYCONT, derived as host_vita_debug_overlay.c does. */
_Static_assert(VGL_MEM_VRAM + 1 == VGL_MEM_RAM && VGL_MEM_RAM + 2 == VGL_MEM_BUDGET,
               "vitaGL memory-domain ABI changed");
static uint32_t ir_free_phycont(void)
{
    return (uint32_t)vglMemFree((vglMemType)(VGL_MEM_RAM + 1));
}
# endif
#endif
#endif

#if defined(ISAAC_VITA_IMAGE_RETAIN_HEADROOM_PHYCONT)
/* One vglMemFree is a sceClibMspaceMallocStats walk: the ghost build measured
 * 1.3-1.6 ms for four free-memory queries, 3.8 ms each during a 456-offer
 * floor exit (p2b-native.log ph120.irm).  Three walks per candidate would add
 * ~0.5 s to a floor exit, so a reading is reused for IR_FREE_CACHE_US.
 * Retaining allocates nothing, so a live re-read inside a burst of offers
 * returns the same value; what a reused reading misses is a foreign upload
 * or free inside the window.  Our own eviction (an unused sheet's texture is
 * freed at once) invalidates it, so headroom_release still sees recovery.
 * shortcut: up to IR_FREE_CACHE_US of foreign uploads (at most one sheet at
 * ~40 ms/MiB decode) are not seen; shorten the window if ph120.irm shows
 * transients deeper than the 16 MiB headroom. */
#define IR_FREE_CACHE_US 4000U
static struct {
    uint64_t at_us;
    uint64_t bytes;
    uint8_t  valid;
    uint32_t samples;
    uint32_t hits;
} s_ir_free;

# if defined(ISAAC_VITA_IMAGE_RETAIN_ORACLE)
static uint64_t ir_now_us(void) { return isaac_ir_oracle_now_us(); }
# else
static uint64_t ir_now_us(void) { return sceKernelGetProcessTimeWide(); }
# endif

static uint64_t ir_free_bytes(void)
{
    uint64_t now = ir_now_us();

    if (s_ir_free.valid && now - s_ir_free.at_us < IR_FREE_CACHE_US) {
        ++s_ir_free.hits;
        return s_ir_free.bytes;
    }
    s_ir_free.bytes = (uint64_t)ir_free_ram() + (uint64_t)ir_free_vram() +
                      (uint64_t)ir_free_phycont();
    s_ir_free.at_us = now;
    s_ir_free.valid = 1U;
    ++s_ir_free.samples;
    return s_ir_free.bytes;
}
#else
static uint64_t ir_free_bytes(void)
{
    return (uint64_t)ir_free_ram() + (uint64_t)ir_free_vram();
}
#endif

/* The control block's embedded lock: this = ctrl + 8, vtable at [this].
 * Returns the vtable only when it is the frozen Mutex class (the one whose
 * Enter/Leave the observer replays and whose held byte the manager probe
 * reads); any other shape is 0 = do not replay. */
static uint32_t ir_lock_vtable(uint32_t ctrl)
{
    uint32_t vtable = ld32(ctrl + ISAAC_IR_CTRL_LOCK);

    if (vtable != s_ir.vt_mutex)
        return 0U;
    if (ld32(vtable + ISAAC_IR_LOCK_VT_ENTER) == 0U ||
            ld32(vtable + ISAAC_IR_LOCK_VT_LEAVE) == 0U)
        return 0U;
    return vtable;
}

/* The manager Mutex object {vtable, u8 init, CS*}: 1 when its shape is the
 * frozen one and it is not initialised-and-free.  Never Unregister while this
 * returns 1: on the single guest thread a set held byte means *this* thread is
 * inside Load/Register/Unregister and Enter would spin forever. */
static int ir_manager_lock_shape_ok(void)
{
    uint32_t lock = s_ir.manager_lock_va;
    uint32_t cs;

    if (ld32(lock) != s_ir.vt_mutex || ld8(lock + ISAAC_IR_MUTEX_INIT) == 0U)
        return 0;
    cs = ld32(lock + ISAAC_IR_MUTEX_CS);
    return cs != 0U && (cs & 3U) == 0U;
}

static int ir_manager_lock_held(void)
{
    if (!ir_manager_lock_shape_ok())
        return 1;
    return ld8(ld32(s_ir.manager_lock_va + ISAAC_IR_MUTEX_CS) + ISAAC_IR_CS_HELD) != 0U;
}

/* Replay the observer's `push -1; mov ecx,lock; call [vt+0xc]` (0x56d8d6..)
 * with its own return word 0x56d8dd, so the sync import census and the
 * sampler see exactly what the translated body would have produced.
 * Mutex::Lock is `ret 4`: ESP returns to its entry value. */
static int ir_guest_lock(CPU *__restrict c, uint32_t ctrl, uint32_t vtable)
{
    uint32_t lock = ctrl + ISAAC_IR_CTRL_LOCK;
    uint32_t saved = c->esp;

    gpush(c, 0xFFFFFFFFU);
    gpush(c, ISAAC_IR_ENTER_SITE_RVA);
    c->ecx = lock;
    guest_call(c, ld32(vtable + ISAAC_IR_LOCK_VT_ENTER));
    if (c->esp != saved) {
        c->esp = saved;
        return 0;
    }
    return 1;
}

/* Replay `mov eax,[lock]; mov ecx,lock; call [eax+0x10]` (0x56d8dd..) with
 * the observer's return word 0x56d8e8.  Mutex::Unlock is a plain `ret`. */
static int ir_guest_unlock(CPU *__restrict c, uint32_t ctrl, uint32_t vtable)
{
    uint32_t lock = ctrl + ISAAC_IR_CTRL_LOCK;
    uint32_t saved = c->esp;

    gpush(c, ISAAC_IR_LEAVE_SITE_RVA);
    c->ecx = lock;
    guest_call(c, ld32(vtable + ISAAC_IR_LOCK_VT_LEAVE));
    if (c->esp != saved) {
        c->esp = saved;
        return 0;
    }
    return 1;
}

/* Replay `push [pair]; call 0x56d220` (0x56d8ee..) with the observer's return
 * word 0x56d8f5.  Unregister is `ret 4`: it pops its return word and the
 * argument, so ESP returns to its entry value; a mismatch is a stack-contract
 * breach. */
static int ir_guest_unregister(CPU *__restrict c, uint32_t image)
{
    uint32_t saved = c->esp;

    gpush(c, image);
    gpush(c, ISAAC_IR_UNREGISTER_SITE_RVA);
    guest_call(c, IR_VA(ISAAC_IR_UNREGISTER_RVA));
    if (c->esp != saved) {
        c->esp = saved;
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------- LRU + hash - */

static uint32_t ir_hash_slot(uint32_t image)
{
    return ((image * 2654435761u) >> 15) & (ISAAC_IR_HASH - 1U);
}

static uint16_t ir_hash_find(uint32_t image)
{
    uint16_t idx = s_ir.hash[ir_hash_slot(image)];

    while (idx != ISAAC_IR_NIL) {
        if (s_ir.rec[idx].image == image)
            return idx;
        idx = s_ir.rec[idx].hash_next;
    }
    return ISAAC_IR_NIL;
}

static void ir_hash_insert(uint16_t idx)
{
    uint32_t slot = ir_hash_slot(s_ir.rec[idx].image);

    s_ir.rec[idx].hash_next = s_ir.hash[slot];
    s_ir.hash[slot] = idx;
}

static void ir_hash_remove(uint16_t idx)
{
    uint32_t slot = ir_hash_slot(s_ir.rec[idx].image);
    uint16_t cur = s_ir.hash[slot];
    uint16_t prev = ISAAC_IR_NIL;

    while (cur != ISAAC_IR_NIL) {
        if (cur == idx) {
            if (prev == ISAAC_IR_NIL)
                s_ir.hash[slot] = s_ir.rec[cur].hash_next;
            else
                s_ir.rec[prev].hash_next = s_ir.rec[cur].hash_next;
            return;
        }
        prev = cur;
        cur = s_ir.rec[cur].hash_next;
    }
}

static void ir_lru_push_head(uint16_t idx)
{
    s_ir.rec[idx].prev = ISAAC_IR_NIL;
    s_ir.rec[idx].next = s_ir.head;
    if (s_ir.head != ISAAC_IR_NIL)
        s_ir.rec[s_ir.head].prev = idx;
    s_ir.head = idx;
    if (s_ir.tail == ISAAC_IR_NIL)
        s_ir.tail = idx;
}

/* Least-recently-used end: for entries recorded without any evidence of use
 * (kept under the manager lock), so they are the first to go. */
static void ir_lru_push_tail(uint16_t idx)
{
    s_ir.rec[idx].next = ISAAC_IR_NIL;
    s_ir.rec[idx].prev = s_ir.tail;
    if (s_ir.tail != ISAAC_IR_NIL)
        s_ir.rec[s_ir.tail].next = idx;
    s_ir.tail = idx;
    if (s_ir.head == ISAAC_IR_NIL)
        s_ir.head = idx;
}

static void ir_lru_unlink(uint16_t idx)
{
    uint16_t p = s_ir.rec[idx].prev;
    uint16_t n = s_ir.rec[idx].next;

    if (p != ISAAC_IR_NIL)
        s_ir.rec[p].next = n;
    else
        s_ir.head = n;
    if (n != ISAAC_IR_NIL)
        s_ir.rec[n].prev = p;
    else
        s_ir.tail = p;
    s_ir.rec[idx].prev = ISAAC_IR_NIL;
    s_ir.rec[idx].next = ISAAC_IR_NIL;
}

static uint16_t ir_alloc_record(void)
{
    uint16_t idx = s_ir.free_list;

    if (idx == ISAAC_IR_NIL)
        return ISAAC_IR_NIL;
    s_ir.free_list = s_ir.rec[idx].next;
    s_ir.rec[idx].next = ISAAC_IR_NIL;
    s_ir.rec[idx].prev = ISAAC_IR_NIL;
    s_ir.rec[idx].hash_next = ISAAC_IR_NIL;
    s_ir.rec[idx].in_use = 1U;
    ++s_ir.count;
    return idx;
}

static void ir_free_record(uint16_t idx)
{
    s_ir.rec[idx].in_use = 0U;
    s_ir.rec[idx].image = 0U;
    s_ir.rec[idx].ctrl = 0U;
    s_ir.rec[idx].bytes = 0U;
    s_ir.rec[idx].prev = ISAAC_IR_NIL;
    s_ir.rec[idx].hash_next = ISAAC_IR_NIL;
    s_ir.rec[idx].next = s_ir.free_list;
    s_ir.free_list = idx;
    if (s_ir.count)
        --s_ir.count;
}

static void ir_account_sub(uint32_t bytes)
{
    if (s_ir.retained_bytes >= bytes)
        s_ir.retained_bytes -= bytes;
    else
        s_ir.retained_bytes = 0U;
}

static void ir_account_add(uint32_t bytes)
{
    s_ir.retained_bytes += bytes;
    if (s_ir.retained_bytes > s_ir.st.peak_bytes)
        s_ir.st.peak_bytes = s_ir.retained_bytes;
}

#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN)
# if !defined(ISAAC_VITA_IMAGE_PRELOAD)
#  error ISAAC_VITA_IMAGE_PRELOAD_PIN needs ISAAC_VITA_IMAGE_PRELOAD
# endif
/* A record left the table: a pin on it goes with it (the preload block). */
static void irpp_dropped(uint32_t image, uint32_t ctrl);
#endif

/* Unlink + unhash + free one record; returns its {image, ctrl, bytes}. */
static void ir_drop_record(uint16_t idx, uint32_t *image, uint32_t *ctrl,
                           uint32_t *bytes)
{
    *image = s_ir.rec[idx].image;
    *ctrl = s_ir.rec[idx].ctrl;
    *bytes = s_ir.rec[idx].bytes;
    ir_lru_unlink(idx);
    ir_hash_remove(idx);
    ir_account_sub(*bytes);
    ir_free_record(idx);
#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN)
    irpp_dropped(*image, *ctrl);
#endif
}

static void ir_reset_records(void)
{
    uint16_t i;

    memset(&s_ir.rec, 0, sizeof s_ir.rec);
    for (i = 0; i < ISAAC_IR_HASH; ++i)
        s_ir.hash[i] = ISAAC_IR_NIL;
    s_ir.head = ISAAC_IR_NIL;
    s_ir.tail = ISAAC_IR_NIL;
    s_ir.free_list = ISAAC_IR_NIL;
    for (i = ISAAC_IR_RECORDS; i-- > 0;) {
        s_ir.rec[i].next = s_ir.free_list;
        s_ir.free_list = i;
    }
    s_ir.count = 0U;
    s_ir.retained_bytes = 0U;
}

/* ---------------------------------------------------------- guest queries - */

static int ir_image_vtable_ok(uint32_t image)
{
    uint32_t vt = ld32(image);

    return vt == s_ir.vt_png || vt == s_ir.vt_pcx || vt == s_ir.vt_pic;
}

#if defined(ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED)
/* ------------------------------------------------------ admit touched ---- */
/* ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED = 1 (OBSERVE) | 2 (ON); undefined is
 * OFF and leaves no token of this block in the unit.
 *
 * Flag 0x10 is membership in the batch renderer's TOUCHED vector (drawn by a
 * flush within the last 5 s, batch buffers still allocated), 0x04 membership
 * in the DIRTY vector (quads queued for the next flush); see the header.  The
 * base module refuses both.  Keeping a 0x10 Image registered is a state the
 * OFF build reaches for any Image another owner still holds: every walker of
 * the TOUCHED vector (flush 0x56d5c1/0x56d801/0x56d88a, sweep 0x561f56 /
 * 0x564fbf, Unregister 0x56d2c2) reads Image fields only, never the control
 * block, and the Image stays alive because the manager's reference stays.
 * Keeping a 0x04 Image is NOT such a state: the next flush would draw quads
 * the OFF build drops in Unregister (0x56d280), so 0x04 stays refused.
 *
 * ON admits a candidate with (flags & 0x14) != 0 only when
 *   T1  0x04 is clear;
 *   T2  the TOUCHED vector is well formed and holds the Image exactly once
 *       and the DIRTY vector does not hold it (the flags tell the truth);
 *   T3  no {batch, Image} pair of it is pending and it is not the Image of
 *       the most recent pair (Unregister would have dropped those draws);
 * and adds rule E: this module never Unregisters a record whose Image has
 * (flags & 0x14) != 0 at that moment.  The OFF build erases a TOUCHED entry
 * only from the Unregister of the Image being released; an LRU eviction runs
 * inside the release of a *different* Image, so without rule E it could
 * shrink the vector under a walker that the OFF build never disturbs.  Such
 * records are skipped (bounded) and become evictable when the game's own
 * sweep clears 0x10, at most 5 s after the last draw.  The byte budget stays
 * a hard cap: when the skipped records leave the table over budget, the
 * candidate itself is Unregistered at once -- the observer's own tail, i.e.
 * exactly what OFF does with it.
 *
 * OBSERVE changes no decision: it runs the same tests and counts what ON
 * would have admitted. */
#if ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED != 1 && \
    ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED != 2
#error ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED must be 1 (OBSERVE) or 2 (ON)
#endif
#define ISAAC_IRT_ON (ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED == 2)
#define ISAAC_IRT_PIN_SKIP_MAX 16U     /* pinned records passed over per eviction */
#define ISAAC_IRT_FLUSH_MASK \
    (ISAAC_IR_IMG_FLAG_DIRTY | ISAAC_IR_IMG_FLAG_TOUCHED)

enum irt_verdict { IRT_OK = 0, IRT_DIRTY, IRT_VEC, IRT_PAIR };

static struct {
    uint32_t offers;         /* unlocked strong == 1 candidates with flags & 0x14 */
    uint32_t admit;          /* ON: retained (new or reuse); OBSERVE: would be */
    uint32_t admit_bytes;    /* texel bytes of the new retentions among them */
    uint32_t dirty;          /* refused: T1 */
    uint32_t vec;            /* refused: T2 */
    uint32_t pair;           /* refused: T3 */
    uint32_t pinned_skip;    /* rule E: records passed over by an eviction walk */
    uint32_t evict_blocked;  /* rule E: an eviction found nothing it may release */
    uint32_t budget_refuse;  /* candidates Unregistered because pins held the budget */
    uint32_t pair_untouched; /* flags & 0x14 == 0 candidates with a pending pair (base retains them) */
} s_irt;

/* Occurrences of `image` in a vector of Image* [*begin_va, *end_va), or -1
 * when the vector is not one this code understands. */
static int irt_vec_count(uint32_t begin_va, uint32_t end_va, uint32_t image)
{
    uint32_t begin = ld32(begin_va);
    uint32_t end = ld32(end_va);
    uint32_t p;
    int n = 0;

    if (begin == end)
        return 0;
    if (begin == 0U || (begin & 3U) != 0U || (end & 3U) != 0U || end < begin ||
            ((end - begin) >> 2) > ISAAC_IR_FLUSH_VEC_MAX)
        return -1;
    for (p = begin; p < end; p += 4U) {
        if (ld32(p) == image)
            ++n;
    }
    return n;
}

/* Pending {batch*, Image*} pairs of `image` (Unregister's scan at 0x56d2f6:
 * the Image is the second word), or -1 for a malformed vector. */
static int irt_pair_count(uint32_t image)
{
    uint32_t begin = ld32(IR_VA(ISAAC_IR_PAIRS_BEGIN_RVA));
    uint32_t end = ld32(IR_VA(ISAAC_IR_PAIRS_END_RVA));
    uint32_t p;
    int n = 0;

    if (begin == end)
        return 0;
    if (begin == 0U || (begin & 3U) != 0U || (end & 7U) != (begin & 7U) ||
            end < begin || ((end - begin) >> 3) > ISAAC_IR_FLUSH_VEC_MAX)
        return -1;
    for (p = begin; p < end; p += 8U) {
        if (ld32(p + 4U) == image)
            ++n;
    }
    return n;
}

static int irt_pair_pending(uint32_t image)
{
    return irt_pair_count(image) != 0 ||
           ld32(IR_VA(ISAAC_IR_LAST_PAIR_IMAGE_RVA)) == image;
}

/* T1..T3 for an Image whose flags word has a flush bit set. */
static enum irt_verdict irt_check(uint32_t image, uint32_t flags)
{
    if ((flags & ISAAC_IR_IMG_FLAG_DIRTY) != 0U)
        return IRT_DIRTY;
    if (irt_vec_count(IR_VA(ISAAC_IR_TOUCHED_BEGIN_RVA),
                      IR_VA(ISAAC_IR_TOUCHED_END_RVA), image) != 1 ||
            irt_vec_count(IR_VA(ISAAC_IR_DIRTY_BEGIN_RVA),
                          IR_VA(ISAAC_IR_DIRTY_END_RVA), image) != 0)
        return IRT_VEC;
    if (irt_pair_pending(image))
        return IRT_PAIR;
    return IRT_OK;
}

/* ir_eligible's remaining tests (texture, name, size), for OBSERVE's
 * would-admit count; the oracle pins OBSERVE's count to ON's. */
static int irt_rest_eligible(uint32_t image, uint32_t *bytes_out)
{
    uint32_t tex0 = ld32(image + ISAAC_IR_IMG_TEX0);
    uint64_t bytes;

    if (tex0 == 0U || tex0 == ISAAC_IR_TEX_UNSET)
        return 0;
    if (ld32(image + ISAAC_IR_IMG_NAME) == 0U)
        return 0;
    bytes = (uint64_t)ld16(image + ISAAC_IR_IMG_PW) *
            (uint64_t)ld16(image + ISAAC_IR_IMG_PH) *
            (uint64_t)ld32(image + ISAAC_IR_IMG_BPP);
    if (bytes == 0U || bytes > (uint64_t)ISAAC_IR_BYTES_CAP)
        return 0;
    *bytes_out = (uint32_t)bytes;
    return 1;
}

/* Count one unlocked candidate.  Returns 1 when it carries a flush bit. */
static int irt_offer(uint32_t image)
{
    uint32_t flags, bytes = 0U;

    if (!ir_image_vtable_ok(image))
        return 0;
    flags = ld32(image + ISAAC_IR_IMG_FLAGS);
    if ((flags & ISAAC_IRT_FLUSH_MASK) == 0U) {
        if (irt_pair_pending(image))
            ++s_irt.pair_untouched;
        return 0;
    }
    ++s_irt.offers;
    switch (irt_check(image, flags)) {
    case IRT_DIRTY: ++s_irt.dirty; break;
    case IRT_VEC:   ++s_irt.vec; break;
    case IRT_PAIR:  ++s_irt.pair; break;
    default:
        if (!ISAAC_IRT_ON && irt_rest_eligible(image, &bytes) &&
                s_ir.budget_bytes != 0U && bytes <= s_ir.budget_bytes) {
            ++s_irt.admit;
            s_irt.admit_bytes += bytes;
        }
        break;
    }
    return 1;
}
#endif

/* Eligibility of an Image the observer is about to Unregister.  Every read is
 * an ld32/ld16 of guest memory the real observer/Unregister would read too. */
static int ir_eligible(uint32_t image, uint32_t *bytes_out)
{
    uint32_t flags, tex0, name, pw, ph, bpp;
    uint64_t bytes;

    if (!ir_image_vtable_ok(image))
        return 0;
    flags = ld32(image + ISAAC_IR_IMG_FLAGS);
#if defined(ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED) && ISAAC_IRT_ON
    if ((flags & 0x14U) != 0U && irt_check(image, flags) != IRT_OK)
        return 0;                          /* T1..T3 */
#else
    if ((flags & 0x14U) != 0U)             /* on a batch-flush list (header) */
        return 0;
#endif
    tex0 = ld32(image + ISAAC_IR_IMG_TEX0);
    if (tex0 == 0U || tex0 == ISAAC_IR_TEX_UNSET) /* upload never happened */
        return 0;
    name = ld32(image + ISAAC_IR_IMG_NAME);
    if (name == 0U)                        /* Load could never hit it */
        return 0;
    pw = ld16(image + ISAAC_IR_IMG_PW);
    ph = ld16(image + ISAAC_IR_IMG_PH);
    bpp = ld32(image + ISAAC_IR_IMG_BPP);
    bytes = (uint64_t)pw * (uint64_t)ph * (uint64_t)bpp;
    if (bytes == 0U || bytes > (uint64_t)ISAAC_IR_BYTES_CAP)
        return 0;
    *bytes_out = (uint32_t)bytes;
    return 1;
}

/* Scan every manager bucket for the exact entry {image, ctrl}.  Bounded by
 * begin/end and a hard 4,096-entry ceiling; every read is an ld32 of guest
 * memory that ImageManager::Load itself reads.  Runs before retention and
 * before eviction, so an entry erased behind the module's back (foreign
 * Unregister/Clear) is detected and no record ever dereferences a dead
 * Image/ctrl. */
static int ir_present_in_manager(uint32_t image, uint32_t ctrl)
{
    uint32_t table = ld32(s_ir.buckets_ptr_va);
    uint32_t scanned = 0U;
    uint32_t b;

    if (table == 0U || (table & 3U) != 0U)
        return 0;
    for (b = 0U; b < ISAAC_IR_BUCKETS; ++b) {
        uint32_t begin = ld32(table + b * ISAAC_IR_BUCKET_STRIDE);
        uint32_t end = ld32(table + b * ISAAC_IR_BUCKET_STRIDE + 4U);
        uint32_t cap = ld32(table + b * ISAAC_IR_BUCKET_STRIDE + 8U);
        uint32_t p;

        if (begin == 0U || end == 0U || (begin & 3U) != 0U || (end & 3U) != 0U)
            continue;
        if (end < begin || cap < end || ((end - begin) & 7U) != 0U)
            return 0;                      /* not a vector we understand */
        for (p = begin; p < end; p += 8U) {
            if (++scanned > ISAAC_IR_MAX_SCAN_ENTRIES)
                return 0;
            if (ld32(p) == image && ld32(p + 4U) == ctrl)
                return 1;
        }
    }
    return 0;
}

#if defined(ISAAC_VITA_IMAGE_RETAIN_BIG_TRACE)
/* ------------------------------------------------------------ big trace --- */
/* Diagnostic (default OFF; OFF leaves no token of this block in the unit).
 * One isaac_vita_log line per decision this module takes about a sheet of at
 * least ISAAC_VITA_IMAGE_RETAIN_BIG_TRACE_MIN_KB KiB (pw * ph * bpp, the
 * module's own accounting unit), with the sheet's ImageManager name, plus
 * per-reason counts of the `ineligible` total for sheets of every size.  It
 * answers "why was this 2 MiB sheet decoded again": a sheet that never shows
 * here was never offered at strong == 1 (it left through a direct Unregister
 * or is not an ImageManager entry at all); one that shows as what=new and is
 * later decoded again without an evict-/stale line was a Load miss, not a
 * retention miss.  No clock is read and nothing is written to guest memory:
 * every read is an ld8/ld16/ld32 of the Image the observer was handed, on
 * the candidate path only (a few calls per second, never the strong != 1 hot
 * path).  The vtable is checked first; an Image of a foreign class is counted
 * (inel-vt) and its other fields are not read. */
#ifndef ISAAC_VITA_IMAGE_RETAIN_BIG_TRACE_MIN_KB
#error ISAAC_VITA_IMAGE_RETAIN_BIG_TRACE needs ISAAC_VITA_IMAGE_RETAIN_BIG_TRACE_MIN_KB
#endif
#define ISAAC_IRB_MIN_BYTES ((uint32_t)(ISAAC_VITA_IMAGE_RETAIN_BIG_TRACE_MIN_KB) * 1024U)
#define ISAAC_IRB_MAX_LINES 512U       /* then counted in `suppressed` */
#define ISAAC_IRB_NAME_MAX  96U        /* the tail of a longer name is kept */
#define ISAAC_IRB_NAME_SCAN 260U       /* as the ghost's name hash */

static struct {
    uint32_t inel_vt, inel_rt, inel_tex, inel_name, inel_bytes;
    uint32_t last_foreign_vt;
    uint32_t lines, suppressed;
} s_irb;

/* The first test of ir_eligible that fails, in ir_eligible's own order, or
 * NULL for an eligible Image.  Kept beside ir_eligible on purpose: the oracle
 * checks the two agree for every refusal. */
static const char *irb_ineligible_reason(uint32_t image)
{
    uint32_t flags, tex0;
    uint64_t bytes;

    if (!ir_image_vtable_ok(image))
        return "inel-vt";
    flags = ld32(image + ISAAC_IR_IMG_FLAGS);
#if defined(ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED) && ISAAC_IRT_ON
    switch ((flags & 0x14U) != 0U ? irt_check(image, flags) : IRT_OK) {
    case IRT_DIRTY: return "inel-dirty";
    case IRT_VEC:   return "inel-fvec";
    case IRT_PAIR:  return "inel-pair";
    default:        break;
    }
#else
    if ((flags & 0x14U) != 0U)
        return "inel-rt";
#endif
    tex0 = ld32(image + ISAAC_IR_IMG_TEX0);
    if (tex0 == 0U || tex0 == ISAAC_IR_TEX_UNSET)
        return "inel-tex";
    if (ld32(image + ISAAC_IR_IMG_NAME) == 0U)
        return "inel-name";
    bytes = (uint64_t)ld16(image + ISAAC_IR_IMG_PW) *
            (uint64_t)ld16(image + ISAAC_IR_IMG_PH) *
            (uint64_t)ld32(image + ISAAC_IR_IMG_BPP);
    if (bytes == 0U || bytes > (uint64_t)ISAAC_IR_BYTES_CAP)
        return "inel-bytes";
    return NULL;
}

/* One line for a sheet at or above the threshold.  `image` must be a live
 * Image the caller was handed or has just found in the manager. */
static void irb_note(uint32_t image, const char *what)
{
    char name[ISAAC_IRB_NAME_MAX + 1U];
    uint32_t pw, ph, bpp, name_va, len = 0U, i, start;
    uint64_t bytes;

    if (!ir_image_vtable_ok(image))
        return;
    pw = ld16(image + ISAAC_IR_IMG_PW);
    ph = ld16(image + ISAAC_IR_IMG_PH);
    bpp = ld32(image + ISAAC_IR_IMG_BPP);
    bytes = (uint64_t)pw * (uint64_t)ph * (uint64_t)bpp;
    if (bytes < (uint64_t)ISAAC_IRB_MIN_BYTES)
        return;
    if (s_irb.lines >= ISAAC_IRB_MAX_LINES) {
        ++s_irb.suppressed;
        return;
    }
    ++s_irb.lines;
    name_va = ld32(image + ISAAC_IR_IMG_NAME);
    if (name_va != 0U) {
        while (len < ISAAC_IRB_NAME_SCAN && ld8(name_va + len) != 0U)
            ++len;
    }
    start = len > ISAAC_IRB_NAME_MAX ? len - ISAAC_IRB_NAME_MAX : 0U;
    for (i = 0U; start + i < len; ++i) {
        uint8_t ch = (uint8_t)ld8(name_va + start + i);

        name[i] = (ch > 0x20U && ch < 0x7fU) ? (char)ch : '?';
    }
    name[i] = '\0';
    isaac_vita_log(
        "KAGE VITA IMAGE RETAIN BIG: what=%s image=%08x bytes=%u dim=%ux%ux%u "
        "flags=%08x tex0=%08x live=%u/%u n=%u name=%s",
        what, (unsigned)image,
        (unsigned)(bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : bytes),
        (unsigned)pw, (unsigned)ph, (unsigned)bpp,
        (unsigned)ld32(image + ISAAC_IR_IMG_FLAGS),
        (unsigned)ld32(image + ISAAC_IR_IMG_TEX0),
        (unsigned)s_ir.count, (unsigned)s_ir.retained_bytes,
        (unsigned)s_irb.lines, name);
}

/* The `ineligible` exit of the wrapper: count the reason, name a big sheet. */
static void irb_ineligible(uint32_t image)
{
    const char *why = irb_ineligible_reason(image);

    if (why == NULL)
        return;
    switch (why[5]) {                      /* "inel-<x>": v r t n b (d f p) */
    case 'v': ++s_irb.inel_vt; s_irb.last_foreign_vt = ld32(image); return;
#if defined(ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED) && ISAAC_IRT_ON
    case 'd': case 'f': case 'p':          /* the flush-list refusals */
#endif
    case 'r': ++s_irb.inel_rt; break;
    case 't': ++s_irb.inel_tex; break;
    case 'n': ++s_irb.inel_name; break;
    default:  ++s_irb.inel_bytes; break;
    }
    irb_note(image, why);
}

/* A retained record leaves the table without the Image being read (it may be
 * dead): bytes come from the record. */
static void irb_note_record(uint32_t image, uint32_t bytes, const char *what)
{
    if (bytes < ISAAC_IRB_MIN_BYTES)
        return;
    if (s_irb.lines >= ISAAC_IRB_MAX_LINES) {
        ++s_irb.suppressed;
        return;
    }
    ++s_irb.lines;
    isaac_vita_log(
        "KAGE VITA IMAGE RETAIN BIG: what=%s image=%08x bytes=%u live=%u/%u n=%u",
        what, (unsigned)image, (unsigned)bytes,
        (unsigned)s_ir.count, (unsigned)s_ir.retained_bytes,
        (unsigned)s_irb.lines);
}
/* What ir_touch is about to do with an accepted candidate. */
static const char *irb_touch_what(uint32_t image, uint32_t ctrl)
{
    uint16_t idx = ir_hash_find(image);

    return (idx != ISAAC_IR_NIL && s_ir.rec[idx].ctrl == ctrl) ? "reuse" : "new";
}
#define IRB_NOTE(image, what)                irb_note((image), (what))
#define IRB_INELIGIBLE(image)                irb_ineligible((image))
#define IRB_NOTE_RECORD(image, bytes, what)  irb_note_record((image), (bytes), (what))
#else
#define IRB_NOTE(image, what)                ((void)0)
#define IRB_INELIGIBLE(image)                ((void)0)
#define IRB_NOTE_RECORD(image, bytes, what)  ((void)0)
#endif

#if defined(ISAAC_VITA_IMAGE_PRELOAD)
/* -------------------------------------------------------------- preload --- */
/* ISAAC_VITA_IMAGE_PRELOAD (default OFF; OFF leaves no token of this block in
 * the unit).  At the end of Level::Init (the SFX floor preload's
 * --wrap=sub_00311e60 seam calls isaac_vita_image_preload_level_init) every
 * name of ISAAC_VITA_IMAGE_PRELOAD_LIST that is not in the ImageManager yet
 * is loaded by replaying the anm2 sprite-sheet loader's own call site
 * (0x565fad..0x566015): ImageManager::Load(&out, name, 0xbbf948, 0) and then
 * the SmartPointer<Image> destructor of `out` -- ctrl->Release(), and the
 * release observer when it returns true.  That observer call is an ordinary
 * strong == 1 offer of an undrawn sheet (flags 0x01), so the retention seam
 * above records it exactly like a sheet whose last sprite died, and the
 * game's first real Load of the name is a strcmp hit instead of a PNG decode
 * in the middle of a fight.  Nothing is pinned and no reference is held: the
 * preloaded sheet is an LRU record like any other and can be evicted before
 * it is used (counted as `evicted_unused`).
 *
 * A name is skipped, with its reason counted, unless retention is armed with
 * a non-zero budget on this CPU and idle (not inside an eviction, manager
 * Mutex free), the guest heap is not terminal, MAX_KB fits the budget, the
 * vitaGL free memory keeps the retention headroom after MAX_KB more, the
 * guest heap keeps RESERVE_MB + 2 x MAX_KB (decode scratch; a model, see the
 * README; the headroom is read AFTER the SFX floor and fight preloads of
 * the same Level::Init, which on the bench route leaves less than the
 * default gate needs -- README, "heap gate on the bench route") and the
 * guest stack has room for the replay frame.  A sheet the
 * retention seam then refuses (headroom, eligibility) is freed by the real
 * observer inside the same replay and counted as `dropped`. */
#if !defined(ISAAC_VITA_IMAGE_PRELOAD_LIST) || \
    !defined(ISAAC_VITA_IMAGE_PRELOAD_MAX_KB) || \
    !defined(ISAAC_VITA_IMAGE_PRELOAD_RESERVE_MB)
#error ISAAC_VITA_IMAGE_PRELOAD needs _LIST, _MAX_KB and _RESERVE_MB
#endif
#define ISAAC_IRP_MAX         8U       /* list entries */
#define ISAAC_IRP_NAME_MAX    95U      /* characters per entry */
#define ISAAC_IRP_NAME_SCAN   260U     /* as the big trace */
#define ISAAC_IRP_KEY_MAX     63U      /* tail of the resolved name in the receipt */
#define ISAAC_IRP_SCRATCH     0x80U    /* out pair + name, below the caller's ESP */
#define ISAAC_IRP_HEAP_FACTOR 2U       /* guest-heap decode scratch, in MAX_KB */
_Static_assert(8U + ISAAC_IRP_NAME_MAX + 1U <= ISAAC_IRP_SCRATCH,
               "the preload scratch frame holds the out pair and one name");

/* The allocator's terminal flag (host_vita_heap.c; the host oracle supplies
 * it, like the guest-heap headroom the header declares). */
int isaac_vita_guest_heap_terminal(void);
#if defined(ISAAC_VITA_IMAGE_RETAIN_ORACLE)
static uint64_t irp_now_us(void) { return isaac_irp_oracle_now_us(); }
#else
static uint64_t irp_now_us(void) { return sceKernelGetProcessTimeWide(); }
#endif

enum { IRP_IDLE = 0, IRP_WAITING, IRP_USED };
enum irp_skip {
    IRP_SKIP_STATE = 0,   /* retention not armed / budget 0 / other CPU / evicting */
    IRP_SKIP_LOCK,        /* manager Mutex held */
    IRP_SKIP_TERMINAL,    /* guest heap terminal */
    IRP_SKIP_VMEM,        /* MAX_KB over the budget or under the vitaGL headroom */
    IRP_SKIP_HEAP,        /* guest heap below RESERVE_MB + 2 x MAX_KB */
    IRP_SKIP_STACK,       /* no room for the replay frame */
    IRP_SKIP_N
};

static struct {
    char     text[ISAAC_IRP_MAX][ISAAC_IRP_NAME_MAX + 1U];
    uint8_t  len[ISAAC_IRP_MAX];
    uint8_t  state[ISAAC_IRP_MAX];
    uint32_t image[ISAAC_IRP_MAX];
    uint32_t ctrl[ISAAC_IRP_MAX];
    uint32_t n;
    uint8_t  parsed;
    uint8_t  truncated;       /* the list had more / longer entries than fit */
    uint8_t  busy;            /* inside the replay: its own offer is not a hit/miss */

    uint32_t levels;          /* Level::Init returns seen */
    uint32_t loaded;          /* Load replays that returned an Image */
    uint32_t loaded_bytes;    /* their pw * ph * bpp */
    uint32_t load_us;         /* wall time inside the replays */
    uint32_t retained;        /* ... recorded by the retention seam */
    uint32_t dropped;         /* ... freed again by the real observer */
    uint32_t present;         /* names already in the manager */
    uint32_t fail;            /* Load returned no Image */
    uint32_t skip[IRP_SKIP_N];
    uint32_t hit;             /* preloaded sheets later re-offered (they were used) */
    uint32_t miss;            /* offers of a list sheet the preload did not provide */
    uint32_t evicted_unused;  /* preloaded sheets evicted before any use */
    uint32_t breach;          /* replay stack-contract breaches */
} s_irp;

static void irp_parse(void)
{
    static const char list[] = ISAAC_VITA_IMAGE_PRELOAD_LIST;
    size_t i = 0U;

    if (s_irp.parsed)
        return;
    s_irp.parsed = 1U;
    while (list[i] != '\0') {
        size_t start = i, len;

        while (list[i] != '\0' && list[i] != ',')
            ++i;
        len = i - start;
        if (len != 0U) {
            if (len > ISAAC_IRP_NAME_MAX || s_irp.n >= ISAAC_IRP_MAX) {
                s_irp.truncated = 1U;
            } else {
                memcpy(s_irp.text[s_irp.n], list + start, len);
                s_irp.text[s_irp.n][len] = '\0';
                s_irp.len[s_irp.n] = (uint8_t)len;
                ++s_irp.n;
            }
        }
        if (list[i] == ',')
            ++i;
    }
}

static uint8_t irp_fold(uint8_t ch)
{
    if (ch >= (uint8_t)'A' && ch <= (uint8_t)'Z')
        return (uint8_t)(ch + 32U);
    return ch == (uint8_t)'\\' ? (uint8_t)'/' : ch;
}

/* 1 when the Image's ImageManager name is list entry `i`, or ends in
 * "/<entry>" (Load keys a game file as "resources/<name>"); ASCII case and
 * the slash direction are folded. */
static int irp_name_is(uint32_t image, uint32_t i)
{
    uint32_t name = ld32(image + ISAAC_IR_IMG_NAME);
    uint32_t len = 0U, want = s_irp.len[i], k;

    if (name == 0U)
        return 0;
    while (len < ISAAC_IRP_NAME_SCAN && ld8(name + len) != 0U)
        ++len;
    if (len < want)
        return 0;
    if (len > want && irp_fold((uint8_t)ld8(name + len - want - 1U)) != (uint8_t)'/')
        return 0;
    for (k = 0U; k < want; ++k) {
        if (irp_fold((uint8_t)ld8(name + len - want + k)) !=
                irp_fold((uint8_t)s_irp.text[i][k]))
            return 0;
    }
    return 1;
}

/* The first manager entry whose name is list entry `i`. */
static int irp_find_in_manager(uint32_t i, uint32_t *image_out, uint32_t *ctrl_out)
{
    uint32_t table = ld32(s_ir.buckets_ptr_va);
    uint32_t scanned = 0U;
    uint32_t b;

    if (table == 0U || (table & 3U) != 0U)
        return 0;
    for (b = 0U; b < ISAAC_IR_BUCKETS; ++b) {
        uint32_t begin = ld32(table + b * ISAAC_IR_BUCKET_STRIDE);
        uint32_t end = ld32(table + b * ISAAC_IR_BUCKET_STRIDE + 4U);
        uint32_t p;

        if (begin == 0U || end == 0U || (begin & 3U) != 0U || (end & 3U) != 0U)
            continue;
        if (end < begin || ((end - begin) & 7U) != 0U)
            return 0;
        for (p = begin; p < end; p += 8U) {
            uint32_t image = ld32(p);

            if (++scanned > ISAAC_IR_MAX_SCAN_ENTRIES)
                return 0;
            if (image == 0U || (image & 3U) != 0U || !ir_image_vtable_ok(image))
                continue;
            if (irp_name_is(image, i)) {
                *image_out = image;
                *ctrl_out = ld32(p + 4U);
                return 1;
            }
        }
    }
    return 0;
}

#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN)
/* ISAAC_VITA_IMAGE_PRELOAD_PIN (default OFF; needs IMAGE_PRELOAD; 1 = OBSERVE,
 * 2 = ON; OFF leaves no token of this block in the unit).
 *
 * The device run that armed the preload (IA, 12.11) logged present=1 on
 * levels 1, 2 and 3 -- irp_one found a manager entry under the name and did
 * nothing -- and the fight still decoded gfx/BulletAtlas.png.  "Present" can
 * be useless in two ways: the entry is not the key the fight's Load asks for
 * (irp_name_is is a folded path-suffix match, so any "<dir>/gfx/BulletAtlas.png"
 * passes), or it is the right entry and leaves the LRU (or the manager)
 * between the floor load and the fight.
 *
 * OBSERVE changes no decision.  It prints what "present" was -- every match
 * (the first ISAAC_IRPP_SET_MAX per name and level) with its strong count,
 * flags, bytes, whether the retention seam holds a record of it and the tail
 * of its key -- and, on a miss, how many manager entries carry the name
 * (twins >= 2: some Load registered a second copy of a key that was there).
 *
 * ON:
 *  - a present name is replayed through Load anyway, behind the same memory
 *    gates.  Load's own lookup decides: it returns one of the matches
 *    (`exact`, nothing was decoded) or a new Image (`alias`: the match was
 *    another key; the sheet was decoded and handed to the retention seam);
 *    the unused sheet a pin held before is re-pinned without a replay (`own`);
 *  - the sheet the replay leaves retained is pinned: an eviction walks past
 *    it until the game has used it (a Load hit, counted `used` when it lets
 *    go), until the next Level::Init (`floor`: every pin is released there and
 *    re-taken by that level's walk), or until its record goes (`dropped`,
 *    `stale`);
 *  - an exact match the game still holds (strong >= 2 after the replay) is
 *    live -- Load hits it for as long as that lasts -- and is pinned when its
 *    last holder lets go, if the retention seam records it then (`on_free`,
 *    also when that release is kept under the manager Mutex; otherwise
 *    `lost` with the exit it took, e.g. exit=ineligible for flags 0x10 with
 *    ADMIT_TOUCHED OFF; a release the game takes back before the lock
 *    replay is neither: the sheet stays held);
 *  - pins hold at most budget / ISAAC_IRPP_SHARE bytes (`refused`) and never
 *    block an eviction: when every record an eviction can reach is pinned,
 *    the oldest pin yields (`yield`).  The byte budget, the headroom release
 *    and the table bound therefore hold exactly as without pins; only the
 *    order in which LRU records leave changes.  A headroom release never
 *    takes the record of the sheet being offered (it is still its caller's
 *    there); a budget eviction takes it only after every other pin yielded,
 *    as OFF's tail walk would (`cskip` counts the walks that passed it and
 *    took another record or none). */
#if ISAAC_VITA_IMAGE_PRELOAD_PIN != 1 && ISAAC_VITA_IMAGE_PRELOAD_PIN != 2
#error ISAAC_VITA_IMAGE_PRELOAD_PIN must be 1 (OBSERVE) or 2 (ON)
#endif
#define ISAAC_IRPP_ON        (ISAAC_VITA_IMAGE_PRELOAD_PIN == 2)
#define ISAAC_IRPP_SHARE     2U    /* pins hold at most budget / SHARE bytes */
#define ISAAC_IRPP_SET_MAX   4U    /* matches remembered and printed per name */
#define ISAAC_IRPP_LINES_MAX 64U   /* present / miss / lost lines per arm */

enum { IRP_ON_FREE = IRP_USED + 1 };   /* the game holds the exact sheet */
enum irpp_why { IRPP_USED = 0, IRPP_FLOOR, IRPP_YIELD, IRPP_STALE, IRPP_DROPPED,
                IRPP_WHY_N };

static struct {
    uint8_t  pin[ISAAC_IRP_MAX];
    uint32_t pin_bytes[ISAAC_IRP_MAX];
    uint32_t pinned_bytes;          /* sum of pin_bytes[] over the live pins */
    uint32_t set_image[ISAAC_IRPP_SET_MAX];
    uint32_t set_ctrl[ISAAC_IRPP_SET_MAX];
    uint32_t set_n;                 /* matches of the name being replayed */
    uint32_t pend;                  /* 1 + entry whose held sheet is being let go */
    uint32_t pend_image, pend_ctrl, pend_flags;
    uint32_t pend_st[4];            /* s_ir.st headroom_pass oversize ineligible absent then */
    uint32_t lines;

    uint32_t pinned;                /* pins taken */
    uint32_t refused;               /* ... refused by the share cap */
    uint32_t unpin[IRPP_WHY_N];     /* pins released, by reason */
    uint32_t skip;                  /* pinned records an eviction walked past */
    uint32_t cand_skip;             /* ... the candidate's own record it walked past */
    uint32_t own;                   /* present names re-pinned without a replay */
    uint32_t exact;                 /* ON replays of a present name that returned a match */
    uint32_t alias;                 /* ... that returned a new Image */
    uint32_t held;                  /* ... whose match the game held */
    uint32_t on_free;               /* held sheets pinned when the game let go */
    uint32_t lost;                  /* held sheets the retention seam did not record */
    uint32_t twins;                 /* misses with >= 2 manager entries under the name */
    uint32_t lv[6];                 /* level-begin copies: floor pinned own exact alias held */
} s_irpp;

/* The record of {image, ctrl}, or NIL. */
static uint16_t irpp_record(uint32_t image, uint32_t ctrl)
{
    uint16_t idx = ir_hash_find(image);

    return idx != ISAAC_IR_NIL && s_ir.rec[idx].ctrl == ctrl ? idx : ISAAC_IR_NIL;
}

static uint32_t irpp_bytes(uint32_t image)
{
    uint64_t bytes = (uint64_t)ld16(image + ISAAC_IR_IMG_PW) *
                     (uint64_t)ld16(image + ISAAC_IR_IMG_PH) *
                     (uint64_t)ld32(image + ISAAC_IR_IMG_BPP);

    return bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)bytes;
}

/* The printable tail of an Image's manager key. */
static void irpp_key(uint32_t image, char key[ISAAC_IRP_KEY_MAX + 1U])
{
    uint32_t name = ld32(image + ISAAC_IR_IMG_NAME);
    uint32_t len = 0U, first, k;

    if (name != 0U) {
        while (len < ISAAC_IRP_NAME_SCAN && ld8(name + len) != 0U)
            ++len;
    }
    first = len > ISAAC_IRP_KEY_MAX ? len - ISAAC_IRP_KEY_MAX : 0U;
    for (k = 0U; first + k < len; ++k) {
        uint8_t ch = (uint8_t)ld8(name + first + k);

        key[k] = (ch >= 0x21U && ch <= 0x7eU) ? (char)ch : '?';
    }
    key[k] = '\0';
}

/* Every manager entry whose name is list entry `i`: the first
 * ISAAC_IRPP_SET_MAX go to set_image / set_ctrl; returns how many there are. */
static uint32_t irpp_scan(uint32_t i)
{
    uint32_t table = ld32(s_ir.buckets_ptr_va);
    uint32_t scanned = 0U, matched = 0U, b;

    s_irpp.set_n = 0U;
    if (table == 0U || (table & 3U) != 0U)
        return 0U;
    for (b = 0U; b < ISAAC_IR_BUCKETS; ++b) {
        uint32_t first = ld32(table + b * ISAAC_IR_BUCKET_STRIDE);
        uint32_t last = ld32(table + b * ISAAC_IR_BUCKET_STRIDE + 4U);
        uint32_t p;

        if (first == 0U || last == 0U || ((first | last) & 3U) != 0U)
            continue;
        if (last < first || ((last - first) & 7U) != 0U)
            continue;                      /* a malformed bucket hides no twin of the next */
        for (p = first; p < last; p += 8U) {
            uint32_t image = ld32(p);

            if (++scanned > ISAAC_IR_MAX_SCAN_ENTRIES)
                return matched;
            if (image == 0U || (image & 3U) != 0U || !ir_image_vtable_ok(image) ||
                    !irp_name_is(image, i))
                continue;
            if (s_irpp.set_n < ISAAC_IRPP_SET_MAX) {
                s_irpp.set_image[s_irpp.set_n] = image;
                s_irpp.set_ctrl[s_irpp.set_n] = ld32(p + 4U);
                ++s_irpp.set_n;
            }
            ++matched;
        }
    }
    return matched;
}

static void irpp_unpin(uint32_t i, enum irpp_why why)
{
    if (!s_irpp.pin[i])
        return;
    s_irpp.pin[i] = 0U;
    s_irpp.pinned_bytes = s_irpp.pinned_bytes >= s_irpp.pin_bytes[i] ?
                          s_irpp.pinned_bytes - s_irpp.pin_bytes[i] : 0U;
    s_irpp.pin_bytes[i] = 0U;
    ++s_irpp.unpin[why];
}

/* Pin the sheet entry `i` waits with (ON; it must have a record). */
static void irpp_pin(uint32_t i)
{
    uint16_t idx = irpp_record(s_irp.image[i], s_irp.ctrl[i]);
    uint32_t bytes;

    if (!ISAAC_IRPP_ON || s_irpp.pin[i] || s_irp.state[i] != IRP_WAITING ||
            idx == ISAAC_IR_NIL)
        return;
    bytes = s_ir.rec[idx].bytes;
    if ((uint64_t)s_irpp.pinned_bytes + bytes > s_ir.budget_bytes / ISAAC_IRPP_SHARE) {
        ++s_irpp.refused;
        return;
    }
    s_irpp.pin[i] = 1U;
    s_irpp.pin_bytes[i] = bytes;
    s_irpp.pinned_bytes += bytes;
    ++s_irpp.pinned;
}

static void irpp_dropped(uint32_t image, uint32_t ctrl)
{
    uint32_t i;

    for (i = 0U; i < s_irp.n; ++i) {
        if (s_irpp.pin[i] && s_irp.image[i] == image && s_irp.ctrl[i] == ctrl)
            irpp_unpin(i, IRPP_DROPPED);
    }
}

static uint32_t irpp_pins(void)
{
    uint32_t i, n = 0U;

    for (i = 0U; i < s_irp.n; ++i)
        n += s_irpp.pin[i] ? 1U : 0U;
    return n;
}

/* Which ir_wrap exit the held sheet's release took instead of a record:
 * the first of the counters it moves that moved since irpp_on_free. */
static const char *irpp_exit(void)
{
    if (s_ir.st.headroom_pass != s_irpp.pend_st[0])
        return "pressure";
    if (s_ir.st.oversize != s_irpp.pend_st[1])
        return "oversize";
    if (s_ir.st.ineligible != s_irpp.pend_st[2])
        return "ineligible";
    if (s_ir.st.absent != s_irpp.pend_st[3])
        return "absent";
    return "pass";                         /* budget 0, lock shape, table full */
}

/* A held sheet the game let go of was not recorded by the retention seam.
 * Still registered: the game took it back between the unlocked pre-check
 * and the lock replay (the observer's `reacquired` no-op) -- it stays
 * ON_FREE and its next release is the one that pins it.  Gone: the
 * observer freed it (ineligible, oversize, headroom pressure, table); Load
 * will decode it again (`lost`, with the exit it took). */
static void irpp_settle(void)
{
    uint32_t i;

    if (s_irpp.pend == 0U)
        return;
    i = s_irpp.pend - 1U;
    s_irpp.pend = 0U;
    if (s_irp.state[i] != IRP_ON_FREE || s_irp.image[i] != s_irpp.pend_image ||
            s_irp.ctrl[i] != s_irpp.pend_ctrl ||
            ir_present_in_manager(s_irpp.pend_image, s_irpp.pend_ctrl))
        return;
    ++s_irpp.lost;
    if (s_irpp.lines < ISAAC_IRPP_LINES_MAX) {
        ++s_irpp.lines;
        isaac_vita_log("KAGE VITA IMAGE PRELOAD PIN: lost name=%s level=%u "
                       "image=%08x flags=%08x lost=%u exit=%s",
                       s_irp.text[i], (unsigned)s_irp.levels,
                       (unsigned)s_irpp.pend_image, (unsigned)s_irpp.pend_flags,
                       (unsigned)s_irpp.lost, irpp_exit());
    }
    s_irp.state[i] = IRP_IDLE;
    s_irp.image[i] = 0U;
    s_irp.ctrl[i] = 0U;
}

/* irp_offer: the game lets go of the held exact sheet of entry `i`. */
static void irpp_on_free(uint32_t i, uint32_t image, uint32_t ctrl)
{
    s_irpp.pend = i + 1U;
    s_irpp.pend_image = image;
    s_irpp.pend_ctrl = ctrl;
    s_irpp.pend_flags = ld32(image + ISAAC_IR_IMG_FLAGS);
    s_irpp.pend_st[0] = s_ir.st.headroom_pass;
    s_irpp.pend_st[1] = s_ir.st.oversize;
    s_irpp.pend_st[2] = s_ir.st.ineligible;
    s_irpp.pend_st[3] = s_ir.st.absent;
}

/* ir_wrap recorded {image, ctrl}: pin it when it is the held sheet let go. */
static void irpp_touched(uint32_t image, uint32_t ctrl)
{
    uint32_t i;

    if (s_irpp.pend == 0U || s_irpp.pend_image != image || s_irpp.pend_ctrl != ctrl)
        return;
    if (irpp_record(image, ctrl) == ISAAC_IR_NIL) {
        irpp_settle();
        return;
    }
    i = s_irpp.pend - 1U;
    s_irpp.pend = 0U;
    s_irp.state[i] = IRP_WAITING;
    ++s_irpp.on_free;
    irpp_pin(i);
}

/* ir_keep kept a strong == 1 offer of {image, ctrl} (the manager Mutex was
 * held, or the offer is nested in our own Unregister): irp_offer did not see
 * it.  When it is a held sheet's last release and the retention seam holds a
 * record of it, pin it here as irpp_touched would; otherwise it stays
 * ON_FREE (the sheet is still the manager's, nothing is lost). */
static void irpp_kept(uint32_t image, uint32_t ctrl)
{
    uint32_t i;

    if (s_irp.busy || irpp_record(image, ctrl) == ISAAC_IR_NIL)
        return;
    for (i = 0U; i < s_irp.n; ++i) {
        if (s_irp.state[i] != IRP_ON_FREE || s_irp.image[i] != image ||
                s_irp.ctrl[i] != ctrl)
            continue;
        if (s_irpp.pend == i + 1U)
            s_irpp.pend = 0U;
        s_irp.state[i] = IRP_WAITING;
        ++s_irpp.on_free;
        irpp_pin(i);
    }
}

/* irp_offer counted a miss: how many manager entries carry the name. */
static void irpp_miss(uint32_t i, uint32_t image)
{
    uint32_t twins = irpp_scan(i);
    char key[ISAAC_IRP_KEY_MAX + 1U];

    s_irpp.set_n = 0U;
    if (twins >= 2U)
        ++s_irpp.twins;
    if (s_irpp.lines < ISAAC_IRPP_LINES_MAX) {
        ++s_irpp.lines;
        irpp_key(image, key);
        isaac_vita_log("KAGE VITA IMAGE PRELOAD PIN: miss name=%s level=%u "
                       "image=%08x flags=%08x state=%u pinned=%u twins=%u key=%s",
                       s_irp.text[i], (unsigned)s_irp.levels, (unsigned)image,
                       (unsigned)ld32(image + ISAAC_IR_IMG_FLAGS),
                       (unsigned)s_irp.state[i], (unsigned)s_irpp.pin[i],
                       (unsigned)twins, key);
    }
}

/* irp_one found list entry `i` in the manager.  Returns 1 when the walk is
 * done with it (OBSERVE: always, the OFF decision), 0 when ON replays it. */
static int irpp_present(uint32_t i)
{
    const uint32_t matched = irpp_scan(i);
    uint32_t k, own = 0U;
    char key[ISAAC_IRP_KEY_MAX + 1U];

    for (k = 0U; k < s_irpp.set_n; ++k) {
        const uint32_t image = s_irpp.set_image[k], ctrl = s_irpp.set_ctrl[k];
        const uint16_t idx = irpp_record(image, ctrl);
        const uint32_t strong = ld16(ctrl + ISAAC_IR_CTRL_STRONG);
        const uint32_t mine = s_irp.state[i] == IRP_WAITING &&
                              s_irp.image[i] == image && s_irp.ctrl[i] == ctrl;

        if (mine && idx != ISAAC_IR_NIL && strong == 1U)
            own = 1U;
        if (s_irpp.lines < ISAAC_IRPP_LINES_MAX) {
            ++s_irpp.lines;
            irpp_key(image, key);
            isaac_vita_log("KAGE VITA IMAGE PRELOAD PIN: present name=%s level=%u "
                           "k=%u/%u image=%08x strong=%u flags=%08x bytes=%u "
                           "rec=%u own=%u key=%s",
                           s_irp.text[i], (unsigned)s_irp.levels, (unsigned)(k + 1U),
                           (unsigned)matched, (unsigned)image, (unsigned)strong,
                           (unsigned)ld32(image + ISAAC_IR_IMG_FLAGS),
                           (unsigned)irpp_bytes(image),
                           (unsigned)(idx != ISAAC_IR_NIL), (unsigned)mine, key);
        }
    }
    if (!ISAAC_IRPP_ON) {
        s_irpp.set_n = 0U;
        return 1;
    }
    if (own) {
        /* The sheet an earlier walk left retained, unused: Load would hand
         * back exactly this Image. */
        ++s_irpp.own;
        irpp_pin(i);
        s_irpp.set_n = 0U;
        return 1;
    }
    return 0;
}

/* After a replay of entry `i` that returned {image, ctrl}; `result` is the
 * receipt word irp_one chose.  Returns the word to print. */
static const char *irpp_classify(uint32_t i, uint32_t image, uint32_t ctrl,
                                 const char *result)
{
    uint32_t k, exact = 0U, kept;

    if (image == 0U || ctrl == 0U) {
        s_irpp.set_n = 0U;
        return result;
    }
    for (k = 0U; k < s_irpp.set_n; ++k) {
        if (s_irpp.set_image[k] == image && s_irpp.set_ctrl[k] == ctrl)
            exact = 1U;
    }
    if (s_irpp.set_n != 0U) {
        if (exact)
            ++s_irpp.exact;
        else
            ++s_irpp.alias;
    }
    s_irpp.set_n = 0U;
    /* irp_one's `retained` test, restated on the state it left. */
    kept = s_irp.state[i] == IRP_WAITING && s_irp.image[i] == image &&
           s_irp.ctrl[i] == ctrl && irpp_record(image, ctrl) != ISAAC_IR_NIL &&
           ir_present_in_manager(image, ctrl);
    if (!ISAAC_IRPP_ON)
        return result;
    if (ld16(ctrl + ISAAC_IR_CTRL_STRONG) >= 2U && (exact || kept)) {
        /* The game holds it: live, Load hits it for as long as that lasts.
         * Its last holder letting go is not a use of the preload. */
        if (!kept && s_irp.dropped != 0U)
            --s_irp.dropped;               /* irp_one counted it; nothing was freed */
        s_irp.state[i] = IRP_ON_FREE;
        s_irp.image[i] = image;
        s_irp.ctrl[i] = ctrl;
        ++s_irpp.held;
        return "held";
    }
    if (kept)
        irpp_pin(i);
    return result;
}

/* Level::Init: every pin is released (the walk re-takes what it still finds
 * unused); a held sheet's pending pin is forgotten. */
static void irpp_level_begin(void)
{
    uint32_t i;

    irpp_settle();
    s_irpp.lv[0] = s_irpp.unpin[IRPP_FLOOR];
    s_irpp.lv[1] = s_irpp.pinned;
    s_irpp.lv[2] = s_irpp.own;
    s_irpp.lv[3] = s_irpp.exact;
    s_irpp.lv[4] = s_irpp.alias;
    s_irpp.lv[5] = s_irpp.held;
    for (i = 0U; i < s_irp.n; ++i) {
        irpp_unpin(i, IRPP_FLOOR);
        if (s_irp.state[i] == IRP_ON_FREE) {
            s_irp.state[i] = IRP_IDLE;
            s_irp.image[i] = 0U;
            s_irp.ctrl[i] = 0U;
        }
    }
}

static void irpp_level_end(void)
{
    isaac_vita_log(
        "KAGE VITA IMAGE PRELOAD PIN: level=%u mode=%s pins=%u pinned_bytes=%u "
        "share=%u released=%u pinned=%u own=%u exact=%u alias=%u held=%u",
        (unsigned)s_irp.levels, ISAAC_IRPP_ON ? "on" : "observe",
        (unsigned)irpp_pins(), (unsigned)s_irpp.pinned_bytes,
        (unsigned)(s_ir.budget_bytes / ISAAC_IRPP_SHARE),
        (unsigned)(s_irpp.unpin[IRPP_FLOOR] - s_irpp.lv[0]),
        (unsigned)(s_irpp.pinned - s_irpp.lv[1]),
        (unsigned)(s_irpp.own - s_irpp.lv[2]),
        (unsigned)(s_irpp.exact - s_irpp.lv[3]),
        (unsigned)(s_irpp.alias - s_irpp.lv[4]),
        (unsigned)(s_irpp.held - s_irpp.lv[5]));
}

static void irpp_arm(void)
{
    memset(&s_irpp, 0, sizeof s_irpp);
    isaac_vita_log(
        "KAGE VITA IMAGE PRELOAD PIN: armed mode=%s share=1/%u set_max=%u "
        "lines_max=%u build=%s",
        ISAAC_IRPP_ON ? "on" : "observe", (unsigned)ISAAC_IRPP_SHARE,
        (unsigned)ISAAC_IRPP_SET_MAX, (unsigned)ISAAC_IRPP_LINES_MAX,
        ISAAC_VITA_IMAGE_RETAIN_BUILD_ID);
}

static void irpp_report(const char *tag)
{
    isaac_vita_log(
        "KAGE VITA IMAGE PRELOAD PIN: totals mode=%s pinned=%u refused=%u "
        "used=%u floor=%u yield=%u stale=%u dropped=%u skip=%u cskip=%u "
        "own=%u exact=%u alias=%u held=%u on_free=%u lost=%u twins=%u pins=%u "
        "pinned_bytes=%u why=%s",
        ISAAC_IRPP_ON ? "on" : "observe", (unsigned)s_irpp.pinned,
        (unsigned)s_irpp.refused, (unsigned)s_irpp.unpin[IRPP_USED],
        (unsigned)s_irpp.unpin[IRPP_FLOOR], (unsigned)s_irpp.unpin[IRPP_YIELD],
        (unsigned)s_irpp.unpin[IRPP_STALE], (unsigned)s_irpp.unpin[IRPP_DROPPED],
        (unsigned)s_irpp.skip, (unsigned)s_irpp.cand_skip,
        (unsigned)s_irpp.own, (unsigned)s_irpp.exact,
        (unsigned)s_irpp.alias, (unsigned)s_irpp.held, (unsigned)s_irpp.on_free,
        (unsigned)s_irpp.lost, (unsigned)s_irpp.twins, (unsigned)irpp_pins(),
        (unsigned)s_irpp.pinned_bytes, tag);
}
#endif

/* Every unlocked strong == 1 candidate of the wrapper passes here. */
static void irp_offer(uint32_t image, uint32_t ctrl)
{
    uint32_t i;

#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN)
    irpp_settle();
#endif
    if (s_irp.busy || s_irp.n == 0U || !ir_image_vtable_ok(image))
        return;
    for (i = 0U; i < s_irp.n; ++i) {
        if (!irp_name_is(image, i))
            continue;
        if (s_irp.state[i] != IRP_IDLE && s_irp.image[i] == image &&
                s_irp.ctrl[i] == ctrl) {
            if (s_irp.state[i] == IRP_WAITING) {
                /* Someone took the preloaded sheet (a Load hit) and has just
                 * let go of it again. */
                s_irp.state[i] = IRP_USED;
                ++s_irp.hit;
#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN)
                irpp_unpin(i, IRPP_USED);
#endif
                isaac_vita_log("KAGE VITA IMAGE PRELOAD: hit name=%s image=%08x "
                               "hit=%u miss=%u",
                               s_irp.text[i], (unsigned)image,
                               (unsigned)s_irp.hit, (unsigned)s_irp.miss);
            }
#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN)
            else if (s_irp.state[i] == IRP_ON_FREE) {
                irpp_on_free(i, image, ctrl);
            }
#endif
            return;
        }
        /* A list sheet this module did not provide: the game decoded it. */
        ++s_irp.miss;
#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN)
        irpp_miss(i, image);
#endif
        if (s_irp.miss <= 8U)
            isaac_vita_log("KAGE VITA IMAGE PRELOAD: miss name=%s image=%08x "
                           "state=%u flags=%08x hit=%u miss=%u",
                           s_irp.text[i], (unsigned)image,
                           (unsigned)s_irp.state[i],
                           (unsigned)ld32(image + ISAAC_IR_IMG_FLAGS),
                           (unsigned)s_irp.hit, (unsigned)s_irp.miss);
        return;
    }
}

/* The retention seam is about to Unregister {image, ctrl}. */
static void irp_evicting(uint32_t image, uint32_t ctrl)
{
    uint32_t i;

    for (i = 0U; i < s_irp.n; ++i) {
        if (s_irp.state[i] == IRP_IDLE || s_irp.image[i] != image ||
                s_irp.ctrl[i] != ctrl)
            continue;
        if (s_irp.state[i] == IRP_WAITING) {
            ++s_irp.evicted_unused;
            isaac_vita_log("KAGE VITA IMAGE PRELOAD: evicted-unused name=%s "
                           "image=%08x n=%u",
                           s_irp.text[i], (unsigned)image,
                           (unsigned)s_irp.evicted_unused);
        }
        s_irp.state[i] = IRP_IDLE;
        s_irp.image[i] = 0U;
        s_irp.ctrl[i] = 0U;
    }
}

static void irp_arm(void)
{
    static const char list[] = ISAAC_VITA_IMAGE_PRELOAD_LIST;

    irp_parse();
    memset(s_irp.state, 0, sizeof s_irp.state);
    memset(s_irp.image, 0, sizeof s_irp.image);
    memset(s_irp.ctrl, 0, sizeof s_irp.ctrl);
    s_irp.busy = 0U;
    isaac_vita_log(
        "KAGE VITA IMAGE PRELOAD: armed n=%u truncated=%u max_kb=%u "
        "reserve_mb=%u heap_factor=%u list=%s build=%s",
        (unsigned)s_irp.n, (unsigned)s_irp.truncated,
        (unsigned)ISAAC_VITA_IMAGE_PRELOAD_MAX_KB,
        (unsigned)ISAAC_VITA_IMAGE_PRELOAD_RESERVE_MB,
        (unsigned)ISAAC_IRP_HEAP_FACTOR, list,
        ISAAC_VITA_IMAGE_RETAIN_BUILD_ID);
#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN)
    irpp_arm();
#endif
}

static void irp_report(const char *tag)
{
    isaac_vita_log(
        "KAGE VITA IMAGE PRELOAD: totals levels=%u loaded=%u retained=%u "
        "dropped=%u present=%u fail=%u hit=%u miss=%u evicted_unused=%u "
        "skip(s,l,t,v,h,k)=%u,%u,%u,%u,%u,%u breach=%u bytes=%u us=%u why=%s",
        (unsigned)s_irp.levels, (unsigned)s_irp.loaded,
        (unsigned)s_irp.retained, (unsigned)s_irp.dropped,
        (unsigned)s_irp.present, (unsigned)s_irp.fail, (unsigned)s_irp.hit,
        (unsigned)s_irp.miss, (unsigned)s_irp.evicted_unused,
        (unsigned)s_irp.skip[IRP_SKIP_STATE], (unsigned)s_irp.skip[IRP_SKIP_LOCK],
        (unsigned)s_irp.skip[IRP_SKIP_TERMINAL], (unsigned)s_irp.skip[IRP_SKIP_VMEM],
        (unsigned)s_irp.skip[IRP_SKIP_HEAP], (unsigned)s_irp.skip[IRP_SKIP_STACK],
        (unsigned)s_irp.breach, (unsigned)s_irp.loaded_bytes,
        (unsigned)s_irp.load_us, tag);
#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN)
    irpp_report(tag);
#endif
}

#if defined(ISAAC_VITA_IMAGE_RETAIN_ORACLE)
static void irp_saturate(void)
{
    uint32_t k;

    s_irp.levels = s_irp.loaded = s_irp.loaded_bytes = s_irp.load_us = 0xFFFFFFFFu;
    s_irp.retained = s_irp.dropped = s_irp.present = s_irp.fail = 0xFFFFFFFFu;
    s_irp.hit = s_irp.miss = s_irp.evicted_unused = s_irp.breach = 0xFFFFFFFFu;
    for (k = 0U; k < IRP_SKIP_N; ++k)
        s_irp.skip[k] = 0xFFFFFFFFu;
#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN)
    s_irpp.pinned = s_irpp.refused = s_irpp.skip = s_irpp.own = 0xFFFFFFFFu;
    s_irpp.cand_skip = 0xFFFFFFFFu;
    s_irpp.exact = s_irpp.alias = s_irpp.held = s_irpp.on_free = 0xFFFFFFFFu;
    s_irpp.lost = s_irpp.twins = s_irpp.pinned_bytes = 0xFFFFFFFFu;
    for (k = 0U; k < IRPP_WHY_N; ++k)
        s_irpp.unpin[k] = 0xFFFFFFFFu;
    for (k = 0U; k < ISAAC_IRP_MAX; ++k)
        s_irpp.pin[k] = 1U;              /* pins= counts the parsed entries */
#endif
}
#endif

/* The replay.  Returns 1 with *image_out / *ctrl_out = what Load put in `out`
 * (0 when it produced no Image) and *bytes_out / key[] read while `out` still
 * held its reference; 0 when the replay must stop (fault, stack breach). */
static int irp_replay(CPU *__restrict c, uint32_t i, uint32_t *image_out,
                      uint32_t *ctrl_out, uint32_t *bytes_out,
                      uint32_t dim[3], uint32_t *flags_out,
                      char key[ISAAC_IRP_KEY_MAX + 1U])
{
    const uint32_t saved = c->esp;
    const uint32_t base = (saved - ISAAC_IRP_SCRATCH) & ~3U;
    const uint32_t out = base;             /* SmartPointer<Image> {Image*, ctrl*} */
    const uint32_t name = base + 8U;
    uint32_t image, ctrl, k, hook;

    *image_out = 0U;
    *ctrl_out = 0U;
    *bytes_out = 0U;
    *flags_out = 0U;
    dim[0] = dim[1] = dim[2] = 0U;
    key[0] = '\0';
    st32(out, 0U);
    st32(out + 4U, 0U);
    for (k = 0U; k < s_irp.len[i]; ++k)
        st8(name + k, (uint8_t)s_irp.text[i][k]);
    st8(name + k, 0U);
    c->esp = base;

    /* 0x565fad push 0 / push 0xbbf948 / push name / push &out / call Load. */
    gpush(c, 0U);
    gpush(c, IR_VA(ISAAC_IR_LOAD_VERTEX_DESC_RVA));
    gpush(c, name);
    gpush(c, out);
    gpush(c, ISAAC_IR_LOAD_SITE_RVA);
    guest_call(c, IR_VA(ISAAC_IR_LOAD_RVA));
    if (c->fault)
        return 0;
    if (c->esp != base) {                  /* Load is `ret 0x10` */
        c->esp = saved;
        ++s_irp.breach;
        return 0;
    }
    image = ld32(out);
    ctrl = ld32(out + 4U);
    if (image != 0U && (image & 3U) == 0U && ir_image_vtable_ok(image)) {
        uint32_t name_va = ld32(image + ISAAC_IR_IMG_NAME);
        uint32_t len = 0U, start;
        uint64_t bytes;

        dim[0] = ld16(image + ISAAC_IR_IMG_PW);
        dim[1] = ld16(image + ISAAC_IR_IMG_PH);
        dim[2] = ld32(image + ISAAC_IR_IMG_BPP);
        bytes = (uint64_t)dim[0] * (uint64_t)dim[1] * (uint64_t)dim[2];
        *bytes_out = bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)bytes;
        *flags_out = ld32(image + ISAAC_IR_IMG_FLAGS);
        if (name_va != 0U) {
            while (len < ISAAC_IRP_NAME_SCAN && ld8(name_va + len) != 0U)
                ++len;
        }
        start = len > ISAAC_IRP_KEY_MAX ? len - ISAAC_IRP_KEY_MAX : 0U;
        for (k = 0U; start + k < len; ++k) {
            uint8_t ch = (uint8_t)ld8(name_va + start + k);

            key[k] = (ch >= 0x21U && ch <= 0x7eU) ? (char)ch : '?';
        }
        key[k] = '\0';
    }
    *image_out = image;
    *ctrl_out = ctrl;

    /* 0x565fee: the SmartPointer<Image> destructor of `out`. */
    if (ctrl != 0U) {
        gpush(c, ISAAC_IR_LOAD_RELEASE_SITE_RVA);
        c->ecx = ctrl;
        guest_call(c, ld32(ld32(ctrl) + ISAAC_IR_CTRL_VT_RELEASE));
        if (c->fault)
            return 0;
        if (c->esp != base) {              /* Release is a plain `ret` */
            c->esp = saved;
            ++s_irp.breach;
            return 0;
        }
        hook = ld32(s_ir.hook_global_va);
        if ((c->eax & 0xFFU) != 0U && hook != 0U) {
            gpush(c, out);
            gpush(c, ISAAC_IR_LOAD_OBSERVER_SITE_RVA);
            guest_call(c, hook);
            if (c->fault)
                return 0;
            if (c->esp != base - 4U) {     /* cdecl: the caller drops &out */
                c->esp = saved;
                ++s_irp.breach;
                return 0;
            }
        }
    }
    c->esp = saved;
    return 1;
}

/* One list entry.  Returns 0 when the walk must stop. */
static int irp_one(CPU *__restrict c, uint32_t i)
{
    const uint32_t est = (uint32_t)ISAAC_VITA_IMAGE_PRELOAD_MAX_KB * 1024U;
    const uint32_t reserve = (uint32_t)ISAAC_VITA_IMAGE_PRELOAD_RESERVE_MB << 20;
    uint32_t image = 0U, ctrl = 0U, bytes = 0U, flags = 0U, dim[3], hr0, hr1, us;
    char key[ISAAC_IRP_KEY_MAX + 1U];
    const char *result;
    uint64_t t0;
    uint16_t idx;

    if (!s_ir.armed || s_ir.budget_bytes == 0U || s_ir.in_evict ||
            (s_ir.cpu != NULL && s_ir.cpu != c)) {
        ++s_irp.skip[IRP_SKIP_STATE];
        return 1;
    }
    if (ir_manager_lock_held()) {
        ++s_irp.skip[IRP_SKIP_LOCK];
        return 1;
    }
    if (isaac_vita_guest_heap_terminal()) {
        ++s_irp.skip[IRP_SKIP_TERMINAL];
        return 1;
    }
#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN)
    s_irpp.set_n = 0U;
    if (irp_find_in_manager(i, &image, &ctrl) != 0) {
        s_irp.present += 1U;
        if (irpp_present(i))
            return 1;
    }
#else
    if (irp_find_in_manager(i, &image, &ctrl)) {
        ++s_irp.present;
        return 1;
    }
#endif
    if (est > s_ir.budget_bytes ||
            (s_ir.headroom_bytes != 0U &&
             ir_free_bytes() < (uint64_t)s_ir.headroom_bytes + (uint64_t)est)) {
        ++s_irp.skip[IRP_SKIP_VMEM];
        return 1;
    }
    hr0 = isaac_vita_image_preload_heap_headroom();
    if (hr0 < reserve || (hr0 - reserve) / ISAAC_IRP_HEAP_FACTOR < est) {
        ++s_irp.skip[IRP_SKIP_HEAP];
        return 1;
    }
    if (!guest_stack_contains(c, c->esp - ISAAC_IRP_SCRATCH - 64U,
                              ISAAC_IRP_SCRATCH + 64U)) {
        ++s_irp.skip[IRP_SKIP_STACK];
        return 1;
    }

    t0 = irp_now_us();
    s_irp.busy = 1U;
    if (!irp_replay(c, i, &image, &ctrl, &bytes, dim, &flags, key)) {
        s_irp.busy = 0U;
        return 0;
    }
    s_irp.busy = 0U;
    us = (uint32_t)(irp_now_us() - t0);
    s_irp.load_us += us;
    hr1 = isaac_vita_image_preload_heap_headroom();
    if (image == 0U || ctrl == 0U) {
        ++s_irp.fail;
        result = "fail";
    } else {
        ++s_irp.loaded;
        s_irp.loaded_bytes += bytes;
        idx = ir_hash_find(image);
        if (idx != ISAAC_IR_NIL && s_ir.rec[idx].ctrl == ctrl &&
                ir_present_in_manager(image, ctrl)) {
            ++s_irp.retained;
            s_irp.state[i] = IRP_WAITING;
            s_irp.image[i] = image;
            s_irp.ctrl[i] = ctrl;
            result = "retained";
        } else {
            ++s_irp.dropped;
            result = "dropped";
        }
    }
#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN)
    result = irpp_classify(i, image, ctrl, result);
#endif
    isaac_vita_log(
        "KAGE VITA IMAGE PRELOAD: image name=%s result=%s image=%08x bytes=%u "
        "dim=%ux%ux%u flags=%08x us=%u hr_kb=%u..%u live=%u/%u key=%s",
        s_irp.text[i], result, (unsigned)image, (unsigned)bytes,
        (unsigned)dim[0], (unsigned)dim[1], (unsigned)dim[2], (unsigned)flags,
        (unsigned)us, (unsigned)(hr0 >> 10), (unsigned)(hr1 >> 10),
        (unsigned)s_ir.count, (unsigned)s_ir.retained_bytes, key);
    return 1;
}

/* Level::Init has returned on CPU `c` (ESP as its caller sees it).  The
 * caller saves and restores the guest registers around this call. */
void isaac_vita_image_preload_level_init(CPU *__restrict c)
{
    const uint32_t loaded = s_irp.loaded, retained = s_irp.retained;
    const uint32_t dropped = s_irp.dropped, present = s_irp.present;
    const uint32_t fail = s_irp.fail, bytes = s_irp.loaded_bytes;
    uint32_t skipped = 0U, i, k, hr0;
    uint64_t t0;

    irp_parse();
    ++s_irp.levels;
#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN)
    irpp_level_begin();
#endif
    for (k = 0U; k < IRP_SKIP_N; ++k)
        skipped -= s_irp.skip[k];
    hr0 = isaac_vita_image_preload_heap_headroom();
    t0 = irp_now_us();
    for (i = 0U; i < s_irp.n && !c->fault; ++i) {
        if (!irp_one(c, i))
            break;
    }
    for (k = 0U; k < IRP_SKIP_N; ++k)
        skipped += s_irp.skip[k];
#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN)
    irpp_level_end();                      /* before the level line, the last one */
#endif
    isaac_vita_log(
        "KAGE VITA IMAGE PRELOAD: level=%u n=%u loaded=%u retained=%u "
        "dropped=%u present=%u fail=%u skipped=%u bytes=%u us=%u "
        "hr_kb=%u..%u need_kb=%u fault=%u",
        (unsigned)s_irp.levels, (unsigned)s_irp.n,
        (unsigned)(s_irp.loaded - loaded), (unsigned)(s_irp.retained - retained),
        (unsigned)(s_irp.dropped - dropped), (unsigned)(s_irp.present - present),
        (unsigned)(s_irp.fail - fail), (unsigned)skipped,
        (unsigned)(s_irp.loaded_bytes - bytes),
        (unsigned)(irp_now_us() - t0), (unsigned)(hr0 >> 10),
        (unsigned)(isaac_vita_image_preload_heap_headroom() >> 10),
        /* the `h` gate of irp_one in KiB: hr_kb's first value against this */
        (unsigned)(((uint32_t)ISAAC_VITA_IMAGE_PRELOAD_RESERVE_MB << 10) +
                   ISAAC_IRP_HEAP_FACTOR *
                   (uint32_t)ISAAC_VITA_IMAGE_PRELOAD_MAX_KB),
        (unsigned)(c->fault != NULL));
}
#endif

/* ------------------------------------------------------------- eviction --- */

/* Unregister one image on the game CPU (the deferred original behaviour).
 * Returns 1 when the guest Unregister ran, 0 when the record was stale
 * (gone from the manager, replaced, foreign vtable, or re-acquired). */
static int ir_evict_one(CPU *__restrict c, uint32_t image, uint32_t ctrl,
                        uint32_t bytes, enum ir_reason reason)
{
    if (!ir_present_in_manager(image, ctrl) || !ir_image_vtable_ok(image) ||
            ld32(ctrl) != s_ir.vt_ctrl ||
            ld16(ctrl + ISAAC_IR_CTRL_STRONG) != 1U) {
        /* Gone behind our back, or someone re-acquired it (the observer
         * will re-offer it when that holder dies). */
        ++s_ir.st.dropped_stale;
        IRB_NOTE_RECORD(image, bytes, "stale");
        return 0;
    }
    IRB_NOTE(image, reason == IR_REASON_BUDGET ? "evict-budget" :
                    reason == IR_REASON_HEADROOM ? "evict-headroom" :
                    "evict-table");
#if defined(ISAAC_VITA_IMAGE_PRELOAD)
    irp_evicting(image, ctrl);
#endif
    s_ir.in_evict = 1U;
    if (!ir_guest_unregister(c, image)) {
        s_ir.in_evict = 0U;
        ++s_ir.st.fault;
        guest_fault(c, image, "image retain: Unregister stack contract");
        return 0;
    }
    s_ir.in_evict = 0U;
#if defined(ISAAC_VITA_IMAGE_RETAIN_HEADROOM_PHYCONT)
    s_ir_free.valid = 0U;                  /* the sheet's texture may be free now */
#endif
    ++s_ir.st.evicted;
    s_ir.st.evicted_bytes += bytes;
    if (reason == IR_REASON_BUDGET)
        ++s_ir.st.budget_evict;
    else if (reason == IR_REASON_HEADROOM)
        ++s_ir.st.headroom_evict;
    else
        ++s_ir.st.table_full;
    return 1;
}

/* Evict the LRU tail.  Returns 1 if a record slot was reclaimed (whether the
 * image was Unregistered or dropped as stale), 0 when the table is empty. */
#if defined(ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED) && ISAAC_IRT_ON
/* Rule E.  A record is pinned while its Image is live, still the manager's
 * alone (the tests ir_evict_one applies before it Unregisters) and on a
 * batch-flush list.  A stale record is never pinned: ir_evict_one drops it
 * without touching the guest. */
static int irt_record_pinned(uint16_t idx)
{
    uint32_t image = s_ir.rec[idx].image;
    uint32_t ctrl = s_ir.rec[idx].ctrl;

    if (!ir_present_in_manager(image, ctrl) || !ir_image_vtable_ok(image) ||
            ld32(ctrl) != s_ir.vt_ctrl ||
            ld16(ctrl + ISAAC_IR_CTRL_STRONG) != 1U)
        return 0;
    return (ld32(image + ISAAC_IR_IMG_FLAGS) & ISAAC_IRT_FLUSH_MASK) != 0U;
}

#endif
#if defined(ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED) && ISAAC_IRT_ON && \
    !(defined(ISAAC_VITA_IMAGE_PRELOAD_PIN) && ISAAC_IRPP_ON)
/* The least-recently-used record an eviction may release, or NIL. */
static uint16_t irt_evictable_from_tail(void)
{
    uint16_t idx = s_ir.tail;
    uint32_t skipped = 0U;

    while (idx != ISAAC_IR_NIL) {
        if (!irt_record_pinned(idx))
            return idx;
        ++s_irt.pinned_skip;
        if (++skipped >= ISAAC_IRT_PIN_SKIP_MAX)
            break;
        idx = s_ir.rec[idx].prev;
    }
    ++s_irt.evict_blocked;
    return ISAAC_IR_NIL;
}
#endif

#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN) && ISAAC_IRPP_ON
/* The list entry whose pin holds record `idx`, or -1.  A pin whose sheet left
 * the manager behind our back (or no longer is the manager's control block)
 * is released here as `stale`; ir_evict_one then drops the record without
 * touching the guest. */
static int irpp_pin_of(uint16_t idx)
{
    uint32_t image = s_ir.rec[idx].image;
    uint32_t ctrl = s_ir.rec[idx].ctrl;
    uint32_t i;

    for (i = 0U; i < s_irp.n; ++i) {
        if (!s_irpp.pin[i] || s_irp.image[i] != image || s_irp.ctrl[i] != ctrl)
            continue;
        if (!ir_present_in_manager(image, ctrl) || !ir_image_vtable_ok(image) ||
                ld32(ctrl) != s_ir.vt_ctrl) {
            irpp_unpin(i, IRPP_STALE);
            return -1;
        }
        return (int)i;
    }
    return -1;
}

/* The least-recently-used record an eviction may release, or NIL.  Pinned
 * records are walked past; when nothing else is reachable the oldest pin
 * yields, so a pin never blocks an eviction (rule E's records still can).
 *
 * The candidate's own record is a barrier.  ir_wrap moves it to the head
 * before a headroom release and records it at the head before a budget
 * eviction, so OFF's plain tail walk reaches it only once everything older
 * is gone.  A walker that skips records reaches it sooner:
 *  - headroom (`reason` HEADROOM, before the lock replay): taking it would
 *    Unregister the Image the observer's caller still holds and leave
 *    ir_wrap a stale index to drop (review F-BA item 1), so it is never
 *    returned -- the oldest other pin yields, else NIL;
 *  - budget (after the replay, the record just made): taking it is the
 *    observer's own Unregister, which the plain walk and rule E's walker
 *    also issue, but only as the last resort -- the oldest other pin
 *    yields first (item 2), so ON keeps OFF's choice of what stays.
 * It is tested after both pin tests, so a rule-E-pinned candidate counts as
 * rule E's (pinned_skip, then budget_refuse) exactly as without this knob.
 * cand_skip counts walks that passed it and did not return it. */
static uint16_t irpp_evictable_from_tail(enum ir_reason reason)
{
    uint16_t idx = s_ir.tail, oldest = ISAAC_IR_NIL, cand = ISAAC_IR_NIL;
    int k, oldest_k = -1, cand_k = -1;
#if defined(ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED) && ISAAC_IRT_ON
    uint32_t skipped = 0U;
#endif

    while (idx != ISAAC_IR_NIL) {
        int is_cand = s_ir.candidate != 0U &&
                      s_ir.rec[idx].image == s_ir.candidate;

        k = irpp_pin_of(idx);
        if (k >= 0) {
            ++s_irpp.skip;
            if (is_cand) {
                cand = idx;
                cand_k = k;
            } else if (oldest == ISAAC_IR_NIL) {
                oldest = idx;
                oldest_k = k;
            }
            idx = s_ir.rec[idx].prev;
            continue;
        }
#if defined(ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED) && ISAAC_IRT_ON
        if (irt_record_pinned(idx)) {
            ++s_irt.pinned_skip;
            if (++skipped >= ISAAC_IRT_PIN_SKIP_MAX)
                break;
            idx = s_ir.rec[idx].prev;
            continue;
        }
#endif
        if (is_cand) {
            cand = idx;
            cand_k = -1;
            idx = s_ir.rec[idx].prev;
            continue;
        }
        return idx;
    }
    if (oldest != ISAAC_IR_NIL) {
        irpp_unpin((uint32_t)oldest_k, IRPP_YIELD);
        if (cand != ISAAC_IR_NIL)
            ++s_irpp.cand_skip;
        return oldest;
    }
    if (cand != ISAAC_IR_NIL) {
        if (reason == IR_REASON_BUDGET) {
            if (cand_k >= 0)
                irpp_unpin((uint32_t)cand_k, IRPP_YIELD);
            return cand;
        }
        ++s_irpp.cand_skip;
    }
#if defined(ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED) && ISAAC_IRT_ON
    ++s_irt.evict_blocked;
#endif
    return ISAAC_IR_NIL;
}
#endif

static int ir_evict_tail(CPU *__restrict c, enum ir_reason reason)
{
    uint16_t idx = s_ir.tail;
    uint32_t image, ctrl, bytes;

    if (idx == ISAAC_IR_NIL)
        return 0;
#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN) && ISAAC_IRPP_ON
    idx = irpp_evictable_from_tail(reason);
    if (idx == ISAAC_IR_NIL)
        return 0;                          /* every reachable record is rule E's */
#elif defined(ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED) && ISAAC_IRT_ON
    idx = irt_evictable_from_tail();
    if (idx == ISAAC_IR_NIL)
        return 0;                          /* every reachable record is pinned */
#endif
    ir_drop_record(idx, &image, &ctrl, &bytes);
    (void)ir_evict_one(c, image, ctrl, bytes, reason);
    return 1;
}

/* Byte budget: evict LRU tails until the retained set fits again. */
static void ir_evict_as_needed(CPU *__restrict c)
{
    if (s_ir.in_evict || ir_manager_lock_held()) {
        /* Unregister would re-enter the manager Mutex on this thread: keep
         * everything for now, the next unlocked candidate evicts. */
        if (s_ir.retained_bytes > s_ir.budget_bytes ||
                (s_ir.headroom_bytes != 0U &&
                 ir_free_bytes() < (uint64_t)s_ir.headroom_bytes))
            ++s_ir.st.deferred;
        return;
    }
    while (s_ir.count > 0U && s_ir.retained_bytes > s_ir.budget_bytes) {
        if (!ir_evict_tail(c, IR_REASON_BUDGET))
            break;
    }
}

/* Headroom pressure: release LRU tails (never the candidate's own record,
 * which the caller has moved to the head: with the plain tail walk, `keep`
 * records stay; a walker that skips records -- IMAGE_PRELOAD_PIN ON -- could
 * reach the head, and its own candidate barrier is what keeps the record)
 * until free memory is back above the headroom or ISAAC_IR_HEADROOM_EVICT_MAX
 * tails went.  Bounded because vitaGL defers the free of a recently drawn texture
 * to its purge list (gpu_free_texture_data, FRAME_PURGE_FREQ frames), so
 * vglMemFree cannot be expected to rise inside this call.  Returns the last
 * free-memory reading; the caller retains normally when it recovered and
 * otherwise hands the candidate to the real observer, so the resident set
 * never grows while free memory is below the headroom. */
static uint64_t ir_headroom_release(CPU *__restrict c, uint64_t free_bytes,
                                    uint32_t keep)
{
    uint32_t n = 0U;

    while (n < ISAAC_IR_HEADROOM_EVICT_MAX && s_ir.count > keep &&
           free_bytes < (uint64_t)s_ir.headroom_bytes) {
        if (!ir_evict_tail(c, IR_REASON_HEADROOM))
            break;
        ++n;
        free_bytes = ir_free_bytes();
    }
    return free_bytes;
}

/* Insert-at-head (new) or move-to-head (reuse); `at_tail` inserts a new
 * record at the LRU end instead (a kept, never-seen-used entry).  A record
 * with the same Image address but a different control block is a dead image
 * whose address was reused: drop it as stale first. */
static void ir_touch(uint32_t image, uint32_t ctrl, uint32_t bytes, int at_tail)
{
    uint16_t idx = ir_hash_find(image);

    if (idx != ISAAC_IR_NIL && s_ir.rec[idx].ctrl != ctrl) {
        uint32_t di, dc, db;
        ir_drop_record(idx, &di, &dc, &db);
        ++s_ir.st.dropped_stale;
        IRB_NOTE_RECORD(di, db, "stale-reused");
        idx = ISAAC_IR_NIL;
    }
    if (idx != ISAAC_IR_NIL) {
        ir_lru_unlink(idx);
        ir_account_sub(s_ir.rec[idx].bytes);
        s_ir.rec[idx].bytes = bytes;
        ir_account_add(bytes);
        ir_lru_push_head(idx);
        ++s_ir.st.reuse;
        return;
    }
    idx = ir_alloc_record();               /* the caller made room */
    if (idx == ISAAC_IR_NIL)
        return;
    s_ir.rec[idx].image = image;
    s_ir.rec[idx].ctrl = ctrl;
    s_ir.rec[idx].bytes = bytes;
    ir_hash_insert(idx);
    if (at_tail)
        ir_lru_push_tail(idx);
    else
        ir_lru_push_head(idx);
    ir_account_add(bytes);
    ++s_ir.st.retained_new;
}

/* --------------------------------------------------------------- public --- */

static void ir_default_env(void)
{
#if !defined(ISAAC_VITA_IMAGE_RETAIN_ORACLE)
    s_ir.image_base = (uint32_t)GUEST_IMAGE_BASE;
    s_ir.buckets_ptr_va = IR_VA(ISAAC_IR_BUCKETS_PTR_RVA);
    s_ir.hook_global_va = IR_VA(ISAAC_IR_HOOK_GLOBAL_RVA);
    s_ir.hook_global2_va = IR_VA(ISAAC_IR_HOOK_GLOBAL2_RVA);
    s_ir.vt_png = IR_VA(ISAAC_IR_VT_PNG_RVA);
    s_ir.vt_pcx = IR_VA(ISAAC_IR_VT_PCX_RVA);
    s_ir.vt_pic = IR_VA(ISAAC_IR_VT_PIC_RVA);
    s_ir.vt_ctrl = IR_VA(ISAAC_IR_CTRL_VT_RVA);
    s_ir.vt_mutex = IR_VA(ISAAC_IR_MUTEX_VT_RVA);
    s_ir.manager_lock_va = IR_VA(ISAAC_IR_MANAGER_LOCK_RVA);
    s_ir.budget_bytes = (uint32_t)ISAAC_VITA_IMAGE_RETAIN_BUDGET_MB << 20;
    s_ir.headroom_bytes = (uint32_t)ISAAC_VITA_IMAGE_RETAIN_HEADROOM_MB << 20;
    s_ir.env_ready = 1U;
#endif
}

void isaac_vita_image_retain_arm(void)
{
    uint32_t global, global2;
    int global_ok;

    if (s_ir.armed)
        return;
    if (!s_ir.env_ready)
        ir_default_env();
    ir_reset_records();
    s_ir.cpu = NULL;
    s_ir.in_evict = 0U;
    memset(&s_ir.st, 0, sizeof s_ir.st);

    global = ld32(s_ir.hook_global_va);
    global2 = ld32(s_ir.hook_global2_va);
    global_ok = (global == IR_VA(ISAAC_IR_HOOK_RVA) &&
                 global2 == IR_VA(ISAAC_IR_HOOK_RVA));
    if (!global_ok) {
        /* The observer is not installed the way the frozen PE installs it:
         * never arm, so every hook call stays a plain passthrough. */
        isaac_vita_log(
            "KAGE VITA IMAGE RETAIN ARM: armed=0 reason=hook-global "
            "global=%08x global2=%08x expected=%08x build=%s",
            (unsigned)global, (unsigned)global2,
            (unsigned)IR_VA(ISAAC_IR_HOOK_RVA),
            ISAAC_VITA_IMAGE_RETAIN_BUILD_ID);
        return;
    }
    if (!ir_manager_lock_shape_ok() ||
            ld32(s_ir.vt_mutex + ISAAC_IR_LOCK_VT_ENTER) != IR_VA(ISAAC_IR_MUTEX_ENTER_RVA) ||
            ld32(s_ir.vt_mutex + ISAAC_IR_LOCK_VT_LEAVE) != IR_VA(ISAAC_IR_MUTEX_LEAVE_RVA)) {
        /* The manager lock is not the frozen Mutex (or not constructed): the
         * held-byte probe that keeps eviction off the game's own Unregister
         * would be meaningless -> never arm. */
        isaac_vita_log(
            "KAGE VITA IMAGE RETAIN ARM: armed=0 reason=manager-lock "
            "lock_vt=%08x init=%u cs=%08x expected_vt=%08x build=%s",
            (unsigned)ld32(s_ir.manager_lock_va),
            (unsigned)ld8(s_ir.manager_lock_va + ISAAC_IR_MUTEX_INIT),
            (unsigned)ld32(s_ir.manager_lock_va + ISAAC_IR_MUTEX_CS),
            (unsigned)s_ir.vt_mutex, ISAAC_VITA_IMAGE_RETAIN_BUILD_ID);
        return;
    }
#if defined(ISAAC_VITA_IMAGE_RETAIN_HEADROOM_PHYCONT)
    memset(&s_ir_free, 0, sizeof s_ir_free);
    isaac_vita_log(
        "KAGE VITA IMAGE RETAIN HEADROOM: phycont=1 ram=%u vram=%u phycont_free=%u "
        "headroom=%u",
        (unsigned)ir_free_ram(), (unsigned)ir_free_vram(),
        (unsigned)ir_free_phycont(), (unsigned)s_ir.headroom_bytes);
#endif
#if defined(ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED)
    memset(&s_irt, 0, sizeof s_irt);
    isaac_vita_log(
        "KAGE VITA IMAGE RETAIN TOUCHED: armed mode=%s idle_ms=%u "
        "pin_skip_max=%u build=%s",
        ISAAC_IRT_ON ? "on" : "observe", (unsigned)ISAAC_IR_TOUCHED_IDLE_MS,
        (unsigned)ISAAC_IRT_PIN_SKIP_MAX, ISAAC_VITA_IMAGE_RETAIN_BUILD_ID);
#endif
#if defined(ISAAC_VITA_IMAGE_PRELOAD)
    irp_arm();
#endif
#if defined(ISAAC_VITA_IMAGE_RETAIN_BIG_TRACE)
    /* Before the ARM line, which stays the last line of a successful arm. */
    memset(&s_irb, 0, sizeof s_irb);
    isaac_vita_log(
        "KAGE VITA IMAGE RETAIN BIG TRACE: on min_bytes=%u max_lines=%u build=%s",
        (unsigned)ISAAC_IRB_MIN_BYTES, (unsigned)ISAAC_IRB_MAX_LINES,
        ISAAC_VITA_IMAGE_RETAIN_BUILD_ID);
#endif
    s_ir.armed = 1U;
    isaac_vita_log(
        "KAGE VITA IMAGE RETAIN ARM: armed=1 budget=%u headroom=%u records=%u "
        "hook=%08x unregister=%08x table=%08x mutex_vt=%08x lock_held=%u "
        "global_ok=1 build=%s",
        (unsigned)s_ir.budget_bytes, (unsigned)s_ir.headroom_bytes,
        (unsigned)ISAAC_IR_RECORDS,
        (unsigned)IR_VA(ISAAC_IR_HOOK_RVA),
        (unsigned)IR_VA(ISAAC_IR_UNREGISTER_RVA),
        (unsigned)s_ir.buckets_ptr_va, (unsigned)s_ir.vt_mutex,
        (unsigned)ir_manager_lock_held(),
        ISAAC_VITA_IMAGE_RETAIN_BUILD_ID);
}

void isaac_vita_image_retain_disarm(void)
{
    /* Retained entries (strong == 1) stay in the manager after the disarm;
     * a shift/growth re-offer under the held manager Mutex must still never
     * reach __real (Unregister would re-enter the Mutex), so the wrapper
     * keeps the held-lock guard armed for as long as the lock is known. */
    s_ir.guard = (s_ir.manager_lock_va != 0U) ? 1U : 0U;
    s_ir.armed = 0U;
    ir_reset_records();
    s_ir.cpu = NULL;
    s_ir.in_evict = 0U;
    s_ir.candidate = 0U;
}

/* Two lines: isaac_vita_log formats into a 384-byte body (vsnprintf, silent
 * truncation).  The gate fields come first on the first line; the second
 * line carries the memory model and the fail-closed diagnostics.  The oracle
 * pins both lines below 384 bytes with every %u at u32 max. */
void isaac_vita_image_retain_report(const char *why)
{
    const char *tag = why ? why : "?";
    uint64_t free_bytes = s_ir.armed ? ir_free_bytes() : 0U;

    isaac_vita_log(
        "KAGE VITA IMAGE RETAIN: offered=%u retained=%u reuse=%u evicted=%u "
        "evicted_bytes=%u retained_bytes=%u budget=%u passthrough=%u "
        "ineligible=%u stale=%u table_full=%u headroom_evict=%u "
        "budget_evict=%u headroom_pass=%u live=%u armed=%u why=%s",
        (unsigned)s_ir.st.offered, (unsigned)s_ir.st.retained_new,
        (unsigned)s_ir.st.reuse, (unsigned)s_ir.st.evicted,
        (unsigned)s_ir.st.evicted_bytes, (unsigned)s_ir.retained_bytes,
        (unsigned)s_ir.budget_bytes, (unsigned)s_ir.st.passthrough,
        (unsigned)s_ir.st.ineligible, (unsigned)s_ir.st.dropped_stale,
        (unsigned)s_ir.st.table_full, (unsigned)s_ir.st.headroom_evict,
        (unsigned)s_ir.st.budget_evict, (unsigned)s_ir.st.headroom_pass,
        (unsigned)s_ir.count, (unsigned)s_ir.armed, tag);
    isaac_vita_log(
        "KAGE VITA IMAGE RETAIN MEM: free=%u vf_ram=%u vf_vram=%u headroom=%u "
        "peak_bytes=%u oversize=%u absent=%u nested=%u foreign_cpu=%u fault=%u "
        "held_keep=%u orphan=%u deferred=%u records=%u why=%s",
        (unsigned)(free_bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : free_bytes),
        (unsigned)(s_ir.armed ? ir_free_ram() : 0U),
        (unsigned)(s_ir.armed ? ir_free_vram() : 0U),
        (unsigned)s_ir.headroom_bytes, (unsigned)s_ir.st.peak_bytes,
        (unsigned)s_ir.st.oversize, (unsigned)s_ir.st.absent,
        (unsigned)s_ir.st.nested, (unsigned)s_ir.st.foreign_cpu,
        (unsigned)s_ir.st.fault, (unsigned)s_ir.st.held_keep,
        (unsigned)s_ir.st.orphan, (unsigned)s_ir.st.deferred,
        (unsigned)ISAAC_IR_RECORDS, tag);
#if defined(ISAAC_VITA_IMAGE_RETAIN_HEADROOM_PHYCONT)
    isaac_vita_log(
        "KAGE VITA IMAGE RETAIN HEADROOM: samples=%u cached=%u why=%s",
        (unsigned)s_ir_free.samples, (unsigned)s_ir_free.hits, tag);
#endif
#if defined(ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED)
    isaac_vita_log(
        "KAGE VITA IMAGE RETAIN TOUCHED: mode=%s offers=%u admit=%u "
        "admit_bytes=%u dirty=%u vec=%u pair=%u pinned_skip=%u "
        "evict_blocked=%u budget_refuse=%u pair_untouched=%u why=%s",
        ISAAC_IRT_ON ? "on" : "observe", (unsigned)s_irt.offers,
        (unsigned)s_irt.admit, (unsigned)s_irt.admit_bytes,
        (unsigned)s_irt.dirty, (unsigned)s_irt.vec, (unsigned)s_irt.pair,
        (unsigned)s_irt.pinned_skip, (unsigned)s_irt.evict_blocked,
        (unsigned)s_irt.budget_refuse, (unsigned)s_irt.pair_untouched, tag);
#endif
#if defined(ISAAC_VITA_IMAGE_PRELOAD)
    irp_report(tag);
#endif
#if defined(ISAAC_VITA_IMAGE_RETAIN_BIG_TRACE)
    isaac_vita_log(
        "KAGE VITA IMAGE RETAIN BIG TOTALS: inel(vt/rt/tex/name/bytes)="
        "%u/%u/%u/%u/%u last_foreign_vt=%08x lines=%u suppressed=%u "
        "min_bytes=%u why=%s",
        (unsigned)s_irb.inel_vt, (unsigned)s_irb.inel_rt,
        (unsigned)s_irb.inel_tex, (unsigned)s_irb.inel_name,
        (unsigned)s_irb.inel_bytes, (unsigned)s_irb.last_foreign_vt,
        (unsigned)s_irb.lines, (unsigned)s_irb.suppressed,
        (unsigned)ISAAC_IRB_MIN_BYTES, tag);
#endif
}

void isaac_vita_image_retain_window(isaac_ir_window *out)
{
    if (!out)
        return;
    out->totals = s_ir.st;
    out->totals.retained_bytes = s_ir.retained_bytes;
    out->live_records = s_ir.count;
    out->budget_bytes = s_ir.budget_bytes;
    out->headroom_bytes = s_ir.headroom_bytes;
    out->armed = s_ir.armed;
    out->vf_ram = s_ir.armed ? ir_free_ram() : 0U;
    out->vf_vram = s_ir.armed ? ir_free_vram() : 0U;
}

#if defined(ISAAC_VITA_IMAGE_RETAIN_GHOST)
/* Load's strcmp key, hashed; bounded like any guest string read here. */
static uint32_t ir_name_hash(uint32_t image)
{
    uint32_t name = ld32(image + ISAAC_IR_IMG_NAME);
    uint32_t h = IRG_FNV1A_INIT;
    uint32_t i;

    for (i = 0U; i < 260U; ++i) {
        uint8_t ch = (uint8_t)ld8(name + i);

        if (ch == 0U)
            break;
        h = irg_fnv1a_step(h, ch);
    }
    return h;
}

void isaac_vita_image_retain_ghost_sample(void)
{
# if !defined(ISAAC_VITA_IMAGE_RETAIN_ORACLE)
    SceKernelFreeMemorySizeInfo info;
    uint64_t t0 = sceKernelGetProcessTimeWide();
#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
    /* The render thread's copy, refreshed after every swap. */
    uint32_t ram = ir_free_ram();
    uint32_t vram = ir_free_vram();
    uint32_t phy = kage_vita_backend_vgl_mem(KAGE_VITA_VGL_SNAPSHOT_PHYCONT_FREE);
#else
    uint32_t ram = (uint32_t)vglMemFree(VGL_MEM_RAM);
    uint32_t vram = (uint32_t)vglMemFree(VGL_MEM_VRAM);
    /* vitaGL's ordinal after RAM is PHYCONT in the pinned header (see
     * host_vita_debug_overlay.c). */
    uint32_t phy = (uint32_t)vglMemFree((vglMemType)(VGL_MEM_RAM + 1));
#endif
    uint64_t t1;

    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    if (sceKernelGetFreeMemorySize(&info) < 0)
        info.size_user = 0;
    t1 = sceKernelGetProcessTimeWide();
    irg_mem_note(&s_irg.mem, ram, vram, phy, (uint32_t)info.size_user,
                 t1 > t0 ? (uint32_t)(t1 - t0) : 0U);
# endif
}

void isaac_vita_image_retain_ghost_take(isaac_irg_stats *st, isaac_irg_mem *mem)
{
    if (st)
        *st = s_irg.st;
    if (mem)
        *mem = s_irg.mem;
    memset(&s_irg.mem, 0, sizeof s_irg.mem);
}
#endif

/* --------------------------------------------------------------- wrapper --- */

#if defined(ISAAC_VITA_IMAGE_RETAIN_ORACLE) || defined(ISAAC_VITA_IMAGE_RETAIN_WRAP)
/* cdecl entry: [esp] = return word (RVA), [esp+4] = pair* ({Image*, ctrl*}).
 * May clobber eax/ecx/edx like the translated body; pops exactly the return
 * word on the retain path (the observer's `ret`; the caller does add esp,4). */
/* The observer would Unregister `image` now, but this thread holds the manager
 * Mutex: the game's own Unregister erase loop / bucket growth is re-offering a
 * retained entry (held byte set), or the offer comes from inside our own
 * eviction (in_evict).  Unregister here -- by __real or by us -- would re-enter
 * the Mutex and spin forever, so keep the manager's reference instead: replay
 * the observer's Enter(-1)/Leave on the control block, leave any existing
 * record where it is (this is not a Load hit), give an untracked strong == 1
 * entry a record when one is free, and emulate the observer's ret.  Eviction
 * pressure is re-evaluated at the next unlocked candidate. */
static void ir_keep(CPU *__restrict c, uint32_t image, uint32_t ctrl,
                    uint32_t vtable, int nested)
{
    uint32_t strong = ld16(ctrl + ISAAC_IR_CTRL_STRONG);
    uint32_t bytes = 0U;
    uint16_t idx;

    if (nested)
        ++s_ir.st.nested;
    else
        ++s_ir.st.held_keep;
    if (vtable != 0U) {
        if (!ir_guest_lock(c, ctrl, vtable)) {
            ++s_ir.st.fault;
            guest_fault(c, ctrl, "image retain: lock Enter stack contract");
            return;
        }
        strong = ld16(ctrl + ISAAC_IR_CTRL_STRONG);
        if (!ir_guest_unlock(c, ctrl, vtable)) {
            ++s_ir.st.fault;
            guest_fault(c, ctrl, "image retain: lock Leave stack contract");
            return;
        }
    }
    if (s_ir.retained_bytes > s_ir.budget_bytes ||
            (s_ir.headroom_bytes != 0U &&
             ir_free_bytes() < (uint64_t)s_ir.headroom_bytes))
        ++s_ir.st.deferred;                /* pressure seen, eviction postponed */
    if (strong == 1U && image != s_ir.candidate) {
        idx = ir_hash_find(image);
        if (idx == ISAAC_IR_NIL || s_ir.rec[idx].ctrl != ctrl) {
            if (vtable != 0U && s_ir.budget_bytes != 0U &&
                    s_ir.count < ISAAC_IR_RECORDS &&
                    ir_eligible(image, &bytes) &&
                    ir_present_in_manager(image, ctrl))
                ir_touch(image, ctrl, bytes, 1); /* LRU tail; no eviction */
            else
                ++s_ir.st.orphan;
        }
#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN)
        irpp_kept(image, ctrl);
#endif
    }
    (void)gpop(c);                         /* the observer's `ret` */
}

static void ir_wrap(CPU *__restrict c, void (*real)(CPU *__restrict))
{
    uint32_t pair, image, ctrl, vtable, bytes = 0U, strong;
    uint16_t idx;
#if defined(ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED)
    int touched;
#endif

    if (!s_ir.armed && !s_ir.guard) {
        real(c);
        return;
    }
    /* Single pinned CPU.  The Vita runtime has exactly one guest CPU
     * (entry_vita.c's `cpu`, bound to the main thread; see gl_bridge.c's
     * ISAAC_VITA_GL_SHIM_FASTDISPATCH note): the observer, ImageManager::Load
     * and every GL dispatch -- including the glDeleteTextures our deferred
     * Unregister triggers -- run on it, and the arm happens on it (the loop
     * head / Present).  The first armed call latches that CPU; any other CPU
     * pointer is anomalous and falls closed. */
    if (s_ir.armed && s_ir.cpu != c) {
        if (s_ir.cpu != NULL) {
            ++s_ir.st.foreign_cpu;
            ++s_ir.st.passthrough;
            real(c);
            return;
        }
        s_ir.cpu = c;
    }
    /* Frame checks are pure predicates: the real body's own GPUSH raises the
     * established stack fault on a bad frame, never this wrapper. */
    if (!guest_stack_contains(c, c->esp + 4U, 4U)) {
        ++s_ir.st.passthrough;
        real(c);
        return;
    }
    pair = ld32(c->esp + 4U);
    if (pair == 0U || (pair & 3U) != 0U) {
        ++s_ir.st.passthrough;
        real(c);
        return;
    }
    image = ld32(pair);
    ctrl = ld32(pair + 4U);
    if (image == 0U || ctrl == 0U || (image & 3U) != 0U || (ctrl & 3U) != 0U) {
        ++s_ir.st.passthrough;
        real(c);
        return;
    }
    /* Hot path: only strong == 1 (the manager is the last holder) is an
     * eviction candidate; the ~1,200/loop refcount ops stop here.  (This is
     * also every nested offer with strong >= 2: the observer's own
     * Enter/Leave no-op.) */
    if (ld16(ctrl + ISAAC_IR_CTRL_STRONG) != 1U) {
        ++s_ir.st.passthrough;
        real(c);
        return;
    }
    if (ld32(ctrl) != s_ir.vt_ctrl) {
        ++s_ir.st.passthrough;
        real(c);
        return;
    }

    /* ---- candidate: the observer would Unregister this image now ---- */
    if (!s_ir.armed) {
        /* Disarmed guard: a retained entry re-offered under the held manager
         * Mutex is kept (the observer's `ret`); anything else is __real's. */
        if (ir_manager_lock_held()) {
            ++s_ir.st.held_keep;
            (void)gpop(c);
            return;
        }
        ++s_ir.st.passthrough;
        real(c);
        return;
    }
    ++s_ir.st.offered;
    if ((s_ir.st.offered % ISAAC_IR_REPORT_EVERY) == 0U)
        isaac_vita_image_retain_report("periodic");
    vtable = ir_lock_vtable(ctrl);
    if (s_ir.in_evict || ir_manager_lock_held()) {
        /* Unregister (ours or __real's) would re-enter the manager Mutex on
         * this thread and never return: keep the entry. */
        IRB_NOTE(image, s_ir.in_evict ? "keep-nested" : "keep-held");
        ir_keep(c, image, ctrl, vtable, s_ir.in_evict != 0U);
        return;
    }
#if defined(ISAAC_VITA_IMAGE_PRELOAD)
    irp_offer(image, ctrl);
#endif
#if defined(ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED)
    touched = irt_offer(image);
#endif
    if (!ir_eligible(image, &bytes)) {
        ++s_ir.st.ineligible;
        IRB_INELIGIBLE(image);
        real(c);
        return;
    }
    if (!ir_present_in_manager(image, ctrl)) {
        /* Not registered: the real Unregister is a not-found no-op. */
        ++s_ir.st.absent;
        ++s_ir.st.passthrough;
        IRB_NOTE(image, "absent");
        real(c);
        return;
    }
    if (vtable == 0U || s_ir.budget_bytes == 0U) {
        /* Unknown lock shape, or the stage-2a measurement / kill switch
         * (budget 0): count the candidate, behave exactly like OFF. */
#if defined(ISAAC_VITA_IMAGE_RETAIN_GHOST)
        irg_offer(&s_irg, ir_name_hash(image), bytes);
        isaac_vita_image_retain_ghost_sample();
#endif
        ++s_ir.st.passthrough;
        IRB_NOTE(image, "pass-budget0");
        real(c);
        return;
    }
    if (bytes > s_ir.budget_bytes) {
        /* Larger than the whole budget: retaining it could only flush the
         * LRU to make room for a sheet that is evicted on the next offer. */
        ++s_ir.st.oversize;
        ++s_ir.st.ineligible;
        IRB_NOTE(image, "oversize");
        real(c);
        return;
    }
    /* From here on nested offers of this very image (its own bucket being
     * compacted by an eviction below) are kept without a record: this
     * wrapper records it. */
    s_ir.candidate = image;
    idx = ir_hash_find(image);
    if (s_ir.headroom_bytes != 0U) {
        uint64_t free_bytes = ir_free_bytes();   /* once per candidate */

        if (free_bytes < (uint64_t)s_ir.headroom_bytes) {
            /* Below the headroom: release LRU tails (bounded; never this
             * sheet's own record, so the Image the observer's caller still
             * points at is not destroyed under it).  Recovered -> retain
             * normally below; still short -> forget this sheet's record and
             * let the real observer free it exactly as OFF would.  Pure
             * predicate before any lock replay; see the header. */
            if (idx != ISAAC_IR_NIL) {
                ir_lru_unlink(idx);
                ir_lru_push_head(idx);
            }
            free_bytes = ir_headroom_release(
                c, free_bytes, idx != ISAAC_IR_NIL ? 1U : 0U);
#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN) && ISAAC_IRPP_ON
            if (idx != ISAAC_IR_NIL && ir_hash_find(image) != idx) {
                /* Unreachable while irpp_evictable_from_tail keeps its
                 * candidate barrier (the oracle's mutant without it lands
                 * here).  An eviction above took this sheet's own record
                 * and Unregistered it -- the observer's own effect, done --
                 * and `pair` may point into the compacted bucket: touch
                 * nothing, drop no index, emulate the observer's ret. */
                ++s_ir.st.passthrough;
                s_ir.candidate = 0U;
                IRB_NOTE(image, "headroom-self");
                (void)gpop(c);
                return;
            }
#endif
            if (free_bytes < (uint64_t)s_ir.headroom_bytes) {
                ++s_ir.st.headroom_pass;
                if (idx != ISAAC_IR_NIL) {
                    uint32_t di, dc, db;
                    ir_drop_record(idx, &di, &dc, &db);
                    ++s_ir.st.evicted;
                    s_ir.st.evicted_bytes += db;
                    ++s_ir.st.headroom_evict;
                }
                ++s_ir.st.passthrough;
                s_ir.candidate = 0U;
                IRB_NOTE(image, "headroom-pass");
                real(c);
                return;
            }
        }
    }
    if (idx == ISAAC_IR_NIL && s_ir.count >= ISAAC_IR_RECORDS) {
        /* Table full: make room by evicting the LRU tail (the guest's own
         * Unregister, on this CPU, inside a hook call where the observer
         * would itself have Unregistered).  Unevictable -> fall closed. */
        if (!ir_evict_tail(c, IR_REASON_TABLE_FULL) ||
                s_ir.count >= ISAAC_IR_RECORDS) {
            ++s_ir.st.passthrough;
            s_ir.candidate = 0U;
            IRB_NOTE(image, "pass-table");
            real(c);
            return;
        }
    }

    /* Reproduce the observer's Enter(-1) / read / Leave exactly. */
    if (!ir_guest_lock(c, ctrl, vtable)) {
        ++s_ir.st.fault;
        s_ir.candidate = 0U;
        guest_fault(c, ctrl, "image retain: lock Enter stack contract");
        return;
    }
    strong = ld16(ctrl + ISAAC_IR_CTRL_STRONG);
    if (!ir_guest_unlock(c, ctrl, vtable)) {
        ++s_ir.st.fault;
        s_ir.candidate = 0U;
        guest_fault(c, ctrl, "image retain: lock Leave stack contract");
        return;
    }
    if (strong != 1U) {
        /* Re-acquired between the unlocked pre-check and the lock: this is
         * the observer's own `jne 0x56d8f5` no-op tail -- emulate its ret. */
        ++s_ir.st.passthrough;
        s_ir.candidate = 0U;
        IRB_NOTE(image, "reacquired");
        (void)gpop(c);
        return;
    }
#if defined(ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED) && ISAAC_IRT_ON
    if (touched) {
        ++s_irt.admit;
        if (ir_hash_find(image) == ISAAC_IR_NIL)
            s_irt.admit_bytes += bytes;
    }
#elif defined(ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED)
    (void)touched;
#endif
    IRB_NOTE(image, irb_touch_what(image, ctrl));
    ir_touch(image, ctrl, bytes, 0);
#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN)
    irpp_touched(image, ctrl);
#endif
    ir_evict_as_needed(c);
    s_ir.candidate = 0U;
#if defined(ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED) && ISAAC_IRT_ON
    if (s_ir.retained_bytes > s_ir.budget_bytes && !ir_manager_lock_held()) {
        /* Rule E left the table over budget (every record an eviction could
         * reach is pinned).  The budget is a hard cap: this candidate goes
         * the way OFF sends it -- the observer's own Unregister, issued
         * here, in its own release.  Its Enter/Leave were replayed above. */
        idx = ir_hash_find(image);
        if (idx != ISAAC_IR_NIL && s_ir.rec[idx].ctrl == ctrl) {
            uint32_t di, dc, db;

            ir_drop_record(idx, &di, &dc, &db);
            ++s_irt.budget_refuse;
            IRB_NOTE(image, "pass-pinned");
            (void)ir_evict_one(c, di, dc, db, IR_REASON_BUDGET);
        }
    }
#endif
    (void)gpop(c);                         /* the observer's `ret` */
}
#endif

#if defined(ISAAC_VITA_IMAGE_RETAIN_WRAP)
void __wrap_sub_0056d8c0(CPU *__restrict c)
{
    ir_wrap(c, __real_sub_0056d8c0);
}
#endif

#if defined(ISAAC_VITA_IMAGE_RETAIN_ORACLE)
void isaac_ir_oracle_set_env(const isaac_ir_env *env)
{
    s_ir.image_base = env->image_base;
    s_ir.buckets_ptr_va = env->buckets_ptr_va;
    s_ir.hook_global_va = env->hook_global_va;
    s_ir.hook_global2_va = env->hook_global2_va;
    s_ir.vt_png = env->vt_png;
    s_ir.vt_pcx = env->vt_pcx;
    s_ir.vt_pic = env->vt_pic;
    s_ir.vt_ctrl = env->vt_ctrl;
    s_ir.vt_mutex = env->vt_mutex;
    s_ir.manager_lock_va = env->manager_lock_va;
    s_ir.env_ready = 1U;
}

void isaac_ir_oracle_set_budget(uint32_t budget_bytes, uint32_t headroom_bytes)
{
    s_ir.budget_bytes = budget_bytes;
    s_ir.headroom_bytes = headroom_bytes;
}

void isaac_ir_oracle_wrap(CPU *__restrict c, void (*real)(CPU *__restrict))
{
    ir_wrap(c, real);
}

const isaac_ir_stats *isaac_ir_oracle_stats(void) { return &s_ir.st; }
uint32_t isaac_ir_oracle_retained_bytes(void)     { return s_ir.retained_bytes; }
uint32_t isaac_ir_oracle_live_records(void)       { return s_ir.count; }
int      isaac_ir_oracle_in_evict(void)           { return s_ir.in_evict; }
int      isaac_ir_oracle_armed(void)              { return s_ir.armed; }
void     isaac_ir_oracle_saturate_stats(void)
{
    memset(&s_ir.st, 0xff, sizeof s_ir.st);
    s_ir.retained_bytes = 0xFFFFFFFFu;
    s_ir.budget_bytes = 0xFFFFFFFFu;
    s_ir.headroom_bytes = 0xFFFFFFFFu;
    s_ir.count = 0xFFFFFFFFu;
#if defined(ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED)
    memset(&s_irt, 0xff, sizeof s_irt);
#endif
#if defined(ISAAC_VITA_IMAGE_PRELOAD)
    irp_saturate();
#endif
}
#if defined(ISAAC_VITA_IMAGE_PRELOAD)
uint32_t isaac_ir_oracle_preload(unsigned field)
{
    switch (field) {
    case ISAAC_IRP_F_LEVELS:         return s_irp.levels;
    case ISAAC_IRP_F_LOADED:         return s_irp.loaded;
    case ISAAC_IRP_F_LOADED_BYTES:   return s_irp.loaded_bytes;
    case ISAAC_IRP_F_RETAINED:       return s_irp.retained;
    case ISAAC_IRP_F_DROPPED:        return s_irp.dropped;
    case ISAAC_IRP_F_PRESENT:        return s_irp.present;
    case ISAAC_IRP_F_FAIL:           return s_irp.fail;
    case ISAAC_IRP_F_HIT:            return s_irp.hit;
    case ISAAC_IRP_F_MISS:           return s_irp.miss;
    case ISAAC_IRP_F_EVICTED_UNUSED: return s_irp.evicted_unused;
    case ISAAC_IRP_F_BREACH:         return s_irp.breach;
    case ISAAC_IRP_F_SKIP_STATE:     return s_irp.skip[IRP_SKIP_STATE];
    case ISAAC_IRP_F_SKIP_LOCK:      return s_irp.skip[IRP_SKIP_LOCK];
    case ISAAC_IRP_F_SKIP_TERMINAL:  return s_irp.skip[IRP_SKIP_TERMINAL];
    case ISAAC_IRP_F_SKIP_VMEM:      return s_irp.skip[IRP_SKIP_VMEM];
    case ISAAC_IRP_F_SKIP_HEAP:      return s_irp.skip[IRP_SKIP_HEAP];
    case ISAAC_IRP_F_SKIP_STACK:     return s_irp.skip[IRP_SKIP_STACK];
    case ISAAC_IRP_F_ENTRIES:        return s_irp.n;
    default:                         return 0xFFFFFFFFu;
    }
}
#endif
#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN)
uint32_t isaac_ir_oracle_preload_pin(unsigned field)
{
    switch (field) {
    case ISAAC_IRPP_F_PINNED:       return s_irpp.pinned;
    case ISAAC_IRPP_F_REFUSED:      return s_irpp.refused;
    case ISAAC_IRPP_F_USED:         return s_irpp.unpin[IRPP_USED];
    case ISAAC_IRPP_F_FLOOR:        return s_irpp.unpin[IRPP_FLOOR];
    case ISAAC_IRPP_F_YIELD:        return s_irpp.unpin[IRPP_YIELD];
    case ISAAC_IRPP_F_STALE:        return s_irpp.unpin[IRPP_STALE];
    case ISAAC_IRPP_F_DROPPED:      return s_irpp.unpin[IRPP_DROPPED];
    case ISAAC_IRPP_F_SKIP:         return s_irpp.skip;
    case ISAAC_IRPP_F_CAND_SKIP:    return s_irpp.cand_skip;
    case ISAAC_IRPP_F_OWN:          return s_irpp.own;
    case ISAAC_IRPP_F_EXACT:        return s_irpp.exact;
    case ISAAC_IRPP_F_ALIAS:        return s_irpp.alias;
    case ISAAC_IRPP_F_HELD:         return s_irpp.held;
    case ISAAC_IRPP_F_ON_FREE:      return s_irpp.on_free;
    case ISAAC_IRPP_F_LOST:         return s_irpp.lost;
    case ISAAC_IRPP_F_TWINS:        return s_irpp.twins;
    case ISAAC_IRPP_F_PINS:         return irpp_pins();
    case ISAAC_IRPP_F_PINNED_BYTES: return s_irpp.pinned_bytes;
    case ISAAC_IRPP_F_PIN0:         return s_irpp.pin[0];
    case ISAAC_IRPP_F_PIN1:         return s_irpp.pin[1];
    case ISAAC_IRPP_F_STATE0:       return s_irp.state[0];
    case ISAAC_IRPP_F_STATE1:       return s_irp.state[1];
    default:                        return 0xFFFFFFFFu;
    }
}
#endif
#if defined(ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED)
uint32_t isaac_ir_oracle_touched(unsigned field)
{
    const uint32_t *words = (const uint32_t *)(const void *)&s_irt;

    return field < sizeof s_irt / sizeof words[0] ? words[field] : 0xFFFFFFFFu;
}
#endif
#endif
