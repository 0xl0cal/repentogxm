/* ISAAC_VITA_NATIVE_THEORA: theoraplayer decodes its video through the VitaSDK
 * libtheoradec compiled natively instead of the translated libtheora
 * 1.2.0alpha.
 *
 * Why.  The cutscenes are 960x544 (picture 960x540) Theora 4:2:0 at 30 frames
 * per second, converted to RGB (1,566,720 bytes a frame) by theoraplayer.  The
 * translated decoder needs about 25 ms of the main thread per packet even for
 * the mostly static ending drawings (popfd6 device run, 2026-09-10: the
 * decoder pump completed 46 decodeNextFrame calls in its first 40 s, each
 * call having to chew through 16+ packets to reach a frame the pre-drop rule
 * keeps, then hit the end of the file at ~245 s and idled), so the clip timer
 * runs ahead, theoraplayer pre-drops all but every sixteenth frame, the game
 * loop falls to 1-2 frames per second while the pump slice runs and the audio
 * that decodeNextFrame feeds arrives in bursts.  A native decoder brings a
 * packet down to a few milliseconds; the pump then fits its slice into the
 * frame budget and the timer never gets ahead.
 *
 * How.  theoraplayer (TheoraVideoClip_Theora, guest_0175.c) reaches libtheora
 * only through thirteen public entry points, all direct calls into
 * guest_0181.c/guest_0182.c and none of them address-taken (pinned by
 * recomp/test_native_theora_sites.py), so GNU ld --wrap hands every call to
 * the wrappers below, which read the cdecl arguments from the guest stack,
 * call the native function on the very same memory, put the result where the
 * translated caller expects it (eax, edx:eax, or ST(0) for th_granule_time)
 * and pop the return word.  The translated bodies stay linked but unreached.
 * Three of the four structures the two sides share have the same layout on
 * x86-32 MSVC and ARM32 EABI and are used in place (static asserts below):
 * th_comment (0x10), ogg_packet (0x20, granulepos at 0x10) and
 * th_ycbcr_buffer (3 x {int,int,int,ptr}).  th_info is converted field by
 * field because the VitaSDK toolchain uses one-byte enums (see the layout
 * note at the guest offsets).  Guest address == host address, so the native
 * decoder reads the packet bytes libogg produced in guest memory and the
 * translated YUV->RGB conversion reads the planes the native decoder hands
 * back.  The opaque th_dec_ctx / th_setup_info pointers theoraplayer
 * stores in its clip (+0x404 / +0x400) are native objects; every function
 * that receives them is wrapped, so no pointer ever crosses allocators.
 *
 * Memory.  libtheoradec's four allocator imports (malloc, calloc, realloc,
 * free) are renamed by objcopy when the archive is copied at build time
 * (CMake knob block: libtheoradec_isaac_pool.a) to the isaac_vita_theora_*
 * entry points below, so the decoder never reaches the raw newlib heap.  That
 * heap is the 81 MiB arena the guest fills to within a megabyte or two by the
 * time a run ends (nt1 device run, 2026-09-10: the ending video's headers
 * parsed, th_decode_alloc's 2.5 MiB reference frame block found no room,
 * oc_dec_init returned NULL, theoraplayer stored the NULL decoder without a
 * check and th_granule_frame read keyframe_granule_shift at +0x38 of it: data
 * abort, C2-12828-1).  A 960x544 decoder needs about 4.7 MiB (three padded
 * reference frames 2.5 MiB, dct_tokens 1.5 MiB, fragment/superblock/
 * macroblock maps, setup tables), so the first allocation of a video creates
 * an 8 MiB (else 6 MiB) USER_RW memblock with a SceClibMspace on it out of
 * the post-vitaGL window, and the last free (th_comment_clear at clip
 * destruction) gives the block back.  When no memblock can be had, or the
 * pool is full, the request falls through to the guest heap's public API
 * (arena, then overflow pool: exactly where the translated decoder's
 * _ogg_malloc landed), and every seam that receives the decoder pointer
 * answers a NULL one with TH_EFAULT / -1 instead of dereferencing it, so an
 * allocation failure ends the video (theoraplayer reads on to the end of the
 * file) instead of the process.
 *
 * TheoraVideoFrame::decode (005bf7f0, guest_0177.c, reached through the frame
 * vtable slot at 0x769284 and therefore through the dispatch table, which
 * --wrap also rewrites) is wrapped as a timed pass-through only: it reports
 * how long the translated YUV->RGB conversion takes so the next step, if any,
 * can be chosen from a number.
 *
 * Everything here is compiled only when the knob is ON; the OFF build has no
 * object, no link option, no libtheoradec and byte-identical units. */
#include <stddef.h>
#include <stdint.h>

/* ogg/os_types.h spells _ogg_malloc/_ogg_calloc/_ogg_realloc/_ogg_free as the
 * raw newlib allocator names, which the production poison
 * (isaac_vita_raw_allocator_poison.h, force-included into every runtime unit)
 * rejects even inside a macro definition.  This unit never allocates; it only
 * needs the ogg integer typedefs, so it supplies them itself and closes the
 * header's include guard.  The typedefs are the ones os_types.h picks for a
 * <stdint.h> platform. */
#define _OS_TYPES_H
typedef int16_t ogg_int16_t;
typedef uint16_t ogg_uint16_t;
typedef int32_t ogg_int32_t;
typedef uint32_t ogg_uint32_t;
typedef int64_t ogg_int64_t;
typedef uint64_t ogg_uint64_t;
#include <theora/theoradec.h>

#include <string.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "guest.h"
#include "host_vita_heap.h"
#include "kage_vita_backend.h"

#if !defined(ISAAC_VITA_NATIVE_THEORA)
#error "host_vita_native_theora.c is compiled only with ISAAC_VITA_NATIVE_THEORA"
#endif

/* Diagnostic plane digest receipts (CMake ISAAC_VITA_NATIVE_THEORA_PLANE_DIGEST,
 * default OFF): see the plane digest section below. */
#if !defined(ISAAC_VITA_NATIVE_THEORA_PLANE_DIGEST)
#define ISAAC_VITA_NATIVE_THEORA_PLANE_DIGEST 0
#endif

/* recomp/vita/platform.h; declared here so a host oracle can stub it. */
void isaac_vita_log(const char *format, ...);

void __real_sub_005bf7f0(CPU *__restrict c);  /* TheoraVideoFrame::decode */
void __real_sub_005c2910(CPU *__restrict c);  /* vorbis_synthesis (clip audio) */

/* Frozen PE facts, pinned by recomp/test_native_theora_sites.py. */
#define VITA_NT_TH_DECODE_ALLOC_RVA      0x005da480U
#define VITA_NT_TH_DECODE_CTL_RVA        0x005da4f0U
#define VITA_NT_TH_DECODE_FREE_RVA       0x005da670U
#define VITA_NT_TH_DECODE_PACKETIN_RVA   0x005da6d0U
#define VITA_NT_TH_DECODE_YCBCR_OUT_RVA  0x005db3c0U
#define VITA_NT_TH_COMMENT_CLEAR_RVA     0x005db3f0U
#define VITA_NT_TH_COMMENT_INIT_RVA      0x005db440U
#define VITA_NT_TH_INFO_CLEAR_RVA        0x005db450U
#define VITA_NT_TH_INFO_INIT_RVA         0x005db470U
#define VITA_NT_TH_DECODE_HEADERIN_RVA   0x005dbac0U
#define VITA_NT_TH_SETUP_FREE_RVA        0x005dbb10U
#define VITA_NT_TH_GRANULE_FRAME_RVA     0x005ddb60U
#define VITA_NT_TH_GRANULE_TIME_RVA      0x005ddc10U
#define VITA_NT_FRAME_DECODE_RVA         0x005bf7f0U
#define VITA_NT_VORBIS_SYNTHESIS_RVA     0x005c2910U

/* Guest th_info (x86-32 MSVC, int-sized enums), 0x40 bytes. */
#define VITA_NT_GUEST_INFO_VERSION            0x00U  /* three bytes + pad */
#define VITA_NT_GUEST_INFO_FRAME_WIDTH        0x04U
#define VITA_NT_GUEST_INFO_FRAME_HEIGHT       0x08U
#define VITA_NT_GUEST_INFO_PIC_WIDTH          0x0cU
#define VITA_NT_GUEST_INFO_PIC_HEIGHT         0x10U
#define VITA_NT_GUEST_INFO_PIC_X              0x14U
#define VITA_NT_GUEST_INFO_PIC_Y              0x18U
#define VITA_NT_GUEST_INFO_FPS_NUMERATOR      0x1cU
#define VITA_NT_GUEST_INFO_FPS_DENOMINATOR    0x20U
#define VITA_NT_GUEST_INFO_ASPECT_NUMERATOR   0x24U
#define VITA_NT_GUEST_INFO_ASPECT_DENOMINATOR 0x28U
#define VITA_NT_GUEST_INFO_COLORSPACE         0x2cU
#define VITA_NT_GUEST_INFO_PIXEL_FMT          0x30U
#define VITA_NT_GUEST_INFO_TARGET_BITRATE     0x34U
#define VITA_NT_GUEST_INFO_QUALITY            0x38U
#define VITA_NT_GUEST_INFO_KEYFRAME_SHIFT     0x3cU
#define VITA_NT_GUEST_INFO_SIZE               0x40U

/* The guest side is x86-32 MSVC: long and pointers are 4 bytes, ogg_int64_t
 * is 8-aligned, enums are int.  The native side agrees for th_comment,
 * ogg_packet and th_ycbcr_buffer (no enums in them), so those are shared in
 * place.  th_info is the exception: the VitaSDK toolchain uses AAPCS "small"
 * enums (Tag_ABI_enum_size: small on libtheoradec.a), so th_colorspace and
 * th_pixel_fmt are one byte each and the native th_info is 0x3c bytes against
 * the guest's 0x40 (th_info_clear memsets 0x40; keyframe_granule_shift sits
 * at +0x3c, th_info_init stores 6 there).  Every seam that takes a th_info
 * therefore converts field by field between the guest layout above and a
 * native stack copy; libtheora never keeps the th_info pointer it is given
 * (th_decode_alloc copies it into the context), so a stack copy is complete. */
_Static_assert(sizeof(void *) == 4, "native theora seam needs 32-bit pointers");
_Static_assert(sizeof(long) == 4, "native theora seam needs 32-bit long");
_Static_assert(sizeof(th_info) <= VITA_NT_GUEST_INFO_SIZE,
               "native th_info larger than the guest's");
_Static_assert(sizeof(th_comment) == 0x10, "th_comment layout drifted");
_Static_assert(sizeof(ogg_packet) == 0x20, "ogg_packet layout drifted");
_Static_assert(offsetof(ogg_packet, bytes) == 0x4, "ogg_packet.bytes");
_Static_assert(offsetof(ogg_packet, granulepos) == 0x10, "ogg_packet.granulepos");
_Static_assert(offsetof(ogg_packet, packetno) == 0x18, "ogg_packet.packetno");
_Static_assert(sizeof(th_img_plane) == 0x10, "th_img_plane layout drifted");
_Static_assert(offsetof(th_img_plane, data) == 0xc, "th_img_plane.data");
_Static_assert(sizeof(th_ycbcr_buffer) == 0x30, "th_ycbcr_buffer layout drifted");
_Static_assert(sizeof(ogg_int64_t) == 8, "ogg_int64_t");

static struct {
    uint32_t info_inits, info_clears, comment_inits, comment_clears;
    uint32_t headers, header_errors, allocs, alloc_failures, frees, setup_frees;
    uint32_t ctls, granule_frames, granule_times;
    uint32_t packets, dup_frames, packet_errors;
    uint32_t ycbcr_outs, ycbcr_errors, rgb_decodes;
    uint32_t packetin_us_last, packetin_us_max;
    uint32_t ycbcr_us_last, ycbcr_us_max;
    uint32_t rgb_us_last, rgb_us_max;
    uint64_t packetin_us_sum, ycbcr_us_sum, rgb_us_sum;
    uint32_t vorbis_syntheses, vorbis_us_last, vorbis_us_max;
    uint64_t vorbis_us_sum;
    uint32_t live_ctx;
    uint32_t null_ctx;  /* seams handed a NULL decoder after a failed alloc */
} s_nt;

#if defined(ISAAC_VITA_THEORA_CADENCE_PROFILE) && ISAAC_VITA_THEORA_CADENCE_PROFILE
#include "host_vita_native_theora_snapshot.h"
void isaac_vita_native_theora_snapshot(VitaTheoraNativeSnapshot *out)
{
    out->packets = s_nt.packets;
    out->rgb_decodes = s_nt.rgb_decodes;
    out->vorbis_syntheses = s_nt.vorbis_syntheses;
    out->dup_frames = s_nt.dup_frames;
    out->packet_errors = s_nt.packet_errors;
    out->packetin_us_sum = s_nt.packetin_us_sum;
    out->rgb_us_sum = s_nt.rgb_us_sum;
    out->vorbis_us_sum = s_nt.vorbis_us_sum;
}
#endif

static void vita_nt_count(uint32_t *counter)
{
    if (*counter != UINT32_MAX)
        ++*counter;
}

static uint64_t vita_nt_now_us(void)
{
    double seconds = 0.0;

    if (!kage_vita_backend_time_seconds(&seconds) || seconds < 0.0)
        return 0U;
    return (uint64_t)(seconds * 1000000.0);
}

static void vita_nt_time(uint64_t t0, uint32_t *last, uint32_t *max,
                         uint64_t *sum)
{
    uint64_t t1 = vita_nt_now_us();
    uint32_t dt = (t0 && t1 > t0) ? (uint32_t)(t1 - t0) : 0U;

    *last = dt;
    if (dt > *max)
        *max = dt;
    *sum += dt;
}

static uint32_t vita_nt_mean(uint64_t sum, uint32_t count)
{
    return count ? (uint32_t)(sum / count) : 0U;
}

/* cdecl argument `index` (0-based) of the wrapped call: [esp + 4 + 4*index]
 * with the return word still on the stack.  A stack violation is recorded by
 * guest_stack_address; the caller checks c->fault before using the value. */
static uint32_t vita_nt_arg(CPU *__restrict c, uint32_t index, uint32_t rva)
{
    uint32_t address = guest_stack_address(c, c->esp + 4U + 4U * index, 4U, rva);

    if (!address) {
        if (!c->fault)
            guest_fault(c, GUEST_IMAGE_BASE + rva,
                        "native theora: argument off the guest stack");
        return 0U;
    }
    return ld32(address);
}

static ogg_int64_t vita_nt_arg64(CPU *__restrict c, uint32_t index, uint32_t rva)
{
    uint32_t lo = vita_nt_arg(c, index, rva);
    uint32_t hi = vita_nt_arg(c, index + 1U, rva);

    return (ogg_int64_t)(((uint64_t)hi << 32) | lo);
}

/* Guest th_info -> native th_info, field by field (see the layout note). */
static void vita_nt_info_load(uint32_t guest, th_info *native)
{
    th_info_init(native);
    native->version_major = ld8(guest + VITA_NT_GUEST_INFO_VERSION);
    native->version_minor = ld8(guest + VITA_NT_GUEST_INFO_VERSION + 1U);
    native->version_subminor = ld8(guest + VITA_NT_GUEST_INFO_VERSION + 2U);
    native->frame_width = ld32(guest + VITA_NT_GUEST_INFO_FRAME_WIDTH);
    native->frame_height = ld32(guest + VITA_NT_GUEST_INFO_FRAME_HEIGHT);
    native->pic_width = ld32(guest + VITA_NT_GUEST_INFO_PIC_WIDTH);
    native->pic_height = ld32(guest + VITA_NT_GUEST_INFO_PIC_HEIGHT);
    native->pic_x = ld32(guest + VITA_NT_GUEST_INFO_PIC_X);
    native->pic_y = ld32(guest + VITA_NT_GUEST_INFO_PIC_Y);
    native->fps_numerator = ld32(guest + VITA_NT_GUEST_INFO_FPS_NUMERATOR);
    native->fps_denominator = ld32(guest + VITA_NT_GUEST_INFO_FPS_DENOMINATOR);
    native->aspect_numerator = ld32(guest + VITA_NT_GUEST_INFO_ASPECT_NUMERATOR);
    native->aspect_denominator =
        ld32(guest + VITA_NT_GUEST_INFO_ASPECT_DENOMINATOR);
    native->colorspace =
        (th_colorspace)ld32(guest + VITA_NT_GUEST_INFO_COLORSPACE);
    native->pixel_fmt = (th_pixel_fmt)ld32(guest + VITA_NT_GUEST_INFO_PIXEL_FMT);
    native->target_bitrate =
        (int)ld32(guest + VITA_NT_GUEST_INFO_TARGET_BITRATE);
    native->quality = (int)ld32(guest + VITA_NT_GUEST_INFO_QUALITY);
    native->keyframe_granule_shift =
        (int)ld32(guest + VITA_NT_GUEST_INFO_KEYFRAME_SHIFT);
}

/* Native th_info -> guest th_info. */
static void vita_nt_info_store(uint32_t guest, const th_info *native)
{
    st8(guest + VITA_NT_GUEST_INFO_VERSION, native->version_major);
    st8(guest + VITA_NT_GUEST_INFO_VERSION + 1U, native->version_minor);
    st8(guest + VITA_NT_GUEST_INFO_VERSION + 2U, native->version_subminor);
    st8(guest + VITA_NT_GUEST_INFO_VERSION + 3U, 0U);
    st32(guest + VITA_NT_GUEST_INFO_FRAME_WIDTH, native->frame_width);
    st32(guest + VITA_NT_GUEST_INFO_FRAME_HEIGHT, native->frame_height);
    st32(guest + VITA_NT_GUEST_INFO_PIC_WIDTH, native->pic_width);
    st32(guest + VITA_NT_GUEST_INFO_PIC_HEIGHT, native->pic_height);
    st32(guest + VITA_NT_GUEST_INFO_PIC_X, native->pic_x);
    st32(guest + VITA_NT_GUEST_INFO_PIC_Y, native->pic_y);
    st32(guest + VITA_NT_GUEST_INFO_FPS_NUMERATOR, native->fps_numerator);
    st32(guest + VITA_NT_GUEST_INFO_FPS_DENOMINATOR, native->fps_denominator);
    st32(guest + VITA_NT_GUEST_INFO_ASPECT_NUMERATOR, native->aspect_numerator);
    st32(guest + VITA_NT_GUEST_INFO_ASPECT_DENOMINATOR,
         native->aspect_denominator);
    st32(guest + VITA_NT_GUEST_INFO_COLORSPACE, (uint32_t)native->colorspace);
    st32(guest + VITA_NT_GUEST_INFO_PIXEL_FMT, (uint32_t)native->pixel_fmt);
    st32(guest + VITA_NT_GUEST_INFO_TARGET_BITRATE,
         (uint32_t)native->target_bitrate);
    st32(guest + VITA_NT_GUEST_INFO_QUALITY, (uint32_t)native->quality);
    st32(guest + VITA_NT_GUEST_INFO_KEYFRAME_SHIFT,
         (uint32_t)native->keyframe_granule_shift);
}

static void vita_nt_guest_zero(uint32_t guest, uint32_t size)
{
    uint32_t i;

    for (i = 0U; i < size; i += 4U)
        st32(guest + i, 0U);
}

static void vita_nt_receipt(const char *why)
{
    isaac_vita_log(
        "KAGE VITA NATIVE THEORA: %s ctx=0x%08x packets=%u (dup=%u err=%u) "
        "packetin_us last=%u mean=%u max=%u ycbcr=%u (err=%u) ycbcr_us last=%u "
        "mean=%u max=%u rgb=%u rgb_us last=%u mean=%u max=%u "
        "granule(frame,time)=%u,%u ctl=%u headers=%u (err=%u) "
        "alloc/free=%u/%u (fail=%u) setup_free=%u info(init,clear)=%u,%u "
        "comment(init,clear)=%u,%u null_ctx=%u vorbis=%u vorbis_us last=%u "
        "mean=%u max=%u sum=%u",
        why, (unsigned)s_nt.live_ctx, (unsigned)s_nt.packets,
        (unsigned)s_nt.dup_frames, (unsigned)s_nt.packet_errors,
        (unsigned)s_nt.packetin_us_last,
        (unsigned)vita_nt_mean(s_nt.packetin_us_sum, s_nt.packets),
        (unsigned)s_nt.packetin_us_max,
        (unsigned)s_nt.ycbcr_outs, (unsigned)s_nt.ycbcr_errors,
        (unsigned)s_nt.ycbcr_us_last,
        (unsigned)vita_nt_mean(s_nt.ycbcr_us_sum, s_nt.ycbcr_outs),
        (unsigned)s_nt.ycbcr_us_max,
        (unsigned)s_nt.rgb_decodes, (unsigned)s_nt.rgb_us_last,
        (unsigned)vita_nt_mean(s_nt.rgb_us_sum, s_nt.rgb_decodes),
        (unsigned)s_nt.rgb_us_max,
        (unsigned)s_nt.granule_frames, (unsigned)s_nt.granule_times,
        (unsigned)s_nt.ctls, (unsigned)s_nt.headers,
        (unsigned)s_nt.header_errors, (unsigned)s_nt.allocs,
        (unsigned)s_nt.frees, (unsigned)s_nt.alloc_failures,
        (unsigned)s_nt.setup_frees, (unsigned)s_nt.info_inits,
        (unsigned)s_nt.info_clears, (unsigned)s_nt.comment_inits,
        (unsigned)s_nt.comment_clears, (unsigned)s_nt.null_ctx,
        (unsigned)s_nt.vorbis_syntheses, (unsigned)s_nt.vorbis_us_last,
        (unsigned)vita_nt_mean(s_nt.vorbis_us_sum, s_nt.vorbis_syntheses),
        (unsigned)s_nt.vorbis_us_max,
        (unsigned)(s_nt.vorbis_us_sum / 1000U));
}

/* A seam handed a NULL decoder (th_decode_alloc failed earlier and
 * theoraplayer stores the result unchecked) answers the libtheora error code
 * instead of letting the native body dereference it. */
static int vita_nt_null_ctx(const char *what)
{
    vita_nt_count(&s_nt.null_ctx);
    if (s_nt.null_ctx <= 4U)
        isaac_vita_log(
            "KAGE VITA NATIVE THEORA: %s with a NULL decoder (th_decode_alloc "
            "failed earlier) answers TH_EFAULT instead of dereferencing it",
            what);
    return TH_EFAULT;
}

static void vita_nt_log_info(const char *why, const th_info *info,
                             uint32_t setup, int result)
{
    isaac_vita_log(
        "KAGE VITA NATIVE THEORA: %s result=%d version=%u.%u.%u frame=%ux%u "
        "pic=%ux%u+%u,%u fps=%u/%u aspect=%u/%u fmt=%d colorspace=%d "
        "shift=%d setup=0x%08x headers=%u",
        why, result, (unsigned)info->version_major,
        (unsigned)info->version_minor, (unsigned)info->version_subminor,
        (unsigned)info->frame_width, (unsigned)info->frame_height,
        (unsigned)info->pic_width, (unsigned)info->pic_height,
        (unsigned)info->pic_x, (unsigned)info->pic_y,
        (unsigned)info->fps_numerator, (unsigned)info->fps_denominator,
        (unsigned)info->aspect_numerator, (unsigned)info->aspect_denominator,
        (int)info->pixel_fmt, (int)info->colorspace,
        (int)info->keyframe_granule_shift, (unsigned)setup,
        (unsigned)s_nt.headers);
}

/* ------------------------------------------------- libtheoradec memory ---- */

/* libtheoradec's malloc/calloc/realloc/free are renamed to the four
 * isaac_vita_theora_* entry points below by objcopy when the archive is copied
 * at build time (CMake knob block), so the decoder never touches the raw
 * newlib heap.  Requests go to a private SceClibMspace on a USER_RW memblock
 * created at the first allocation of a video (8 MiB, else 6 MiB) and released
 * when the last allocation is freed; a request the pool cannot serve, or a
 * video that got no memblock, falls through to the guest heap's public API,
 * which is arena first and overflow pool second, exactly as the translated
 * decoder's _ogg_malloc did.  A free of a pointer neither side owns is counted
 * and dropped, never handed to a raw allocator.  The decoder worker runs on
 * its own thread when the pump is off, so the bookkeeping is under a spin
 * lock; the receipts are written outside it. */
#define VITA_NT_POOL_BYTES_FIRST  0x00800000U
#define VITA_NT_POOL_BYTES_SECOND 0x00600000U

static struct {
    volatile int lock;
    int attempted;             /* a memblock was tried in this live cycle */
    SceUID uid;
    uintptr_t base, end;
    SceClibMspace mspace;      /* NULL while no pool exists */
    uint32_t bytes;            /* pool size, 0 when absent */
    uint32_t live, live_pool, live_guest, peak_live;
    uint32_t cycle_bytes, peak_cycle_bytes;  /* requested since live was 0 */
    uint32_t pool_allocs, pool_failures, guest_allocs, guest_failures;
    uint32_t creates, create_failures, destroys, foreign_frees, reallocs;
    int32_t last_create_rc;
    uint32_t free_user_before, free_user_after;
} s_pool;

static void vita_nt_pool_lock(void)
{
    while (__sync_lock_test_and_set(&s_pool.lock, 1))
        sceKernelDelayThread(10U);
}

static void vita_nt_pool_unlock(void)
{
    __sync_lock_release(&s_pool.lock);
}

static uint32_t vita_nt_free_user(void)
{
    SceKernelFreeMemorySizeInfo info;

    memset(&info, 0, sizeof info);
    info.size = (int)sizeof info;
    if (sceKernelGetFreeMemorySize(&info) < 0)
        return 0U;
    return (uint32_t)info.size_user;
}

static int vita_nt_pool_owns_locked(const void *pointer)
{
    return s_pool.mspace != NULL && (uintptr_t)pointer >= s_pool.base &&
           (uintptr_t)pointer < s_pool.end;
}

static void vita_nt_pool_receipt(const char *why)
{
    isaac_vita_log(
        "KAGE VITA NATIVE THEORA POOL: %s bytes=%u live=%u (pool=%u guest=%u "
        "peak=%u) cycle_bytes=%u (peak=%u) allocs pool=%u guest=%u failures "
        "pool=%u guest=%u reallocs=%u foreign_frees=%u creates=%u (fail=%u "
        "rc=0x%08x) destroys=%u free_user before=%u after=%u now=%u "
        "guest_terminal=%d",
        why, (unsigned)s_pool.bytes, (unsigned)s_pool.live,
        (unsigned)s_pool.live_pool, (unsigned)s_pool.live_guest,
        (unsigned)s_pool.peak_live, (unsigned)s_pool.cycle_bytes,
        (unsigned)s_pool.peak_cycle_bytes, (unsigned)s_pool.pool_allocs,
        (unsigned)s_pool.guest_allocs, (unsigned)s_pool.pool_failures,
        (unsigned)s_pool.guest_failures, (unsigned)s_pool.reallocs,
        (unsigned)s_pool.foreign_frees, (unsigned)s_pool.creates,
        (unsigned)s_pool.create_failures, (unsigned)s_pool.last_create_rc,
        (unsigned)s_pool.destroys, (unsigned)s_pool.free_user_before,
        (unsigned)s_pool.free_user_after, (unsigned)vita_nt_free_user(),
        isaac_vita_guest_heap_terminal());
}

static int vita_nt_pool_create_locked(void)
{
    static const uint32_t sizes[2] = {
        VITA_NT_POOL_BYTES_FIRST, VITA_NT_POOL_BYTES_SECOND
    };
    uint32_t before = vita_nt_free_user();
    uint32_t i;

    for (i = 0U; i < 2U; ++i) {
        void *base = NULL;
        SceClibMspace mspace;
        SceUID uid = sceKernelAllocMemBlock(
            "isaac_theora_pool", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
            (SceSize)sizes[i], NULL);

        if (uid < 0) {
            s_pool.last_create_rc = (int32_t)uid;
            continue;
        }
        if (sceKernelGetMemBlockBase(uid, &base) < 0 || !base) {
            (void)sceKernelFreeMemBlock(uid);
            s_pool.last_create_rc = -1;
            continue;
        }
        mspace = sceClibMspaceCreate(base, (SceSize)sizes[i]);
        if (!mspace) {
            (void)sceKernelFreeMemBlock(uid);
            s_pool.last_create_rc = -2;
            continue;
        }
        s_pool.uid = uid;
        s_pool.base = (uintptr_t)base;
        s_pool.end = s_pool.base + sizes[i];
        s_pool.mspace = mspace;
        s_pool.bytes = sizes[i];
        s_pool.free_user_before = before;
        s_pool.free_user_after = vita_nt_free_user();
        vita_nt_count(&s_pool.creates);
        return 1;
    }
    s_pool.free_user_before = before;
    s_pool.free_user_after = before;
    vita_nt_count(&s_pool.create_failures);
    return 0;
}

static void vita_nt_pool_destroy_locked(void)
{
    if (!s_pool.mspace)
        return;
    sceClibMspaceDestroy(s_pool.mspace);
    (void)sceKernelFreeMemBlock(s_pool.uid);
    s_pool.mspace = NULL;
    s_pool.uid = -1;
    s_pool.base = 0U;
    s_pool.end = 0U;
    s_pool.bytes = 0U;
    vita_nt_count(&s_pool.destroys);
}

static void vita_nt_pool_note_alloc_locked(size_t size)
{
    ++s_pool.live;
    if (s_pool.live > s_pool.peak_live)
        s_pool.peak_live = s_pool.live;
    s_pool.cycle_bytes += (uint32_t)size;
    if (s_pool.cycle_bytes > s_pool.peak_cycle_bytes)
        s_pool.peak_cycle_bytes = s_pool.cycle_bytes;
}

/* Returns 1 when the last live allocation went away (pool released). */
static int vita_nt_pool_note_free_locked(void)
{
    if (s_pool.live)
        --s_pool.live;
    if (s_pool.live)
        return 0;
    vita_nt_pool_destroy_locked();
    s_pool.attempted = 0;
    s_pool.cycle_bytes = 0U;
    return 1;
}

/* *pool_event becomes 1 when this call created (or failed to create) the
 * pool, so the caller can write the receipt after unlocking. */
static void *vita_nt_alloc_locked(size_t size, int zero, int *pool_event)
{
    void *result = NULL;
    size_t backing = size ? size : 1U;

    *pool_event = 0;
    if (!s_pool.mspace && !s_pool.attempted) {
        s_pool.attempted = 1;
        (void)vita_nt_pool_create_locked();
        *pool_event = 1;
    }
    if (s_pool.mspace) {
        result = zero ? sceClibMspaceCalloc(s_pool.mspace, 1U, (SceSize)backing)
                      : sceClibMspaceMalloc(s_pool.mspace, (SceSize)backing);
        if (result) {
            ++s_pool.live_pool;
            vita_nt_count(&s_pool.pool_allocs);
        }
        else
            vita_nt_count(&s_pool.pool_failures);
    }
    if (!result) {
        result = zero ? isaac_vita_guest_calloc(1U, backing)
                      : isaac_vita_guest_malloc(backing);
        if (result) {
            ++s_pool.live_guest;
            vita_nt_count(&s_pool.guest_allocs);
        }
        else
            vita_nt_count(&s_pool.guest_failures);
    }
    if (result)
        vita_nt_pool_note_alloc_locked(backing);
    return result;
}

/* Returns 1 when the pointer was ours and is now freed, 2 when that free
 * released the pool, 0 for a pointer neither side owns. */
static int vita_nt_free_locked(void *pointer)
{
    if (vita_nt_pool_owns_locked(pointer)) {
        sceClibMspaceFree(s_pool.mspace, pointer);
        if (s_pool.live_pool)
            --s_pool.live_pool;
    }
    else if (isaac_vita_guest_free(pointer)) {
        if (s_pool.live_guest)
            --s_pool.live_guest;
    }
    else {
        vita_nt_count(&s_pool.foreign_frees);
        return 0;
    }
    return vita_nt_pool_note_free_locked() ? 2 : 1;
}

void *isaac_vita_theora_malloc(size_t size)
{
    void *result;
    int pool_event;

    vita_nt_pool_lock();
    result = vita_nt_alloc_locked(size, 0, &pool_event);
    vita_nt_pool_unlock();
    if (pool_event)
        vita_nt_pool_receipt(s_pool.mspace ? "create" : "create FAILED");
    return result;
}

void *isaac_vita_theora_calloc(size_t count, size_t size)
{
    void *result;
    int pool_event;

    if (count && size > SIZE_MAX / count)
        return NULL;
    vita_nt_pool_lock();
    result = vita_nt_alloc_locked(count * size, 1, &pool_event);
    vita_nt_pool_unlock();
    if (pool_event)
        vita_nt_pool_receipt(s_pool.mspace ? "create" : "create FAILED");
    return result;
}

void isaac_vita_theora_free(void *pointer)
{
    int released;

    if (!pointer)
        return;
    vita_nt_pool_lock();
    released = vita_nt_free_locked(pointer) == 2;
    vita_nt_pool_unlock();
    if (released)
        vita_nt_pool_receipt("idle");
}

void *isaac_vita_theora_realloc(void *pointer, size_t size)
{
    void *result = NULL;

    if (!pointer)
        return isaac_vita_theora_malloc(size);
    if (!size) {
        isaac_vita_theora_free(pointer);
        return NULL;
    }
    vita_nt_pool_lock();
    vita_nt_count(&s_pool.reallocs);
    if (vita_nt_pool_owns_locked(pointer)) {
        result = sceClibMspaceRealloc(s_pool.mspace, pointer, (SceSize)size);
        if (!result) {
            /* The pool is full: move the block to the guest heap. */
            size_t old = (size_t)sceClibMspaceMallocUsableSize(pointer);

            result = isaac_vita_guest_malloc(size);
            if (result) {
                memcpy(result, pointer, old < size ? old : size);
                sceClibMspaceFree(s_pool.mspace, pointer);
                if (s_pool.live_pool)
                    --s_pool.live_pool;
                ++s_pool.live_guest;
                vita_nt_count(&s_pool.guest_allocs);
            }
            else
                vita_nt_count(&s_pool.guest_failures);
        }
    }
    else {
        int valid_owner = 0;

        result = isaac_vita_guest_realloc(pointer, size, &valid_owner);
        if (!valid_owner)
            vita_nt_count(&s_pool.foreign_frees);
    }
    if (result) {
        s_pool.cycle_bytes += (uint32_t)size;
        if (s_pool.cycle_bytes > s_pool.peak_cycle_bytes)
            s_pool.peak_cycle_bytes = s_pool.cycle_bytes;
    }
    vita_nt_pool_unlock();
    return result;
}

/* ------------------------------------------------------------- th_info ---- */

void __wrap_sub_005db470(CPU *__restrict c)  /* th_info_init(th_info *) */
{
    uint32_t info = vita_nt_arg(c, 0U, VITA_NT_TH_INFO_INIT_RVA);
    th_info native;

    if (c->fault)
        return;
    th_info_init(&native);
    vita_nt_guest_zero(info, VITA_NT_GUEST_INFO_SIZE);
    vita_nt_info_store(info, &native);
    vita_nt_count(&s_nt.info_inits);
    (void)gpop(c);
}

void __wrap_sub_005db450(CPU *__restrict c)  /* th_info_clear(th_info *) */
{
    uint32_t info = vita_nt_arg(c, 0U, VITA_NT_TH_INFO_CLEAR_RVA);

    if (c->fault)
        return;
    /* th_info_clear is a memset of the (guest-sized) structure. */
    vita_nt_guest_zero(info, VITA_NT_GUEST_INFO_SIZE);
    vita_nt_count(&s_nt.info_clears);
    (void)gpop(c);
}

/* ---------------------------------------------------------- th_comment ---- */

void __wrap_sub_005db440(CPU *__restrict c)  /* th_comment_init(th_comment *) */
{
    uint32_t tc = vita_nt_arg(c, 0U, VITA_NT_TH_COMMENT_INIT_RVA);

    if (c->fault)
        return;
    th_comment_init((th_comment *)(uintptr_t)tc);
    vita_nt_count(&s_nt.comment_inits);
    (void)gpop(c);
}

void __wrap_sub_005db3f0(CPU *__restrict c)  /* th_comment_clear(th_comment *) */
{
    uint32_t tc = vita_nt_arg(c, 0U, VITA_NT_TH_COMMENT_CLEAR_RVA);

    if (c->fault)
        return;
    /* The translated body tolerates NULL (`test edi, edi; je`); so do we. */
    if (tc)
        th_comment_clear((th_comment *)(uintptr_t)tc);
    vita_nt_count(&s_nt.comment_clears);
    (void)gpop(c);
}

/* ------------------------------------------------------------ headers ---- */

/* th_decode_headerin(th_info *, th_comment *, th_setup_info **, ogg_packet *) */
void __wrap_sub_005dbac0(CPU *__restrict c)
{
    uint32_t info = vita_nt_arg(c, 0U, VITA_NT_TH_DECODE_HEADERIN_RVA);
    uint32_t tc = vita_nt_arg(c, 1U, VITA_NT_TH_DECODE_HEADERIN_RVA);
    uint32_t setup = vita_nt_arg(c, 2U, VITA_NT_TH_DECODE_HEADERIN_RVA);
    uint32_t op = vita_nt_arg(c, 3U, VITA_NT_TH_DECODE_HEADERIN_RVA);
    th_info native;
    int result;

    if (c->fault)
        return;
    if (!info) {
        /* The translated body answers TH_EFAULT without touching anything. */
        vita_nt_count(&s_nt.headers);
        vita_nt_count(&s_nt.header_errors);
        c->eax = (uint32_t)TH_EFAULT;
        (void)gpop(c);
        return;
    }
    vita_nt_info_load(info, &native);
    result = th_decode_headerin(&native, (th_comment *)(uintptr_t)tc,
                                (th_setup_info **)(uintptr_t)setup,
                                (ogg_packet *)(uintptr_t)op);
    vita_nt_info_store(info, &native);
    vita_nt_count(&s_nt.headers);
    if (result < 0) {
        vita_nt_count(&s_nt.header_errors);
        if (s_nt.header_errors <= 4U)
            isaac_vita_log(
                "KAGE VITA NATIVE THEORA: header result=%d info=0x%08x tc=0x%08x "
                "setup=0x%08x op=0x%08x headers=%u",
                result, (unsigned)info, (unsigned)tc, (unsigned)setup,
                (unsigned)op, (unsigned)s_nt.headers);
    } else if (result > 0) {
        /* Positive: the first video packet, all three headers are in. */
        vita_nt_log_info("headers complete", &native,
                         setup ? ld32(setup) : 0U, result);
    }
    c->eax = (uint32_t)result;
    (void)gpop(c);
}

void __wrap_sub_005dbb10(CPU *__restrict c)  /* th_setup_free(th_setup_info *) */
{
    uint32_t setup = vita_nt_arg(c, 0U, VITA_NT_TH_SETUP_FREE_RVA);

    if (c->fault)
        return;
    th_setup_free((th_setup_info *)(uintptr_t)setup);  /* NULL-tolerant */
    vita_nt_count(&s_nt.setup_frees);
    (void)gpop(c);
}

#if ISAAC_VITA_NATIVE_THEORA_PLANE_DIGEST
/* ------------------------------------------------------- plane digest ---- */

/* Diagnostic (ISAAC_VITA_NATIVE_THEORA_PLANE_DIGEST=1).  After every
 * successful th_decode_packetin (0 or TH_DUPFRAME) the decoded planes are
 * read back through th_decode_ycbcr_out, a pointer flip, and one CRC32 per
 * plane over exactly width x height bytes, row by row through the stride, is
 * logged with the packet ordinal since the last th_decode_alloc, the packetin
 * result and the granule position the decoder wrote (-1 when theoraplayer
 * passed no granpos pointer).  th_decode_packetin runs for every packet in
 * stream order whatever theoraplayer later keeps, so the stream is
 * reproducible run to run and keyed by the ordinal.  The key assumes linear
 * playback: a theoraplayer seek (doSeek re-allocs the decoder and restarts
 * from a keyframe) restarts the ordinal mid-clip, and the compare tool then
 * sees a new run whose packets stand for later frames; the game's cutscenes
 * never seek, so the ordinal stays; a seeking clip would need op->packetno
 * in the line.
 * recomp/vita/theora_plane_digest.c prints the same lines on the host from
 * the same file (its two crc functions are textually identical to the ones
 * below, pinned by recomp/test_native_theora_sites.py) and
 * recomp/vita/theora_plane_digest_compare.py diffs the two streams, which is
 * how a replacement archive (libtheora 1.2.0 with the ARM/NEON assembly) is
 * proven bit-exact packet by packet.  CRC32 is IEEE 802.3 reflected:
 * polynomial 0xEDB88320, initial ~0, final ~x, the 256-entry table built on
 * first use in a static array, so nothing here allocates.  Cost: 783,360
 * bytes per 960x544 frame through a byte-serial table CRC, some 4-6 cycles
 * a byte on the Cortex-A9, i.e. roughly 7-11 ms per frame at 444 MHz
 * (unmeasured), outside the packetin_us timing: with the knob ON the frame
 * timing receipts are not representative. */
static uint32_t s_nt_digest_table[256];
static int s_nt_digest_table_ready;
static uint32_t s_nt_digest_packet;   /* packetin ordinal since the last alloc */

static void plane_digest_crc32_init(uint32_t *table)
{
    uint32_t n;

    for (n = 0U; n < 256U; ++n) {
        uint32_t c = n;
        int k;

        for (k = 0; k < 8; ++k)
            c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
        table[n] = c;
    }
}

/* CRC32 of `height` rows of `width` bytes each, rows `stride` bytes apart;
 * the padding beyond `width` is not part of the digest.  0 for an empty
 * plane (the CRC32 of no bytes). */
static uint32_t plane_digest_crc32(const uint32_t *table,
                                   const unsigned char *data,
                                   int width, int height, int stride)
{
    uint32_t crc = 0xFFFFFFFFU;
    int y;

    if (!data || width <= 0 || height <= 0)
        return 0U;
    for (y = 0; y < height; ++y) {
        const unsigned char *row = data + (ptrdiff_t)y * stride;
        int x;

        for (x = 0; x < width; ++x)
            crc = table[(crc ^ row[x]) & 0xFFU] ^ (crc >> 8);
    }
    return ~crc;
}

static void vita_nt_digest(uint32_t dec, int result, uint32_t granpos)
{
    th_ycbcr_buffer planes;
    uint32_t packet = s_nt_digest_packet;
    ogg_int64_t gp = -1;
    int out;

    if (s_nt_digest_packet != UINT32_MAX)
        ++s_nt_digest_packet;
    if (!dec || (result != 0 && result != TH_DUPFRAME))
        return;
    if (!s_nt_digest_table_ready) {
        plane_digest_crc32_init(s_nt_digest_table);
        s_nt_digest_table_ready = 1;
    }
    out = th_decode_ycbcr_out((th_dec_ctx *)(uintptr_t)dec, planes);
    if (out != 0) {
        isaac_vita_log(
            "KAGE VITA NATIVE THEORA DIGEST: packet=%u ret=%d ycbcr_out=%d "
            "(no digest)", (unsigned)packet, result, out);
        return;
    }
    if (granpos)
        gp = (ogg_int64_t)(((uint64_t)ld32(granpos + 4U) << 32) | ld32(granpos));
    isaac_vita_log(
        "KAGE VITA NATIVE THEORA DIGEST: packet=%u ret=%d gp=%lld y=%08x cb=%08x "
        "cr=%08x",
        (unsigned)packet, result, (long long)gp,
        (unsigned)plane_digest_crc32(s_nt_digest_table, planes[0].data,
                                     planes[0].width, planes[0].height,
                                     planes[0].stride),
        (unsigned)plane_digest_crc32(s_nt_digest_table, planes[1].data,
                                     planes[1].width, planes[1].height,
                                     planes[1].stride),
        (unsigned)plane_digest_crc32(s_nt_digest_table, planes[2].data,
                                     planes[2].width, planes[2].height,
                                     planes[2].stride));
}
#endif

/* ------------------------------------------------------------ decoder ---- */

/* th_dec_ctx *th_decode_alloc(const th_info *, const th_setup_info *) */
void __wrap_sub_005da480(CPU *__restrict c)
{
    uint32_t info = vita_nt_arg(c, 0U, VITA_NT_TH_DECODE_ALLOC_RVA);
    uint32_t setup = vita_nt_arg(c, 1U, VITA_NT_TH_DECODE_ALLOC_RVA);
    th_info native;
    th_dec_ctx *ctx = NULL;

    if (c->fault)
        return;
    if (info && setup) {
        vita_nt_info_load(info, &native);
        ctx = th_decode_alloc(&native, (const th_setup_info *)(uintptr_t)setup);
    }
    vita_nt_count(&s_nt.allocs);
    if (!ctx)
        vita_nt_count(&s_nt.alloc_failures);
    s_nt.live_ctx = (uint32_t)(uintptr_t)ctx;
#if ISAAC_VITA_NATIVE_THEORA_PLANE_DIGEST
    s_nt_digest_packet = 0U;
#endif
    if (info && setup) {
        vita_nt_log_info(ctx ? "alloc" : "alloc FAILED", &native, setup,
                         ctx ? 1 : 0);
        /* The archive actually linked; the guest's own th_version_string is
         * not wrapped and keeps answering "1.2.0alpha".  The native th_info
         * size (0x3c with the toolchain's short enums) is the ABI receipt
         * for a replacement archive. */
        isaac_vita_log("KAGE VITA NATIVE THEORA: archive=%s native_th_info=%u",
                       th_version_string(), (unsigned)sizeof(th_info));
        vita_nt_pool_receipt(ctx ? "alloc" : "alloc FAILED");
    }
    else
        isaac_vita_log(
            "KAGE VITA NATIVE THEORA: alloc info=0x%08x setup=0x%08x answers "
            "NULL like the translated body", (unsigned)info, (unsigned)setup);
    c->eax = (uint32_t)(uintptr_t)ctx;
    (void)gpop(c);
}

/* int th_decode_ctl(th_dec_ctx *, int req, void *buf, size_t buf_sz) */
void __wrap_sub_005da4f0(CPU *__restrict c)
{
    uint32_t dec = vita_nt_arg(c, 0U, VITA_NT_TH_DECODE_CTL_RVA);
    uint32_t req = vita_nt_arg(c, 1U, VITA_NT_TH_DECODE_CTL_RVA);
    uint32_t buf = vita_nt_arg(c, 2U, VITA_NT_TH_DECODE_CTL_RVA);
    uint32_t buf_sz = vita_nt_arg(c, 3U, VITA_NT_TH_DECODE_CTL_RVA);
    int result;

    if (c->fault)
        return;
    if (!dec)
        result = vita_nt_null_ctx("th_decode_ctl");
    else
        result = th_decode_ctl((th_dec_ctx *)(uintptr_t)dec, (int)req,
                               (void *)(uintptr_t)buf, (size_t)buf_sz);
    vita_nt_count(&s_nt.ctls);
    if (s_nt.ctls <= 4U)
        isaac_vita_log(
            "KAGE VITA NATIVE THEORA: ctl req=%d buf=0x%08x size=%u result=%d "
            "ctx=0x%08x", (int)req, (unsigned)buf, (unsigned)buf_sz, result,
            (unsigned)dec);
    c->eax = (uint32_t)result;
    (void)gpop(c);
}

void __wrap_sub_005da670(CPU *__restrict c)  /* th_decode_free(th_dec_ctx *) */
{
    uint32_t dec = vita_nt_arg(c, 0U, VITA_NT_TH_DECODE_FREE_RVA);

    if (c->fault)
        return;
    th_decode_free((th_dec_ctx *)(uintptr_t)dec);  /* NULL-tolerant */
    vita_nt_count(&s_nt.frees);
    vita_nt_receipt("free");
    vita_nt_pool_receipt("free");
    if (s_nt.live_ctx == dec)
        s_nt.live_ctx = 0U;
    (void)gpop(c);
}

/* int th_decode_packetin(th_dec_ctx *, const ogg_packet *, ogg_int64_t *granpos) */
void __wrap_sub_005da6d0(CPU *__restrict c)
{
    uint32_t dec = vita_nt_arg(c, 0U, VITA_NT_TH_DECODE_PACKETIN_RVA);
    uint32_t op = vita_nt_arg(c, 1U, VITA_NT_TH_DECODE_PACKETIN_RVA);
    uint32_t granpos = vita_nt_arg(c, 2U, VITA_NT_TH_DECODE_PACKETIN_RVA);
    uint64_t t0;
    int result;

    if (c->fault)
        return;
    t0 = vita_nt_now_us();
    if (!dec)
        result = vita_nt_null_ctx("th_decode_packetin");
    else
        result = th_decode_packetin((th_dec_ctx *)(uintptr_t)dec,
                                    (const ogg_packet *)(uintptr_t)op,
                                    (ogg_int64_t *)(uintptr_t)granpos);
    vita_nt_time(t0, &s_nt.packetin_us_last, &s_nt.packetin_us_max,
                 &s_nt.packetin_us_sum);
    vita_nt_count(&s_nt.packets);
    if (result == TH_DUPFRAME)
        vita_nt_count(&s_nt.dup_frames);
    else if (result < 0)
        vita_nt_count(&s_nt.packet_errors);
#if ISAAC_VITA_NATIVE_THEORA_PLANE_DIGEST
    vita_nt_digest(dec, result, granpos);
#endif
    if (s_nt.packets <= 8U || (s_nt.packets & 255U) == 0U)
        vita_nt_receipt("packet");
    c->eax = (uint32_t)result;
    (void)gpop(c);
}

/* int th_decode_ycbcr_out(th_dec_ctx *, th_ycbcr_buffer) */
void __wrap_sub_005db3c0(CPU *__restrict c)
{
    uint32_t dec = vita_nt_arg(c, 0U, VITA_NT_TH_DECODE_YCBCR_OUT_RVA);
    uint32_t ycbcr = vita_nt_arg(c, 1U, VITA_NT_TH_DECODE_YCBCR_OUT_RVA);
    uint64_t t0;
    int result;

    if (c->fault)
        return;
    t0 = vita_nt_now_us();
    if (!dec || !ycbcr)
        result = vita_nt_null_ctx("th_decode_ycbcr_out");
    else
        result = th_decode_ycbcr_out((th_dec_ctx *)(uintptr_t)dec,
                                     *(th_ycbcr_buffer *)(uintptr_t)ycbcr);
    vita_nt_time(t0, &s_nt.ycbcr_us_last, &s_nt.ycbcr_us_max,
                 &s_nt.ycbcr_us_sum);
    vita_nt_count(&s_nt.ycbcr_outs);
    if (result < 0)
        vita_nt_count(&s_nt.ycbcr_errors);
    c->eax = (uint32_t)result;
    (void)gpop(c);
}

/* ogg_int64_t th_granule_frame(void *dec, ogg_int64_t granpos) -> edx:eax */
void __wrap_sub_005ddb60(CPU *__restrict c)
{
    uint32_t dec = vita_nt_arg(c, 0U, VITA_NT_TH_GRANULE_FRAME_RVA);
    ogg_int64_t granpos = vita_nt_arg64(c, 1U, VITA_NT_TH_GRANULE_FRAME_RVA);
    ogg_int64_t frame;

    if (c->fault)
        return;
    /* th_granule_frame reads keyframe_granule_shift through the pointer with
     * no check of its own (native +0x38; the nt1 data abort). */
    if (!dec)
        frame = (ogg_int64_t)vita_nt_null_ctx("th_granule_frame");
    else
        frame = th_granule_frame((void *)(uintptr_t)dec, granpos);
    vita_nt_count(&s_nt.granule_frames);
    c->eax = (uint32_t)((uint64_t)frame & 0xffffffffU);
    c->edx = (uint32_t)((uint64_t)frame >> 32);
    (void)gpop(c);
}

/* double th_granule_time(void *dec, ogg_int64_t granpos) -> ST(0) */
void __wrap_sub_005ddc10(CPU *__restrict c)
{
    uint32_t dec = vita_nt_arg(c, 0U, VITA_NT_TH_GRANULE_TIME_RVA);
    ogg_int64_t granpos = vita_nt_arg64(c, 1U, VITA_NT_TH_GRANULE_TIME_RVA);
    double time;

    if (c->fault)
        return;
    if (!dec)
        time = (double)vita_nt_null_ctx("th_granule_time");
    else
        time = th_granule_time((void *)(uintptr_t)dec, granpos);
    vita_nt_count(&s_nt.granule_times);
    fpush(c, time);
    (void)gpop(c);
}

/* ------------------------------------------ YUV->RGB timing (pass-through) -- */

/* TheoraVideoFrame::decode(TheoraPixelTransform *): thiscall, `ret 4`.  The
 * translated body runs unchanged; only its duration is recorded. */
void __wrap_sub_005bf7f0(CPU *__restrict c)
{
    uint64_t t0 = vita_nt_now_us();

    __real_sub_005bf7f0(c);
    vita_nt_time(t0, &s_nt.rgb_us_last, &s_nt.rgb_us_max, &s_nt.rgb_us_sum);
    vita_nt_count(&s_nt.rgb_decodes);
    if (s_nt.rgb_decodes <= 4U)
        isaac_vita_log("KAGE VITA NATIVE THEORA: rgb decode #%u %u us",
                       (unsigned)s_nt.rgb_decodes, (unsigned)s_nt.rgb_us_last);
}

/* ---------------------------------------- slice attribution (pass-through) -- */

/* vorbis_synthesis (005c2910, cdecl, called from decodeNextFrame 005aabfb and
 * the clip restart 005ab466): decodeNextFrame reads ahead until a second of
 * the clip's audio is buffered and decodes every queued Vorbis packet with the
 * translated libvorbis before it touches the video packet, so the audio decode
 * is a candidate for the part of a slice the theora receipts do not cover.
 * Timed only; the translated body runs unchanged.  The vorbis sum is printed
 * in milliseconds by the seam receipt. */
void __wrap_sub_005c2910(CPU *__restrict c)
{
    uint64_t t0 = vita_nt_now_us();

    __real_sub_005c2910(c);
    vita_nt_time(t0, &s_nt.vorbis_us_last, &s_nt.vorbis_us_max,
                 &s_nt.vorbis_us_sum);
    vita_nt_count(&s_nt.vorbis_syntheses);
    if (s_nt.vorbis_syntheses <= 4U || (s_nt.vorbis_syntheses & 255U) == 0U)
        isaac_vita_log("KAGE VITA NATIVE THEORA: vorbis_synthesis #%u %u us "
                       "(mean=%u max=%u sum=%u ms)",
                       (unsigned)s_nt.vorbis_syntheses, (unsigned)s_nt.vorbis_us_last,
                       (unsigned)vita_nt_mean(s_nt.vorbis_us_sum, s_nt.vorbis_syntheses),
                       (unsigned)s_nt.vorbis_us_max,
                       (unsigned)(s_nt.vorbis_us_sum / 1000U));
}
