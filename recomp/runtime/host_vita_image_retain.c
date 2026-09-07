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
#else
static uint32_t ir_free_ram(void)  { return (uint32_t)vglMemFree(VGL_MEM_RAM); }
static uint32_t ir_free_vram(void) { return (uint32_t)vglMemFree(VGL_MEM_VRAM); }
#endif

static uint64_t ir_free_bytes(void)
{
    return (uint64_t)ir_free_ram() + (uint64_t)ir_free_vram();
}

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

/* Eligibility of an Image the observer is about to Unregister.  Every read is
 * an ld32/ld16 of guest memory the real observer/Unregister would read too. */
static int ir_eligible(uint32_t image, uint32_t *bytes_out)
{
    uint32_t flags, tex0, name, pw, ph, bpp;
    uint64_t bytes;

    if (!ir_image_vtable_ok(image))
        return 0;
    flags = ld32(image + ISAAC_IR_IMG_FLAGS);
    if ((flags & 0x14U) != 0U)             /* in a render-target list */
        return 0;
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
        return 0;
    }
    s_ir.in_evict = 1U;
    if (!ir_guest_unregister(c, image)) {
        s_ir.in_evict = 0U;
        ++s_ir.st.fault;
        guest_fault(c, image, "image retain: Unregister stack contract");
        return 0;
    }
    s_ir.in_evict = 0U;
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
static int ir_evict_tail(CPU *__restrict c, enum ir_reason reason)
{
    uint16_t idx = s_ir.tail;
    uint32_t image, ctrl, bytes;

    if (idx == ISAAC_IR_NIL)
        return 0;
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
 * which the caller has moved to the head: `keep` records stay) until free
 * memory is back above the headroom or ISAAC_IR_HEADROOM_EVICT_MAX tails
 * went.  Bounded because vitaGL defers the free of a recently drawn texture
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
    }
    (void)gpop(c);                         /* the observer's `ret` */
}

static void ir_wrap(CPU *__restrict c, void (*real)(CPU *__restrict))
{
    uint32_t pair, image, ctrl, vtable, bytes = 0U, strong;
    uint16_t idx;

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
        ir_keep(c, image, ctrl, vtable, s_ir.in_evict != 0U);
        return;
    }
    if (!ir_eligible(image, &bytes)) {
        ++s_ir.st.ineligible;
        real(c);
        return;
    }
    if (!ir_present_in_manager(image, ctrl)) {
        /* Not registered: the real Unregister is a not-found no-op. */
        ++s_ir.st.absent;
        ++s_ir.st.passthrough;
        real(c);
        return;
    }
    if (vtable == 0U || s_ir.budget_bytes == 0U) {
        /* Unknown lock shape, or the stage-2a measurement / kill switch
         * (budget 0): count the candidate, behave exactly like OFF. */
        ++s_ir.st.passthrough;
        real(c);
        return;
    }
    if (bytes > s_ir.budget_bytes) {
        /* Larger than the whole budget: retaining it could only flush the
         * LRU to make room for a sheet that is evicted on the next offer. */
        ++s_ir.st.oversize;
        ++s_ir.st.ineligible;
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
        (void)gpop(c);
        return;
    }
    ir_touch(image, ctrl, bytes, 0);
    ir_evict_as_needed(c);
    s_ir.candidate = 0U;
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
}
#endif
