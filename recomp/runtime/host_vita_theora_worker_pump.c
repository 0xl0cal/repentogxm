/* ISAAC_VITA_THEORA_WORKER_PUMP: run theoraplayer's decoder worker from the
 * main thread, one bounded slice per game frame.
 *
 * theoraplayer decodes video on a worker thread.  TheoraVideoManager's
 * constructor (sub_005ac900) calls createWorkerThreads (sub_005ad490), which
 * builds a TheoraWorkerThread (0x1c bytes: vtable, mRunning byte at +4, self
 * pointer at +8, the loop entry sub_005c2dc0 at +0xc, the Win32 handle at
 * +0x10, mStarted byte at +0x14, the assigned clip at +0x18) and starts it
 * with KERNEL32!CreateThread(0, 0, sub_005a5720, this+8, 0, 0).  The Vita
 * build leaves CreateThread unresolved and gen_all's
 * THEORAPLAYER_PC_WORKER_BYPASS skips createWorkerThreads whenever the KAGE
 * backend is up, so the game boots with zero decoder workers: a video clip
 * loads, its frame queue is built, and no frame is ever decoded (black
 * screen, the cutscene never ends; seen 2026-09-10 on the popfd3 build once
 * the popfd, bad_alloc and getenv layers were fixed).
 *
 * The worker loop (TheoraWorkerThread::execute, sub_005c2dc0, cdecl with the
 * thread object as its one argument) is plain guest code:
 *
 *   while (mRunning && mStarted) {
 *     clip = manager->requestWork(this);        // sub_005ad6a0, mWorkMutex
 *     if (!clip) { Sleep(100); continue; }      // tail re-checks mRunning
 *     lock(clip->mThreadAccessMutex);           // sub_005a5550 on [clip+0x38]
 *     if (clip->mSeekFrame >= 0) clip->doSeek();  // vt+0x1c
 *     ok = clip->decodeNextFrame();             // vt+0x10 = sub_005aa6d0
 *     if (clip->mAssignedWorkerThread == this) clip->mAssignedWorkerThread = 0;
 *     unlock;                                   // LeaveCriticalSection + flag
 *     if (!ok) Sleep(1);                        // queue full
 *   }
 *
 * This file owns a host-resident TheoraWorkerThread image (never registered
 * in the manager's vector; requestWork only stores and compares the pointer)
 * and runs one slice of that loop through guest_call from the game's own
 * per-frame TheoraVideoManager::update (sub_00598d60: called once per
 * main-loop iteration at 0x48bdf0 with CL=0 and from sub_00475810 at 0x476b33
 * with CL=1; the callers live in guest_0140.c/guest_0137.c, the callee in
 * guest_0172.c, so --wrap catches both calls; the body locks mWorkMutex, runs
 * every clip's update and decodedAudioCheck, unlocks).
 * The slice runs before the untouched body, so the frames it decodes are
 * visible to the same frame's getNextFrame (sub_00599db0, the renderer's
 * frame fetch at 0x4b89b3).  The frame-queue accessors the game also calls
 * (getFirstAvailableFrame 0x5a4bb0, getReadyCount 0x5a4c00) sit behind the
 * clip's end-of-file byte at +0x74 and are not reached before the video has
 * been decoded to its end, so they are no use as a trigger.  A slice ends
 * when the loop sleeps (no work or queue full: the Sleep shim clears mRunning
 * and returns without delaying) or when it has decoded
 * ISAAC_VITA_THEORA_WORKER_PUMP frames (the decodeNextFrame seam clears
 * mRunning), so one game frame pays for at most that many decodes.  Slices
 * are at least VITA_THEORA_PUMP_MIN_INTERVAL_US apart.
 *
 * hltypes' Mutex::lock (sub_005a5550) treats a re-lock by the running thread
 * as a deadlock and spins in Sleep(1) forever.  The main thread holds a clip's
 * mThreadAccessMutex inside the game's own restart wrapper (0x5950bd) and
 * decodeNextFrame takes the clip's audio mutex ([clip+0x34]), so a slice runs
 * only while none of mWorkMutex, mThreadAccessMutex or the audio mutex of any
 * clip is held (their lock byte at +4 is 0); otherwise the slice is skipped
 * and counted.  A Sleep from that spin site during a slice faults instead of
 * hanging the device.
 *
 * Everything here is compiled only when the knob is > 0; the OFF build has no
 * object, no link option, an unchanged host_vita_sync.c and byte-identical
 * units. */
#include <stdint.h>
#include <stdio.h>

#include "guest.h"
#include "kage_vita_backend.h"

#if !defined(ISAAC_VITA_THEORA_WORKER_PUMP) || \
    ISAAC_VITA_THEORA_WORKER_PUMP <= 0
#error "host_vita_theora_worker_pump.c is compiled only with ISAAC_VITA_THEORA_WORKER_PUMP > 0"
#endif

/* recomp/vita/platform.h; declared here so a host oracle can stub it. */
void isaac_vita_log(const char *format, ...);

void __real_sub_00598d60(CPU *__restrict c);  /* TheoraVideoManager::update (game-inlined) */
void __real_sub_005aa6d0(CPU *__restrict c);  /* TheoraVideoClip_Theora::decodeNextFrame */

/* Frozen PE facts (RVAs and field offsets), pinned by
 * recomp/test_theora_worker_pump_sites.py. */
#define VITA_THEORA_MANAGER_SINGLETON_RVA    0x007fec00U  /* TheoraVideoManager* */
#define VITA_THEORA_WORKER_LOOP_RVA          0x005c2dc0U  /* TheoraWorkerThread::execute */
#define VITA_THEORA_WORKER_VTABLE_RVA        0x00769338U  /* TheoraWorkerThread vtable */
#define VITA_THEORA_WORKER_SLEEP_RETURN_RVA  0x005c2ea3U  /* after `call edi` (Sleep) */
#define VITA_THEORA_MUTEX_SPIN_RETURN_RVA    0x005a5669U  /* Mutex::lock re-lock spin */
#define VITA_THEORA_WORKER_SIZE              0x1cU
#define VITA_THEORA_WORKER_RUNNING_OFFSET    0x4U   /* byte: hltypes::Thread::mRunning */
#define VITA_THEORA_WORKER_SELF_OFFSET       0x8U   /* Thread*: the CreateThread parameter */
#define VITA_THEORA_WORKER_FUNCTION_OFFSET   0xcU   /* loop entry */
#define VITA_THEORA_WORKER_HANDLE_OFFSET     0x10U  /* Win32 thread handle */
#define VITA_THEORA_WORKER_STARTED_OFFSET    0x14U  /* byte: mStarted */
#define VITA_THEORA_WORKER_CLIP_OFFSET       0x18U  /* TheoraWorkerThread::mClip */
#define VITA_THEORA_MANAGER_CLIPS_BEGIN      0x10U  /* std::vector<TheoraVideoClip*> */
#define VITA_THEORA_MANAGER_CLIPS_END        0x14U
#define VITA_THEORA_MANAGER_WORK_MUTEX       0x28U  /* hltypes::Mutex* mWorkMutex */
#define VITA_THEORA_CLIP_ASSIGNED_WORKER     0x30U  /* TheoraVideoClip::mAssignedWorkerThread */
#define VITA_THEORA_CLIP_AUDIO_MUTEX         0x34U  /* hltypes::Mutex* mAudioMutex */
#define VITA_THEORA_CLIP_THREAD_MUTEX        0x38U  /* hltypes::Mutex* mThreadAccessMutex */
#define VITA_THEORA_MUTEX_LOCKED_OFFSET      0x4U   /* byte in hltypes::Mutex */
#define VITA_THEORA_MAX_CLIPS                64U
#define VITA_THEORA_PUMP_MIN_INTERVAL_US     12000U
#define VITA_THEORA_PUMP_RETURN_SENTINEL     0xfff15343U

/* The host-resident TheoraWorkerThread.  Guest address == host address. */
static uint32_t s_vita_theora_worker[VITA_THEORA_WORKER_SIZE / 4U];

static int s_vita_theora_pump_active;
static uint32_t s_vita_theora_pump_budget_left;
static uint64_t s_vita_theora_pump_last_us;
static uint32_t s_vita_theora_pump_slices;
static uint32_t s_vita_theora_pump_decodes;
static uint32_t s_vita_theora_pump_sleeps;
static uint32_t s_vita_theora_pump_sleeps_no_work;  /* Sleep(100): queue full or clip busy */
static uint32_t s_vita_theora_pump_budget_stops;
static uint32_t s_vita_theora_pump_skipped_locked;
static uint32_t s_vita_theora_pump_skipped_gate;
static uint32_t s_vita_theora_pump_skipped_idle;
static uint32_t s_vita_theora_pump_locked_logged;
static uint32_t s_vita_theora_pump_slice_us_last;
static uint32_t s_vita_theora_pump_slice_us_max;
static uint64_t s_vita_theora_pump_slice_us_sum;

static void vita_theora_count(uint32_t *counter)
{
    if (*counter != UINT32_MAX)
        ++*counter;
}

static uint32_t vita_theora_worker_address(void)
{
    return (uint32_t)(uintptr_t)s_vita_theora_worker;
}

static void vita_theora_worker_init(void)
{
    uint32_t worker = vita_theora_worker_address();

    if (s_vita_theora_worker[0])
        return;
    s_vita_theora_worker[0] = GUEST_IMAGE_BASE + VITA_THEORA_WORKER_VTABLE_RVA;
    st8(worker + VITA_THEORA_WORKER_RUNNING_OFFSET, 0U);
    st32(worker + VITA_THEORA_WORKER_SELF_OFFSET, worker);
    st32(worker + VITA_THEORA_WORKER_FUNCTION_OFFSET,
         GUEST_IMAGE_BASE + VITA_THEORA_WORKER_LOOP_RVA);
    st32(worker + VITA_THEORA_WORKER_HANDLE_OFFSET, 0U);
    st8(worker + VITA_THEORA_WORKER_STARTED_OFFSET, 1U);
    st32(worker + VITA_THEORA_WORKER_CLIP_OFFSET, 0U);
    isaac_vita_log(
        "KAGE VITA THEORA PUMP: armed worker=0x%08x loop=0x%08x "
        "budget=%u interval_us=%u",
        (unsigned)worker,
        (unsigned)(GUEST_IMAGE_BASE + VITA_THEORA_WORKER_LOOP_RVA),
        (unsigned)ISAAC_VITA_THEORA_WORKER_PUMP,
        (unsigned)VITA_THEORA_PUMP_MIN_INTERVAL_US);
}

static int vita_theora_mutex_held(uint32_t mutex)
{
    if (!mutex || (mutex & 3U))
        return 0;
    return ld8(mutex + VITA_THEORA_MUTEX_LOCKED_OFFSET) != 0U;
}

static uint64_t vita_theora_now_us(void)
{
    double seconds = 0.0;

    if (!kage_vita_backend_time_seconds(&seconds) || seconds < 0.0)
        return 0U;
    return (uint64_t)(seconds * 1000000.0);
}

/* Bounded receipt for the one skip reason that would hide a real problem. */
static void vita_theora_pump_note_locked(uint32_t site, uint32_t manager,
                                         uint32_t clip, uint32_t work_mutex)
{
    vita_theora_count(&s_vita_theora_pump_skipped_locked);
    if (s_vita_theora_pump_locked_logged >= 4U)
        return;
    ++s_vita_theora_pump_locked_logged;
    isaac_vita_log(
        "KAGE VITA THEORA PUMP: skip locked site=0x%08x manager=0x%08x "
        "clip=0x%08x work=%u thread=%u audio=%u assigned=0x%08x",
        (unsigned)site, (unsigned)manager, (unsigned)clip,
        (unsigned)vita_theora_mutex_held(work_mutex),
        clip ? (unsigned)vita_theora_mutex_held(
            ld32(clip + VITA_THEORA_CLIP_THREAD_MUTEX)) : 0U,
        clip ? (unsigned)vita_theora_mutex_held(
            ld32(clip + VITA_THEORA_CLIP_AUDIO_MUTEX)) : 0U,
        (unsigned)(clip ? ld32(clip + VITA_THEORA_CLIP_ASSIGNED_WORKER) : 0U));
}

/* 1 when the manager has clips and no theora mutex the slice would take is
 * held by the calling (main) thread. */
static int vita_theora_pump_ready(uint32_t site)
{
    uint32_t manager = ld32(GUEST_IMAGE_BASE + VITA_THEORA_MANAGER_SINGLETON_RVA);
    uint32_t begin, end, slot, work_mutex;

    if (!manager || (manager & 3U))
        return 0;
    begin = ld32(manager + VITA_THEORA_MANAGER_CLIPS_BEGIN);
    end = ld32(manager + VITA_THEORA_MANAGER_CLIPS_END);
    if (begin == end || (begin & 3U) || end < begin ||
            end - begin > VITA_THEORA_MAX_CLIPS * 4U) {
        vita_theora_count(&s_vita_theora_pump_skipped_idle);
        return 0;
    }
    work_mutex = ld32(manager + VITA_THEORA_MANAGER_WORK_MUTEX);
    if (vita_theora_mutex_held(work_mutex)) {
        vita_theora_pump_note_locked(site, manager, 0U, work_mutex);
        return 0;
    }
    for (slot = begin; slot != end; slot += 4U) {
        uint32_t clip = ld32(slot);

        if (!clip || (clip & 3U))
            return 0;
        if (ld32(clip + VITA_THEORA_CLIP_ASSIGNED_WORKER) != 0U ||
                vita_theora_mutex_held(ld32(clip + VITA_THEORA_CLIP_THREAD_MUTEX)) ||
                vita_theora_mutex_held(ld32(clip + VITA_THEORA_CLIP_AUDIO_MUTEX))) {
            vita_theora_pump_note_locked(site, manager, clip, work_mutex);
            return 0;
        }
    }
    return 1;
}

#if defined(ISAAC_VITA_THEORA_WORKER_PUMP_WATCHDOG) && \
    ISAAC_VITA_THEORA_WORKER_PUMP_WATCHDOG > 0
/* Diagnostic watchdog (ISAAC_VITA_THEORA_WORKER_PUMP_WATCHDOG, default OFF):
 * a thread on another core that, once the pump is armed, reports when no
 * slice has completed for five seconds: pump/worker state, the first clip's
 * fields and mutex bytes, and the return addresses found on the game
 * thread's guest stack (c->esp is published at every call boundary).  Reads
 * only; at most three reports. */
#include <psp2/kernel/threadmgr.h>

#define VITA_THEORA_WATCH_QUIET_US       5000000ULL
#define VITA_THEORA_WATCH_REPEAT_US      10000000ULL
#define VITA_THEORA_WATCH_POLL_US        500000U
#define VITA_THEORA_WATCH_MAX_REPORTS    3U
#define VITA_THEORA_WATCH_SCAN_WORDS     512U
#define VITA_THEORA_WATCH_MAX_HITS       24U
/* Stack words are bare RVAs (see the Sleep intercept); .text of the frozen
 * PE is 0x1000..0x605xxx. */
#define VITA_THEORA_WATCH_TEXT_BEGIN     0x00001000U
#define VITA_THEORA_WATCH_TEXT_END       0x00606000U
#define VITA_THEORA_CLIP_FRAME_QUEUE     0x1cU
#define VITA_THEORA_CLIP_TIMER           0x28U
#define VITA_THEORA_CLIP_END_OF_FILE     0x74U
#define VITA_THEORA_CLIP_SEEK_FRAME      0x88U
#define VITA_THEORA_CLIP_FIRST_SHOWN     0x90U
#define VITA_THEORA_CLIP_RESTARTED       0x91U
#define VITA_THEORA_CLIP_ITERATION       0x94U
#define VITA_THEORA_CLIP_DROPPED         0x9cU
#define VITA_THEORA_CLIP_DISPLAYED       0xa0U
#define VITA_THEORA_CLIP_LAST_FRAME      0x524U
#define VITA_THEORA_QUEUE_SIZE           0x4U
#define VITA_THEORA_QUEUE_MUTEX          0xcU

static CPU *s_vita_theora_watch_cpu;
static SceUID s_vita_theora_watch_thread = -1;
static uint32_t s_vita_theora_watch_reports;

/* `rva` is a stack word; returns the call-site kind when the bytes before
 * GUEST_IMAGE_BASE + rva decode to a call, and the direct target's RVA. */
static char vita_theora_watch_site_kind(uint32_t rva, uint32_t *target)
{
    uint32_t ret = GUEST_IMAGE_BASE + rva;

    *target = 0U;
    if (rva < VITA_THEORA_WATCH_TEXT_BEGIN + 8U || rva >= VITA_THEORA_WATCH_TEXT_END)
        return 0;
    if (ld8(ret - 5U) == 0xe8U) {
        *target = rva + ld32(ret - 4U);
        return 'd';
    }
    if (ld8(ret - 6U) == 0xffU && (ld8(ret - 5U) & 0x38U) == 0x10U)
        return 'm';
    if (ld8(ret - 2U) == 0xffU && (ld8(ret - 1U) & 0xf8U) == 0xd0U)
        return 'r';
    if (ld8(ret - 3U) == 0xffU && (ld8(ret - 2U) & 0xf8U) == 0x50U)
        return 'b';
    return 0;
}

static void vita_theora_watch_dump(const char *why, uint64_t quiet_us)
{
    CPU *c = __atomic_load_n(&s_vita_theora_watch_cpu, __ATOMIC_ACQUIRE);
    uint32_t worker = vita_theora_worker_address();
    uint32_t manager = ld32(GUEST_IMAGE_BASE + VITA_THEORA_MANAGER_SINGLETON_RVA);
    uint32_t esp = 0U, floor = 0U, ceiling = 0U;
    char line[VITA_THEORA_WATCH_MAX_HITS * 20U + 8U];
    uint32_t hits = 0U, i, used = 0U;

    if (c) {
        esp = *(volatile const uint32_t *)&c->esp;
        floor = c->stack_floor;
        ceiling = c->stack_ceiling;
    }
    isaac_vita_log(
        "KAGE VITA THEORA WATCH: why=%s quiet_ms=%u active=%u budget=%u "
        "worker(run,started,clip)=%u,%u,0x%08x slices=%u decodes=%u sleeps=%u "
        "budget_stops=%u skipped(locked,gate,idle)=%u,%u,%u esp=0x%08x "
        "stack=0x%08x..0x%08x fault=%s manager=0x%08x",
        why, (unsigned)(quiet_us / 1000U),
        (unsigned)s_vita_theora_pump_active,
        (unsigned)s_vita_theora_pump_budget_left,
        (unsigned)ld8(worker + VITA_THEORA_WORKER_RUNNING_OFFSET),
        (unsigned)ld8(worker + VITA_THEORA_WORKER_STARTED_OFFSET),
        (unsigned)ld32(worker + VITA_THEORA_WORKER_CLIP_OFFSET),
        (unsigned)s_vita_theora_pump_slices, (unsigned)s_vita_theora_pump_decodes,
        (unsigned)s_vita_theora_pump_sleeps, (unsigned)s_vita_theora_pump_budget_stops,
        (unsigned)s_vita_theora_pump_skipped_locked,
        (unsigned)s_vita_theora_pump_skipped_gate,
        (unsigned)s_vita_theora_pump_skipped_idle,
        (unsigned)esp, (unsigned)floor, (unsigned)ceiling,
        (c && c->fault) ? c->fault : "-", (unsigned)manager);
    if (manager && !(manager & 3U)) {
        uint32_t begin = ld32(manager + VITA_THEORA_MANAGER_CLIPS_BEGIN);
        uint32_t end = ld32(manager + VITA_THEORA_MANAGER_CLIPS_END);
        uint32_t clip = (begin != end && !(begin & 3U)) ? ld32(begin) : 0U;

        if (clip && !(clip & 3U)) {
            uint32_t queue = ld32(clip + VITA_THEORA_CLIP_FRAME_QUEUE);

            isaac_vita_log(
                "KAGE VITA THEORA WATCH CLIP: clips=%u clip=0x%08x eof=%u "
                "seek=%d assigned=0x%08x shown=%u restarted=%u iteration=%u "
                "dropped=%u displayed=%u last_frame=%u timer=0x%08x "
                "mutex(work,thread,audio,queue)=%u,%u,%u,%u queue=0x%08x size=%u",
                (unsigned)((end - begin) / 4U), (unsigned)clip,
                (unsigned)ld8(clip + VITA_THEORA_CLIP_END_OF_FILE),
                (int)ld32(clip + VITA_THEORA_CLIP_SEEK_FRAME),
                (unsigned)ld32(clip + VITA_THEORA_CLIP_ASSIGNED_WORKER),
                (unsigned)ld8(clip + VITA_THEORA_CLIP_FIRST_SHOWN),
                (unsigned)ld8(clip + VITA_THEORA_CLIP_RESTARTED),
                (unsigned)ld32(clip + VITA_THEORA_CLIP_ITERATION),
                (unsigned)ld32(clip + VITA_THEORA_CLIP_DROPPED),
                (unsigned)ld32(clip + VITA_THEORA_CLIP_DISPLAYED),
                (unsigned)ld32(clip + VITA_THEORA_CLIP_LAST_FRAME),
                (unsigned)ld32(clip + VITA_THEORA_CLIP_TIMER),
                (unsigned)vita_theora_mutex_held(ld32(manager + VITA_THEORA_MANAGER_WORK_MUTEX)),
                (unsigned)vita_theora_mutex_held(ld32(clip + VITA_THEORA_CLIP_THREAD_MUTEX)),
                (unsigned)vita_theora_mutex_held(ld32(clip + VITA_THEORA_CLIP_AUDIO_MUTEX)),
                (queue && !(queue & 3U))
                    ? (unsigned)vita_theora_mutex_held(ld32(queue + VITA_THEORA_QUEUE_MUTEX)) : 0U,
                (unsigned)queue,
                (unsigned)((queue && !(queue & 3U)) ? ld32(queue + VITA_THEORA_QUEUE_SIZE) : 0U));
        }
    }
    if (!floor || floor >= ceiling || esp < floor || esp > ceiling - 4U || (esp & 3U))
        return;
    line[0] = 0;
    for (i = 0U; i < VITA_THEORA_WATCH_SCAN_WORDS && hits < VITA_THEORA_WATCH_MAX_HITS; ++i) {
        uint32_t address = esp + i * 4U;
        uint32_t value, target;
        char kind;
        int n;

        if (address > ceiling - 4U)
            break;
        value = ld32(address);
        kind = vita_theora_watch_site_kind(value, &target);
        if (!kind)
            continue;
        if (kind == 'd')
            n = snprintf(line + used, sizeof line - used, " %08x>%08x",
                         (unsigned)value, (unsigned)target);
        else
            n = snprintf(line + used, sizeof line - used, " %08x%c",
                         (unsigned)value, kind);
        if (n <= 0 || (size_t)n >= sizeof line - used)
            break;
        used += (uint32_t)n;
        ++hits;
    }
    isaac_vita_log("KAGE VITA THEORA WATCH STACK: hits=%u words=%u%s",
                   (unsigned)hits, (unsigned)i, line);
}

static int vita_theora_watch_thread(SceSize args, void *argp)
{
    uint32_t seen = s_vita_theora_pump_slices;
    uint64_t quiet_since = vita_theora_now_us();
    uint64_t last_report = 0U;

    (void)args;
    (void)argp;
    for (;;) {
        uint32_t slices;
        uint64_t now;

        sceKernelDelayThread(VITA_THEORA_WATCH_POLL_US);
        slices = __atomic_load_n(&s_vita_theora_pump_slices, __ATOMIC_RELAXED);
        now = vita_theora_now_us();
        if (slices != seen) {
            seen = slices;
            quiet_since = now;
            continue;
        }
        if (now - quiet_since < VITA_THEORA_WATCH_QUIET_US)
            continue;
        if (s_vita_theora_watch_reports >= VITA_THEORA_WATCH_MAX_REPORTS)
            continue;
        if (last_report && now - last_report < VITA_THEORA_WATCH_REPEAT_US)
            continue;
        last_report = now;
        ++s_vita_theora_watch_reports;
        vita_theora_watch_dump("no-slice", now - quiet_since);
    }
    return 0;
}

static void vita_theora_watch_arm(CPU *__restrict c)
{
    SceUID thread;

    if (s_vita_theora_watch_thread >= 0)
        return;
    __atomic_store_n(&s_vita_theora_watch_cpu, c, __ATOMIC_RELEASE);
    thread = sceKernelCreateThread("isaac_theora_watch", vita_theora_watch_thread,
                                   0x10000100, 0x4000U, 0U,
                                   SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (thread < 0) {
        isaac_vita_log("KAGE VITA THEORA WATCH: create failed rc=0x%08x", (unsigned)thread);
        s_vita_theora_watch_thread = 0;
        return;
    }
    s_vita_theora_watch_thread = thread;
    isaac_vita_log("KAGE VITA THEORA WATCH: armed thread=0x%08x start=0x%08x quiet_ms=%u",
                   (unsigned)thread, (unsigned)sceKernelStartThread(thread, 0U, NULL),
                   (unsigned)(VITA_THEORA_WATCH_QUIET_US / 1000U));
}
#else
static void vita_theora_watch_arm(CPU *__restrict c)
{
    (void)c;
}
#endif

static void vita_theora_pump_slice(CPU *__restrict c, uint32_t site)
{
    uint32_t worker = vita_theora_worker_address();
    uint32_t saved_esp = c->esp;
    uint32_t saved_eax = c->eax, saved_ecx = c->ecx, saved_edx = c->edx;
    uint32_t decodes_before = s_vita_theora_pump_decodes;
    uint64_t now;

    if (s_vita_theora_pump_active || c->fault)
        return;
    now = vita_theora_now_us();
    if (s_vita_theora_pump_slices &&
            now - s_vita_theora_pump_last_us < VITA_THEORA_PUMP_MIN_INTERVAL_US) {
        vita_theora_count(&s_vita_theora_pump_skipped_gate);
        return;
    }
    if (!vita_theora_pump_ready(site))
        return;
    vita_theora_worker_init();
    vita_theora_watch_arm(c);
    s_vita_theora_pump_last_us = now;
    s_vita_theora_pump_active = 1;
    s_vita_theora_pump_budget_left = ISAAC_VITA_THEORA_WORKER_PUMP;
    st8(worker + VITA_THEORA_WORKER_RUNNING_OFFSET, 1U);
    st8(worker + VITA_THEORA_WORKER_STARTED_OFFSET, 1U);
    st32(worker + VITA_THEORA_WORKER_CLIP_OFFSET, 0U);

    /* cdecl: the loop pops the sentinel with its ret and leaves the argument
     * word for the caller (Thread::process does `add esp, 4` at 0x5a5916). */
    gpush(c, worker);
    gpush(c, VITA_THEORA_PUMP_RETURN_SENTINEL);
    guest_call(c, GUEST_IMAGE_BASE + VITA_THEORA_WORKER_LOOP_RVA);
    s_vita_theora_pump_active = 0;
    {
        uint64_t end = vita_theora_now_us();
        uint32_t slice_us = (now && end > now) ? (uint32_t)(end - now) : 0U;

        s_vita_theora_pump_slice_us_last = slice_us;
        if (slice_us > s_vita_theora_pump_slice_us_max)
            s_vita_theora_pump_slice_us_max = slice_us;
        s_vita_theora_pump_slice_us_sum += slice_us;
    }
    st8(worker + VITA_THEORA_WORKER_RUNNING_OFFSET, 0U);
    if (c->fault)
        return;
    if (c->esp != saved_esp - 4U) {
        guest_fault(c, GUEST_IMAGE_BASE + VITA_THEORA_WORKER_LOOP_RVA,
                    "theora worker pump: the worker loop broke the cdecl stack");
        return;
    }
    c->esp = saved_esp;
    c->eax = saved_eax;
    c->ecx = saved_ecx;
    c->edx = saved_edx;
    vita_theora_count(&s_vita_theora_pump_slices);
    if (s_vita_theora_pump_slices <= 8U ||
            (s_vita_theora_pump_slices & 255U) == 0U)
        isaac_vita_log(
            "KAGE VITA THEORA PUMP: site=0x%08x slices=%u decodes=%u (+%u) "
            "sleeps=%u (no_work=%u) budget_stops=%u skipped_locked=%u skipped_gate=%u "
            "slice_us=%u (mean=%u max=%u)",
            (unsigned)site, (unsigned)s_vita_theora_pump_slices,
            (unsigned)s_vita_theora_pump_decodes,
            (unsigned)(s_vita_theora_pump_decodes - decodes_before),
            (unsigned)s_vita_theora_pump_sleeps,
            (unsigned)s_vita_theora_pump_sleeps_no_work,
            (unsigned)s_vita_theora_pump_budget_stops,
            (unsigned)s_vita_theora_pump_skipped_locked,
            (unsigned)s_vita_theora_pump_skipped_gate,
            (unsigned)s_vita_theora_pump_slice_us_last,
            (unsigned)(s_vita_theora_pump_slice_us_sum /
                       s_vita_theora_pump_slices),
            (unsigned)s_vita_theora_pump_slice_us_max);
}

/* Called by vita_sync_Sleep (host_vita_sync.c) before it delays.  Returns 1
 * when the call belongs to the pumped worker loop: the slice is over, the
 * shim must return to the guest at once. */
int isaac_vita_theora_worker_pump_intercept_sleep(CPU *__restrict c,
                                                  uint32_t milliseconds)
{
    uint32_t return_address;

    if (!s_vita_theora_pump_active)
        return 0;
    return_address = guest_stack_address(c, c->esp, 4U, 0U);
    if (!return_address || c->fault)
        return 0;
    /* Translated call sites push the bare return RVA (emit.py
     * gpush_generated), not GUEST_IMAGE_BASE + RVA: the popfd5 device run
     * compared against the based form, never matched, and a slice that found
     * the frame queue full slept the real Sleep(100) of the no-work branch
     * forever (the queue can only drain on the thread the slice was
     * running on).  Both worker sleeps share this one call site: the
     * no-work branch does `push 0x64; jmp 0x5c2ea1`. */
    return_address = ld32(return_address);
    if (return_address == VITA_THEORA_WORKER_SLEEP_RETURN_RVA) {
        st8(vita_theora_worker_address() + VITA_THEORA_WORKER_RUNNING_OFFSET, 0U);
        vita_theora_count(&s_vita_theora_pump_sleeps);
        if (milliseconds >= 100U)
            vita_theora_count(&s_vita_theora_pump_sleeps_no_work);
        return 1;
    }
    if (return_address == VITA_THEORA_MUTEX_SPIN_RETURN_RVA) {
        guest_fault(c, return_address,
                    "theora worker pump: Mutex::lock re-lock spin inside a slice");
        return 1;
    }
    return 0;
}

/* TheoraVideoClip_Theora::decodeNextFrame: count the slice's decodes and stop
 * the loop at the budget.  The bool result stays in AL. */
void __wrap_sub_005aa6d0(CPU *__restrict c)
{
    __real_sub_005aa6d0(c);
    if (!s_vita_theora_pump_active)
        return;
    vita_theora_count(&s_vita_theora_pump_decodes);
    if (s_vita_theora_pump_budget_left)
        --s_vita_theora_pump_budget_left;
    if (!s_vita_theora_pump_budget_left) {
        st8(vita_theora_worker_address() + VITA_THEORA_WORKER_RUNNING_OFFSET, 0U);
        vita_theora_count(&s_vita_theora_pump_budget_stops);
    }
}

static uint32_t vita_theora_seam_site(CPU *__restrict c)
{
    uint32_t address = guest_stack_address(c, c->esp, 4U, 0U);

    return address ? ld32(address) : 0U;
}

#if defined(ISAAC_VITA_THEORA_RENDER_PRIORITY) && ISAAC_VITA_THEORA_RENDER_PRIORITY
#include "kage_vita_fullspeed_scheduler.h"
/* Exact main-loop return from test_theora_worker_pump_sites.py. */
#define VITA_THEORA_MAIN_SERVICE_RETURN_RVA 0x0048bdf5U
static int vita_theora_has_single_clip(void)
{
    uint32_t manager = ld32(GUEST_IMAGE_BASE + VITA_THEORA_MANAGER_SINGLETON_RVA);
    uint32_t begin, end, clip;
    if (!manager || (manager & 3U))
        return 0;
    begin = ld32(manager + VITA_THEORA_MANAGER_CLIPS_BEGIN);
    end = ld32(manager + VITA_THEORA_MANAGER_CLIPS_END);
    if (!begin || (begin & 3U) || end < begin || end - begin != 4U)
        return 0;
    clip = ld32(begin);
    return clip != 0U && !(clip & 3U);
}
#endif

#if defined(ISAAC_VITA_THEORA_CADENCE_PROFILE) && ISAAC_VITA_THEORA_CADENCE_PROFILE
#include "host_vita_theora_cadence_profile.h"
#include "host_vita_theora_upload_profile.h"
static VitaTheoraCadenceProfile s_vita_tc;
static uint32_t s_vita_tc_depth;
static uint32_t s_vita_tc_fetch_depth;
static uint32_t s_vita_tc_upload_info_rows;
static uint32_t s_vita_tc_layout_rows;

/* Called at the actual native glTexSubImage2D boundary, only attributed while
 * the PE-pinned video fetch is on this same owner thread's call stack. */
uint64_t isaac_vita_theora_subupload_begin(
    uint32_t width, uint32_t height, uint32_t format, uint32_t type)
{
    if (!s_vita_tc_fetch_depth || !s_vita_tc.pending || s_vita_tc.overflow)
        return UINT64_MAX;
    if (s_vita_tc_upload_info_rows < 4U) {
        ++s_vita_tc_upload_info_rows;
        isaac_vita_log("tc.upload width=%u height=%u format=%08x type=%08x",
                       (unsigned)width, (unsigned)height,
                       (unsigned)format, (unsigned)type);
    }
    return vita_theora_now_us();
}

void isaac_vita_theora_subupload_end(uint64_t begin)
{
    if (begin != UINT64_MAX)
        vita_tc_subupload_time(&s_vita_tc, begin, vita_theora_now_us());
}

int isaac_vita_theora_subupload_layout_needed(void)
{
    return s_vita_tc_fetch_depth && s_vita_tc.pending && !s_vita_tc.overflow &&
        s_vita_tc_layout_rows < 4U;
}

void isaac_vita_theora_subupload_layout(
    uint32_t format, uint32_t width, uint32_t height, uint32_t texture_type)
{
    if (!isaac_vita_theora_subupload_layout_needed())
        return;
    ++s_vita_tc_layout_rows;
    isaac_vita_log("tc.layout native_format=%08x width=%u height=%u texture_type=%08x",
                   (unsigned)format, (unsigned)width, (unsigned)height,
                   (unsigned)texture_type);
}

/* Same manager/vector pins as pump_ready, but do not call its lock/gate logic:
 * a busy or zero-work clip must still produce an interval. */
static void vita_tc_sample(VitaTheoraCadenceSample *out)
{
    uint32_t manager = ld32(GUEST_IMAGE_BASE + VITA_THEORA_MANAGER_SINGLETON_RVA);
    out->clips = 0U;
    if (manager && !(manager & 3U)) {
        uint32_t begin = ld32(manager + VITA_THEORA_MANAGER_CLIPS_BEGIN);
        uint32_t end = ld32(manager + VITA_THEORA_MANAGER_CLIPS_END);
        if (!(begin & 3U) && !(end & 3U) && end >= begin &&
                end - begin <= VITA_THEORA_MAX_CLIPS * 4U)
            out->clips = (end - begin) / 4U;
        else out->clips = UINT32_MAX;
    } else if (manager) out->clips = UINT32_MAX;
    isaac_vita_native_theora_snapshot(&out->native);
    kage_vita_fullspeed_scheduler_snapshot(&out->scheduler);
    out->scheduler_enabled =
        (uint32_t)kage_vita_fullspeed_scheduler_bypass_limiter();
    out->now_us = vita_theora_now_us();
}

/* Copied from the inspected fetch/upload/pop body; pinned in
 * test_theora_cadence_profile.py. No queue accessor calls, guest writes or
 * extra GL calls. displayed advances AFTER the upload call, before _pop(1).
 * upload_pop is an issued upload/pop path, never proof of GPU presentation. */
#define VITA_TC_VIDEO_CLIP_OFFSET 0x4U
#define VITA_TC_CLIP_DROPPED_OFFSET 0x9cU
#define VITA_TC_CLIP_DISPLAYED_OFFSET 0xa0U
void __real_sub_00599db0(CPU *__restrict c);
void __wrap_sub_00599db0(CPU *__restrict c)
{
    if (s_vita_tc.overflow) {
        __real_sub_00599db0(c);
        return;
    }
    uint32_t clip = c->fault ? 0U : ld32(c->ecx + VITA_TC_VIDEO_CLIP_OFFSET);
    uint32_t dropped = clip ? ld32(clip + VITA_TC_CLIP_DROPPED_OFFSET) : 0U;
    uint32_t displayed = clip ? ld32(clip + VITA_TC_CLIP_DISPLAYED_OFFSET) : 0U;
    uint64_t fetch_begin = vita_theora_now_us();
    ++s_vita_tc_fetch_depth;
    __real_sub_00599db0(c);
    --s_vita_tc_fetch_depth;
    vita_tc_fetch_time(&s_vita_tc, fetch_begin, vita_theora_now_us());
    vita_tc_fetch(&s_vita_tc, dropped,
        (clip && !c->fault) ? ld32(clip + VITA_TC_CLIP_DROPPED_OFFSET) : dropped,
        displayed,
        (clip && !c->fault) ? ld32(clip + VITA_TC_CLIP_DISPLAYED_OFFSET) : displayed,
        c->fault != 0);
}
#endif

/* TheoraVideoManager::update, game-inlined (a bool in CL; the frame delta is
 * computed inside from the game clock).  The first call of every main-loop
 * iteration.  It locks mWorkMutex itself, so nothing is held at entry, and
 * the slice runs before the untouched body so this frame's getNextFrame sees
 * the frames it decoded.  ECX/EDX/EAX are restored by the slice. */
void __wrap_sub_00598d60(CPU *__restrict c)
{
#if defined(ISAAC_VITA_THEORA_RENDER_PRIORITY) && ISAAC_VITA_THEORA_RENDER_PRIORITY
    /* Capture before the real function pops its return word. */
    uint32_t video_service_site = vita_theora_seam_site(c);
#endif
#if defined(ISAAC_VITA_THEORA_CADENCE_PROFILE) && ISAAC_VITA_THEORA_CADENCE_PROFILE
    VitaTheoraCadenceSample sample;
    uint64_t pump_start = 0U, pump_end = 0U;
    uint32_t outer = s_vita_tc_depth++ == 0U;
    if (outer && !s_vita_tc.overflow) {
        uint32_t site = vita_theora_seam_site(c);
        vita_tc_sample(&sample);
        vita_tc_close(&s_vita_tc, &sample, site);
        /* Exclude printing the PREVIOUS interval from this service duration. */
        sample.now_us = vita_theora_now_us();
        vita_tc_begin(&s_vita_tc, &sample, site);
        pump_start = vita_theora_now_us();
    } else if (!outer) {
        vita_tc_add(&s_vita_tc.nested, 1U, &s_vita_tc.flags);
        s_vita_tc.flags |= 16U;
    }
#endif
    if (!s_vita_theora_pump_active)
        vita_theora_pump_slice(c, vita_theora_seam_site(c));
#if defined(ISAAC_VITA_THEORA_CADENCE_PROFILE) && ISAAC_VITA_THEORA_CADENCE_PROFILE
    if (outer && !s_vita_tc.overflow) pump_end = vita_theora_now_us();
#endif
    __real_sub_00598d60(c);
#if defined(ISAAC_VITA_THEORA_RENDER_PRIORITY) && ISAAC_VITA_THEORA_RENDER_PRIORITY
    if (!c->fault && video_service_site == VITA_THEORA_MAIN_SERVICE_RETURN_RVA &&
            vita_theora_has_single_clip())
        kage_vita_fullspeed_scheduler_note_video_service();
#endif
#if defined(ISAAC_VITA_THEORA_CADENCE_PROFILE) && ISAAC_VITA_THEORA_CADENCE_PROFILE
    if (outer && !s_vita_tc.overflow) {
        vita_tc_sample(&sample);
        vita_tc_end(&s_vita_tc, &sample, pump_start, pump_end, c->fault != 0);
    }
    --s_vita_tc_depth;
#endif
}
