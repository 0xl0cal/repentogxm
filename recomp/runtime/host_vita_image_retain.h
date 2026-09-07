#ifndef ISAAC_HOST_VITA_IMAGE_RETAIN_H
#define ISAAC_HOST_VITA_IMAGE_RETAIN_H

#include <stddef.h>
#include <stdint.h>

#include "guest.h"

/* ImageManager sprite-sheet retention (ISAAC_VITA_IMAGE_RETAIN, load stage 2).
 *
 * SEAM.  The frozen release observer sub_0056d8c0 (guest_0168.c, 27 insns /
 * 0x39 bytes, cdecl(pair*), sha256 fa990858...) is stored in the two SmartPointer<Image>
 * observer globals [0x7fd62c] / [0x803d00] at 0x5bfb78 / 0x5bfb84 and is
 * invoked -- always through `mov eax,[global]; call eax` -- from every
 * SmartPointer<Image> destructor whose control block just decremented its
 * strong count without reaching zero (ctrl->Release() returned true).  The
 * observer takes the control block's lock (this = ctrl+8, vt+0xc Enter(-1),
 * thiscall ret 4), reads u16 strong at ctrl+4, releases the lock (vt+0x10
 * Leave) and, when strong == 1 (the ImageManager bucket is the only holder
 * left), calls ImageManager::Unregister(image) = sub_0056d220 (ret 4), which
 * erases the bucket entry under the manager lock, releasing the manager's
 * strong reference -> strong 0 -> Image destructor -> glDeleteTextures of the
 * sheet's GL names.  So today the game frees a sprite sheet the instant its
 * last live sprite dies and every room re-decodes ~60 PNGs (~40-52 MB).
 *
 * In the v12 corpus every runtime edge into the observer is indirect (the
 * four sites in guest_0000.c at 0x2985/0x2acf/0x361d/0x43b5 and the ~260
 * other inlined SmartPointer<Image> destructors all `guest_call` the word in
 * the global; the design's "refcount direct edge" into the hook does not
 * exist -- render_vita_refcount_direct_edge devirtualises the mutex
 * Enter/Leave inside the refcount roots, not the observer call), so the
 * translated function pointer table in guest_table.c is the only symbol
 * reference and GNU ld --wrap=sub_0056d8c0 rebinds it; __real_sub_0056d8c0 is
 * the untouched translated body.  The two compiler-devirtualised copies of
 * the body (0x5bfc1a / 0x5bfc7a, `cmp eax,0x9856d8c0`, with their own
 * `call 0x56d220` at 0x5bfc41 / 0x5bfca1) sit in the code tail that installs
 * the observer (0x5bfb78 / 0x5bfb84; shared by sub_005bf8f0 in guest_0177.c
 * and the split function sub_00598c80 in guest_0172.c): they run once at
 * ImageManager construction, before this module is armed, and they are not a
 * Clear path.  No other direct sub_0056d220 caller exists in the corpus; the
 * bucket-presence scan below still guards against any Unregister that reaches
 * the manager through dispatch.
 *
 * MODEL.  When armed and the observer would Unregister an *eligible* image
 * (ImagePng/Pcx/Pic vtable, uploaded, not a render target, present in the
 * manager as {image, ctrl}, strong == 1), the wrapper reproduces the
 * observer's Enter(-1)/Leave on the control block's lock with the observer's
 * own return words and then -- instead of Unregistering -- leaves the
 * manager's strong reference in place and records the image on a
 * byte-budgeted LRU.  The image stays exactly in the state Register left it
 * ({Image*, ctrl*} in its bucket, strong == 1); a later ImageManager::Load of
 * the same name is the game's own strcmp hit -> TryAddRef path with no
 * fopen / seek / inflate / unfilter / glTexImage2D.  The retained GL texture
 * is the one the game uploaded, so rendering is byte-identical.
 *
 * EVICTION (byte budget exceeded, vitaGL free memory below the headroom,
 * record table full) calls the guest's own Unregister on the game CPU with
 * the observer's own return word, inside a hook call where the real observer
 * would itself have Unregistered, so the Image destructor, glDeleteTextures,
 * name free and control-block release run exactly as they would have, on the
 * pinned GL thread.  The module never frees a guest object natively and never
 * holds a reference of its own; it only defers the manager's self-erase.
 *
 * HEADROOM.  vglMemFree(RAM) + vglMemFree(VRAM) is sampled once per candidate
 * (two sceClibMspaceMallocStats walks), before any guest work.  Below the
 * headroom the resident set must not grow: the candidate is handed to the
 * real observer (freed exactly as OFF frees it) after at most
 * ISAAC_IR_HEADROOM_EVICT_MAX LRU tails were released through the guest
 * Unregister, and a retained sheet re-offered under pressure is forgotten
 * and freed by the real observer.  The count is bounded on purpose: vitaGL
 * 73dd57a (gpu_free_texture_data) defers the free of any texture drawn in the
 * last FRAME_PURGE_FREQ = 4 frames to its purge list, so vglMemFree cannot be
 * expected to rise inside one hook call and an unbounded "evict until free >=
 * headroom" loop would flush the whole table at every room exit for nothing.
 * A sheet larger than the budget is never retained (it could only flush the
 * LRU).  The rule protects the next room's Loads only through the margin it
 * keeps (design: worst next-room miss set ~52 MB + the largest sheet 8 MiB);
 * nothing evicts inside glTexImage2D, so ISAAC_VITA_IMAGE_RETAIN_HEADROOM_MB
 * must stay above the measured worst next-room miss set (stage 2a: first run
 * with BUDGET_MB=0 and read the receipt's `free=` / ph120.ir `vf(r/v)`).
 * Before touching a record it re-scans the manager buckets for {image, ctrl}
 * and re-reads strong, so a foreign Unregister/Clear (floor exit, mod reload,
 * shutdown) that ran behind its back can never reach a dangling pointer.
 * Anything unexpected (misaligned pair/image/ctrl, foreign Image or ctrl
 * vtable, render-target flags, tex0 unset, strong != 1, off the pinned CPU,
 * table full and unevictable, budget 0) falls closed to __real_sub_0056d8c0,
 * which then does exactly what the OFF build does.
 *
 * MANAGER LOCK (the one state the OFF build never reaches).  Unregister's
 * erase (0x56d3e0..0x56d46c) does not memmove: for every entry behind the
 * erased one it copy-constructs a temp (TryAddRef), swaps it into the hole and
 * Releases the temp -- and when that Release leaves strong >= 1 it calls the
 * observer through [0x7fd62c] with pair = &temp (0x56d427) / pair = the last
 * entry (0x56d467).  Bucket growth (sub_0056d9b0 -> _Destroy_range
 * sub_0056daf0, 0x56db43) does the same for every old entry.  In the OFF build
 * those entries all have strong >= 2 (manager + a live holder) and the observer
 * is a no-op; a *retained* entry has strong == 1, so with retention every
 * erase/growth in its bucket re-offers it to the observer while the game holds
 * the manager Mutex (0x7e7d04).  That Mutex (vtable 0x75d648, Enter =
 * sub_00562e00) is EnterCriticalSection + `while (cs->held) Sleep(1000)` +
 * held = 1: a second Enter from the holder thread never returns, so an
 * Unregister issued under it -- by the real observer or by this module's
 * eviction -- hangs the game.  Rule: when the manager Mutex's held byte is set
 * (the game's own Unregister/Register/growth on this thread) or during this
 * module's own eviction, a strong == 1 candidate is *kept*: Enter/Leave are
 * replayed, the record (if any) is left in place, nothing is evicted and
 * __real is never entered; eviction resumes at the next unlocked candidate.
 * The module refuses to arm unless the manager lock object has the frozen
 * Mutex vtable and is initialised, and it only replays Enter/Leave on control
 * blocks whose embedded lock has that same vtable.  A disarm (entry_vita.c's
 * teardown) abandons the records but keeps that rule for the strong == 1
 * entries it leaves in the manager: an offer under the held Mutex is still
 * kept, every other offer is __real's.
 *
 * COST.  The passthrough path (strong != 1: the ~1,200 refcount ops per loop)
 * pays 4 guest loads and a few compares; the bucket scan, the lock replay and
 * the free-memory query run only for strong == 1 candidates (~60 per room,
 * ~990 at floor exit). */

/* Frozen PE (8,650,240-byte Repentance, sha256 31846486...ca9404), RVAs.
 * Code/data pointers stored in guest memory carry GUEST_IMAGE_BASE; return
 * words pushed by generated/replayed call sites are plain RVAs. */
#define ISAAC_IR_HOOK_RVA             0x0056d8c0U /* the wrapped release observer */
#define ISAAC_IR_HOOK_BYTES           0x39U       /* observer body length */
#define ISAAC_IR_UNREGISTER_RVA       0x0056d220U /* ret 4 (0x56d497), arg = Image* */
#define ISAAC_IR_ENTER_SITE_RVA       0x0056d8ddU /* observer's return word after Enter */
#define ISAAC_IR_LEAVE_SITE_RVA       0x0056d8e8U /* observer's return word after Leave */
#define ISAAC_IR_UNREGISTER_SITE_RVA  0x0056d8f5U /* observer's return word after Unregister */
#define ISAAC_IR_HOOK_GLOBAL_RVA      0x007fd62cU /* SmartPointer<Image> observer (0x5bfb78) */
#define ISAAC_IR_HOOK_GLOBAL2_RVA     0x00803d00U /* second instantiation (0x5bfb84) */
#define ISAAC_IR_BUCKETS_PTR_RVA      0x007e7d00U /* -> 32 x {begin,end,cap} (12 B) */
#define ISAAC_IR_MANAGER_LOCK_RVA     0x007e7d04U /* manager lock object (Unregister takes it) */

/* The game's Mutex class (the manager lock and every control block's embedded
 * lock): {vtable, u8 initialised @+4, CRITICAL_SECTION* @+8}; the CS carries
 * the class's own `held` byte at +0x18.  Enter(-1) = sub_00562e00:
 * EnterCriticalSection, `while (held) Sleep(1000)`, held = 1 -- a re-entry by
 * the holder thread spins forever.  Leave = sub_00562ec0: held = 0,
 * LeaveCriticalSection. */
#define ISAAC_IR_MUTEX_VT_RVA         0x0075d648U /* +0xc Enter 0x562e00, +0x10 Leave 0x562ec0 */
#define ISAAC_IR_MUTEX_ENTER_RVA      0x00562e00U
#define ISAAC_IR_MUTEX_LEAVE_RVA      0x00562ec0U
#define ISAAC_IR_MUTEX_INIT           0x04U /* u8: 0 = not constructed (Enter asserts) */
#define ISAAC_IR_MUTEX_CS             0x08U /* CRITICAL_SECTION* */
#define ISAAC_IR_CS_HELD              0x18U /* u8 held flag behind the 24-byte CS */
/* Unregister's erase loop / tail observer sites (pair = temp / last entry). */
#define ISAAC_IR_UNREGISTER_SHIFT_SITE_RVA 0x0056d429U
#define ISAAC_IR_UNREGISTER_TAIL_SITE_RVA  0x0056d469U

#define ISAAC_IR_VT_PNG_RVA           0x00766028U /* ImagePng   (0x90 B) */
#define ISAAC_IR_VT_PCX_RVA           0x00765e90U /* ImagePcx   (0x110 B) */
#define ISAAC_IR_VT_PIC_RVA           0x007662f0U /* ImagePic   (0xd0 B) */
#define ISAAC_IR_CTRL_VT_RVA          0x00608360U /* control block: +4 TryAddRef +8 AddRef +c Release */

/* Image and control-block field offsets. */
#define ISAAC_IR_IMG_FLAGS            0x10U /* & 0x14 -> in a render-target list (Unregister 0x56d278/0x56d2b6) */
#define ISAAC_IR_IMG_NAME             0x3cU /* char* (Load's strcmp key) */
#define ISAAC_IR_IMG_TEX0             0x78U /* GL name; 0xffffffff from the base ctor (0x5b080b) until glGenTextures */
#define ISAAC_IR_IMG_TEX1             0x7cU /* second GL name (palette upload for colour type 3) */
#define ISAAC_IR_IMG_PW               0x84U /* padded width u16  (0x5a11c7) */
#define ISAAC_IR_IMG_PH               0x86U /* padded height u16 */
#define ISAAC_IR_IMG_BPP              0x88U /* bytes per pixel u32 (0x5a1143) */
#define ISAAC_IR_CTRL_STRONG          0x04U /* u16 strong count (incl. the manager's) */
#define ISAAC_IR_CTRL_LOCK            0x08U /* embedded lock object (vtable at +8) */
#define ISAAC_IR_LOCK_VT_ENTER        0x0cU /* thiscall Enter(-1), ret 4 */
#define ISAAC_IR_LOCK_VT_LEAVE        0x10U /* thiscall Leave(), ret */
#define ISAAC_IR_TEX_UNSET            0xffffffffU

/* Bounds. */
#define ISAAC_IR_RECORDS              1024U /* LRU capacity */
#define ISAAC_IR_HASH                 2048U /* image-pointer hash slots (power of two) */
#define ISAAC_IR_NIL                  0xFFFFU
#define ISAAC_IR_BUCKETS              32U
#define ISAAC_IR_BUCKET_STRIDE        12U
#define ISAAC_IR_MAX_SCAN_ENTRIES     4096U /* manager-scan ceiling (device: ~1,200 live) */
#define ISAAC_IR_BYTES_CAP           (16U * 1024U * 1024U) /* 2048x2048 RGBA */
#define ISAAC_IR_REPORT_EVERY         64U   /* periodic receipt cadence, in offers */
#define ISAAC_IR_HEADROOM_EVICT_MAX   8U    /* LRU tails released per candidate under headroom pressure */

/* Per-run counters (the receipt / ph120.ir fields). */
typedef struct isaac_ir_stats {
    uint32_t offered;        /* candidates: armed, well-formed, strong == 1 */
    uint32_t retained_new;   /* newly inserted retentions */
    uint32_t reuse;          /* re-offers of an already-retained image */
    uint32_t evicted;        /* records evicted through the guest Unregister */
    uint32_t evicted_bytes;
    uint32_t retained_bytes; /* live retained texel bytes (snapshot) */
    uint32_t budget_evict;   /* evictions caused by the byte budget */
    uint32_t headroom_evict; /* evictions caused by the free-memory headroom */
    uint32_t table_full;     /* evictions caused by a full record table */
    uint32_t dropped_stale;  /* records dropped without Unregister (gone / replaced / re-acquired) */
    uint32_t passthrough;    /* armed wrapper entries routed to __real */
    uint32_t ineligible;     /* candidates that failed the eligibility gate */
    uint32_t absent;         /* candidates not present in the manager buckets */
    uint32_t nested;         /* strong == 1 candidates offered during our own eviction (kept) */
    uint32_t foreign_cpu;    /* hook calls off the pinned CPU (passthrough) */
    uint32_t fault;          /* replay stack-contract breaches */
    uint32_t peak_bytes;     /* high-water retained bytes */
    uint32_t held_keep;      /* strong == 1 candidates offered under the game's manager Mutex (kept) */
    uint32_t orphan;         /* kept candidates that could not get a record (table full under the lock) */
    uint32_t deferred;       /* budget/headroom evictions postponed because the manager Mutex was held */
    uint32_t oversize;       /* eligible candidates larger than the budget (never retained) */
    uint32_t headroom_pass;  /* candidates handed to the real observer under headroom pressure */
} isaac_ir_stats;

/* Snapshot for the ph120.ir record (cumulative counters + instantaneous). */
typedef struct isaac_ir_window {
    isaac_ir_stats totals;
    uint32_t live_records;
    uint32_t budget_bytes;
    uint32_t headroom_bytes;
    uint32_t armed;
    uint32_t vf_ram;         /* vglMemFree(VGL_MEM_RAM) */
    uint32_t vf_vram;        /* vglMemFree(VGL_MEM_VRAM) */
} isaac_ir_window;

/* Latch the frozen addresses, the budget/headroom and the armed flag; log the
 * `KAGE VITA IMAGE RETAIN ARM:` banner once.  Refuses to arm when the observer
 * globals do not hold the frozen hook.  Idempotent; no guest work. */
void isaac_vita_image_retain_arm(void);

/* Abandon every record without touching the guest (the game's own Clear path
 * unregisters everything at teardown) and stop retaining. */
void isaac_vita_image_retain_disarm(void);

/* Emit the `KAGE VITA IMAGE RETAIN: offered=...` receipt tagged `why`, then
 * the `KAGE VITA IMAGE RETAIN MEM: free=...` diagnostics line.  Two lines so
 * each fits isaac_vita_log's 384-byte body even at u32-max counters. */
void isaac_vita_image_retain_report(const char *why);

/* Snapshot the counters for ph120.ir. */
void isaac_vita_image_retain_window(isaac_ir_window *out);

#if defined(ISAAC_VITA_IMAGE_RETAIN_WRAP)
void __wrap_sub_0056d8c0(CPU *__restrict c);
void __real_sub_0056d8c0(CPU *__restrict c);
#endif

/* Oracle seam.  The host oracle replaces the frozen VAs by host-buffer
 * addresses and the vitaGL free-memory query by a callback; the guest replays
 * (Enter/Leave/Unregister through gpush + guest_call) are the production
 * code, driven by the oracle's own guest_call over a fake manager. */
#if defined(ISAAC_VITA_IMAGE_RETAIN_ORACLE)
typedef struct isaac_ir_env {
    uint32_t image_base;
    uint32_t buckets_ptr_va;
    uint32_t hook_global_va, hook_global2_va;
    uint32_t vt_png, vt_pcx, vt_pic, vt_ctrl;
    uint32_t vt_mutex;       /* the lock class vtable (manager lock + ctrl locks) */
    uint32_t manager_lock_va;
} isaac_ir_env;

void isaac_ir_oracle_set_env(const isaac_ir_env *env);
void isaac_ir_oracle_set_budget(uint32_t budget_bytes, uint32_t headroom_bytes);
void isaac_ir_oracle_wrap(CPU *__restrict c, void (*real)(CPU *__restrict));
const isaac_ir_stats *isaac_ir_oracle_stats(void);
uint32_t isaac_ir_oracle_retained_bytes(void);
uint32_t isaac_ir_oracle_live_records(void);
int      isaac_ir_oracle_in_evict(void);
int      isaac_ir_oracle_armed(void);
/* Saturate every counter/byte field (u32 max) so the oracle can measure the
 * worst-case receipt line lengths against isaac_vita_log's body. */
void     isaac_ir_oracle_saturate_stats(void);
/* The oracle supplies the free-memory model (RAM, VRAM). */
extern uint32_t isaac_ir_oracle_free_ram(void);
extern uint32_t isaac_ir_oracle_free_vram(void);
#endif

#endif /* ISAAC_HOST_VITA_IMAGE_RETAIN_H */
