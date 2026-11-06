/* ISAAC_VITA_THEORA_PRECACHE_FRAMES: bound theoraplayer's video frame queue.
 *
 * TheoraFrameQueue::setSize(n) (sub_005a4c60) drops the queue and builds n
 * TheoraVideoFrame objects, each owning an RGBA w*h*4 buffer from the guest
 * heap: 1,566,720 bytes per frame for the 816x480 ending videos.  The clip
 * asks for its mNumPrecachedFrames on load (0x5a9f24 `push [esi+0x44]`) and
 * re-asks the current size on restart (0x5950c9 `push [ecx+4]`).  During a
 * run the 81 MiB guest heap is ~98 % full and the 8 MiB overflow pool admits
 * four such frames, so on 2026-09-09 the first ending cutscene on the test
 * device died in std::bad_alloc from the fifth (bad_alloc diagnostic:
 * request=1566720 owner=0x005bf75e arena=84484096 allocated=83192912
 * free=1291184).
 *
 * GNU ld --wrap=sub_005a4c60 hands every translated call here first.  The
 * count lives at [esp+4] on the guest stack and the queue's parent clip at
 * [ecx+8]; both are rewritten in place so the clip's own bookkeeping agrees
 * with the queue it got, then the untouched translated body runs.  The
 * decoder worker only ever asks the queue for a free frame and idles when
 * there is none, so a short queue costs read-ahead, not correctness.
 * Everything here is compiled only when the knob is > 0; the OFF build has no
 * object, no link option and byte-identical units. */
#include <stdint.h>

#include "guest.h"

#if !defined(ISAAC_VITA_THEORA_PRECACHE_FRAMES) || \
    ISAAC_VITA_THEORA_PRECACHE_FRAMES <= 0
#error "host_vita_theora_precache.c is compiled only with ISAAC_VITA_THEORA_PRECACHE_FRAMES > 0"
#endif

/* recomp/vita/platform.h; declared here so a host oracle can stub it. */
void isaac_vita_log(const char *format, ...);

void __real_sub_005a4c60(CPU *__restrict c);

/* Frozen PE facts, pinned by recomp/test_theora_precache_sites.py. */
#define VITA_THEORA_QUEUE_PARENT_OFFSET      0x8U   /* TheoraFrameQueue::mParent */
#define VITA_THEORA_CLIP_PRECACHED_OFFSET    0x44U  /* TheoraVideoClip::mNumPrecachedFrames */

static uint32_t s_vita_theora_precache_calls;
static uint32_t s_vita_theora_precache_clamped;

static uint32_t vita_theora_count(uint32_t *counter)
{
    if (*counter != UINT32_MAX)
        ++*counter;
    return *counter;
}

void __wrap_sub_005a4c60(CPU *__restrict c)
{
    uint32_t site_address = guest_stack_address(c, c->esp, 4U, 0U);
    uint32_t count_address = guest_stack_address(c, c->esp + 4U, 4U, 0U);

    if (!c->fault && site_address && count_address) {
        uint32_t site = ld32(site_address);
        uint32_t queue = c->ecx;
        int32_t requested = (int32_t)ld32(count_address);
        int32_t applied = requested;
        uint32_t parent = 0U;
        uint32_t parent_synced = 0U;
        uint32_t calls;

        if (requested > (int32_t)ISAAC_VITA_THEORA_PRECACHE_FRAMES) {
            applied = (int32_t)ISAAC_VITA_THEORA_PRECACHE_FRAMES;
            st32(count_address, (uint32_t)applied);
            if (queue && (queue & 3U) == 0U) {
                parent = ld32(queue + VITA_THEORA_QUEUE_PARENT_OFFSET);
                if (parent && (parent & 3U) == 0U &&
                        (int32_t)ld32(parent + VITA_THEORA_CLIP_PRECACHED_OFFSET)
                            == requested) {
                    st32(parent + VITA_THEORA_CLIP_PRECACHED_OFFSET,
                         (uint32_t)applied);
                    parent_synced = 1U;
                }
            }
            vita_theora_count(&s_vita_theora_precache_clamped);
        }
        calls = vita_theora_count(&s_vita_theora_precache_calls);
        if (calls <= 8U || (calls & 63U) == 0U)
            isaac_vita_log(
                "KAGE VITA THEORA PRECACHE: site=0x%08x queue=0x%08x "
                "requested=%d applied=%d limit=%d clip_synced=%u "
                "calls=%u clamped=%u",
                (unsigned)site, (unsigned)queue, (int)requested,
                (int)applied, (int)ISAAC_VITA_THEORA_PRECACHE_FRAMES,
                (unsigned)parent_synced, (unsigned)calls,
                (unsigned)s_vita_theora_precache_clamped);
    }
    __real_sub_005a4c60(c);
}
