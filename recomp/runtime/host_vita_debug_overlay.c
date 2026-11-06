/* On-screen debug overlay; see host_vita_debug_overlay.h.
 *
 * Threads that touch this TU:
 *   - any thread that allocates/frees a memblock or creates/deletes a thread
 *     (the --wrap entry points; the first one is crt0 on the main thread):
 *     the ledger, under one spin lock that sleeps when contended;
 *   - the game thread (present hooks, sampling, formatting, logging);
 *   - vitaGL's display-queue thread (the display callback): reads one
 *     published text page and the geometry constants, writes pixels and three
 *     cost counters.  No lock, allocation, stdio or GL call there.
 * Nothing here allocates: every buffer is static, numbers are formatted by
 * hand (newlib's %f path can allocate). */
#include "host_vita_debug_overlay.h"

#if defined(ISAAC_VITA_DEBUG_OVERLAY)

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(ISAAC_VITA_DEBUG_OVERLAY_ORACLE)
# include "host_vita_debug_overlay_test_platform.h"
#else
# include <malloc.h>
# include <psp2/kernel/modulemgr.h>
# include <psp2/kernel/processmgr.h>
# include <psp2/kernel/sysmem.h>
# include <psp2/kernel/threadmgr.h>
# include <vitaGL.h>
#endif

#if defined(ISAAC_VITA_DEBUG_OVERLAY_HEAP_TELEMETRY)
/* host_vita_heap.h declares the telemetry snapshot under the pool macro,
 * which CMake gives only to the heap router and the entry owner. */
# if !defined(ISAAC_VITA_HEAP_OVERFLOW_MSPACE)
#  define ISAAC_VITA_HEAP_OVERFLOW_MSPACE 1
# endif
# include "host_vita_heap.h"
#endif
#if defined(ISAAC_VITA_DEBUG_OVERLAY_OPENAL_POOL)
# include "host_vita_openal_pool.h"
#endif
#if defined(ISAAC_VITA_DEBUG_OVERLAY_SCHEDULER)
# include "kage_vita_fullspeed_scheduler.h"
#endif

void isaac_vita_log(const char *format, ...);
#if defined(ISAAC_VITA_DEBUG_OVERLAY_LUA_ARENA)
/* host_vita_lua.c (exported under ISAAC_VITA_DEBUG_OVERLAY too); declared
 * here like kage_vita_phase_profile.c does, because host_vita_lua_gc.h needs
 * the Lua include path. */
void isaac_vita_lua_arena_stats(uint32_t *live_kb, uint32_t *peak_kb,
                                uint32_t *fallbacks);
#endif

/* ---- geometry (copied from kage_vita_loading.c) ------------------------ */

/* The build passes ISAAC_VITA_DISPLAY_RASTER_* when the default framebuffer
 * is scaled (720x408, 480x272); otherwise the native 960x544 panel.  vitaGL
 * pads the display stride to 64 pixels (gxm.c DISPLAY_STRIDE =
 * VGL_ALIGN(DISPLAY_WIDTH, 64)). */
#if defined(ISAAC_VITA_DISPLAY_RASTER_WIDTH)
# define DOV_DISPLAY_WIDTH  ((unsigned)ISAAC_VITA_DISPLAY_RASTER_WIDTH)
# define DOV_DISPLAY_HEIGHT ((unsigned)ISAAC_VITA_DISPLAY_RASTER_HEIGHT)
#else
# define DOV_DISPLAY_WIDTH  960u
# define DOV_DISPLAY_HEIGHT 544u
#endif
#define DOV_DISPLAY_STRIDE ((DOV_DISPLAY_WIDTH + 63u) & ~63u)

/* A8B8G8R8 is 0xAABBGGRR in a little-endian uint32_t. */
#define DOV_RGBA(r, g, b) \
    ((0xffu << 24u) | ((uint32_t)(b) << 16u) | ((uint32_t)(g) << 8u) | \
     (uint32_t)(r))
#define DOV_BACK   DOV_RGBA(0x00u, 0x00u, 0x00u)
#define DOV_SHADOW DOV_RGBA(0x00u, 0x00u, 0x00u)

static const uint32_t k_dov_colors[ISAAC_VITA_DEBUG_OVERLAY_COLOR_COUNT] = {
    DOV_RGBA(0xebu, 0xe1u, 0xd6u), /* TEXT: kage_vita_loading TEXT  */
    DOV_RGBA(0xb4u, 0xaau, 0xa2u), /* DIM:  a lighter MUTED */
    DOV_RGBA(0xffu, 0xd0u, 0x40u), /* WARN */
    DOV_RGBA(0xffu, 0x50u, 0x50u), /* BAD  */
};

/* ---- timing ------------------------------------------------------------ */

#define DOV_SAMPLE_US     250000u   /* text, frame and cpu figures: 4 Hz */
#define DOV_MEMORY_US     1000000u  /* memory, thread figures: 1 Hz      */
#define DOV_WINDOW_US     1000000u  /* frame statistics window           */
#define DOV_LOG_US        120000000u /* table dump to the log            */
#define DOV_FRAME_RING    128u
#define DOV_CPU_RING      5u        /* 4 Hz samples -> a 1 s span        */
#define DOV_CPU_CORES     3u

static uint64_t dov_now(void)
{
    return (uint64_t)sceKernelGetProcessTimeWide();
}

/* ---- memblock types (vita-headers psp2common/kernel/sysmem.h) ---------- */

#define DOV_TYPE_USER_MAIN_PHYCONT_RW    0x0C80D060u
#define DOV_TYPE_USER_MAIN_PHYCONT_NC_RW 0x0D808060u
#define DOV_TYPE_USER_MAIN_CDIALOG_RW    0x0CA0D060u
#define DOV_TYPE_USER_MAIN_CDIALOG_NC_RW 0x0CA08060u

/* USER_CDRAM_* are 0x094xxxxx; USER_CDIALOG_* are 0x0E20xxxx. */
static uint8_t dov_domain_of_type(uint32_t type)
{
    if ((type >> 24) == 0x09u)
        return ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_CDRAM;
    if (type == DOV_TYPE_USER_MAIN_PHYCONT_RW ||
            type == DOV_TYPE_USER_MAIN_PHYCONT_NC_RW)
        return ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_PHYCONT;
    if (type == DOV_TYPE_USER_MAIN_CDIALOG_RW ||
            type == DOV_TYPE_USER_MAIN_CDIALOG_NC_RW ||
            (type >> 16) == 0x0E20u)
        return ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_CDLG;
    return ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_USER;
}

/* ---- ledger ------------------------------------------------------------ */

#define DOV_BLOCK_SLOTS  160u
#define DOV_GROUP_SLOTS  48u
#define DOV_THREAD_SLOTS 32u
#define DOV_NAME_BYTES   20u

typedef struct dov_block {
    int32_t uid;
    uint16_t group;
    uint16_t live;
    uint32_t bytes;
} dov_block;

typedef struct dov_group {
    char name[DOV_NAME_BYTES];
    uint8_t domain;
    uint8_t used;
    uint16_t live_count;
    uint32_t live_bytes;
    uint32_t peak_bytes;
} dov_group;

typedef struct dov_thread {
    int32_t thid;
    uint32_t live;
    uint32_t stack_request;
    char name[DOV_NAME_BYTES];
} dov_thread;

static uint32_t s_ledger_lock;
static dov_block s_blocks[DOV_BLOCK_SLOTS];
static dov_group s_groups[DOV_GROUP_SLOTS];
static dov_thread s_threads[DOV_THREAD_SLOTS];
static uint32_t s_ledger_dropped;
static uint32_t s_ledger_allocs;
static uint32_t s_ledger_frees;

/* Spin briefly, then sleep: two threads on one core with different
 * priorities must not livelock on a pure spin. */
static void dov_lock(void)
{
    unsigned spins = 0u;

    while (__atomic_exchange_n(&s_ledger_lock, 1u, __ATOMIC_ACQUIRE) != 0u) {
        if (++spins >= 64u) {
            spins = 0u;
            (void)sceKernelDelayThread(50u);
        }
    }
}

static void dov_unlock(void)
{
    __atomic_store_n(&s_ledger_lock, 0u, __ATOMIC_RELEASE);
}

static void dov_copy_name(char *out, const char *name)
{
    unsigned i = 0u;

    if (name) {
        while (i + 1u < DOV_NAME_BYTES && name[i] != '\0') {
            out[i] = name[i];
            ++i;
        }
    }
    if (i == 0u)
        out[i++] = '?';
    while (i < DOV_NAME_BYTES)
        out[i++] = '\0';
}

/* Caller holds the lock. */
static int dov_group_find_locked(const char *name, uint8_t domain)
{
    char key[DOV_NAME_BYTES];
    unsigned i;
    int free_slot = -1;

    dov_copy_name(key, name);
    for (i = 0u; i < DOV_GROUP_SLOTS; ++i) {
        if (!s_groups[i].used) {
            if (free_slot < 0)
                free_slot = (int)i;
            continue;
        }
        if (s_groups[i].domain == domain &&
                memcmp(s_groups[i].name, key, DOV_NAME_BYTES) == 0)
            return (int)i;
    }
    if (free_slot >= 0) {
        memcpy(s_groups[free_slot].name, key, DOV_NAME_BYTES);
        s_groups[free_slot].domain = domain;
        s_groups[free_slot].used = 1u;
        s_groups[free_slot].live_count = 0u;
        s_groups[free_slot].live_bytes = 0u;
        s_groups[free_slot].peak_bytes = 0u;
    }
    return free_slot;
}

/* Boot probe: the free sizes before the first wrapped allocation (crt0,
 * newlib's heap), i.e. what this process could use from its own code on. */
typedef struct dov_boot {
    uint32_t state; /* 0 none, 1 taking, 2 done */
    int32_t result;
    int32_t main_thid;
    uint32_t free_bytes[ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_RECONCILED];
} dov_boot;

static dov_boot s_boot;

static void dov_boot_probe(void)
{
    uint32_t expected = 0u;
    SceKernelFreeMemorySizeInfo info;

    if (__atomic_load_n(&s_boot.state, __ATOMIC_ACQUIRE) != 0u)
        return;
    if (!__atomic_compare_exchange_n(&s_boot.state, &expected, 1u, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    s_boot.result = (int32_t)sceKernelGetFreeMemorySize(&info);
    s_boot.free_bytes[ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_USER] =
        (uint32_t)info.size_user;
    s_boot.free_bytes[ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_CDRAM] =
        (uint32_t)info.size_cdram;
    s_boot.free_bytes[ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_PHYCONT] =
        (uint32_t)info.size_phycont;
    s_boot.main_thid = (int32_t)sceKernelGetThreadId();
    __atomic_store_n(&s_boot.state, 2u, __ATOMIC_RELEASE);
}

static void dov_ledger_add(SceUID uid, const char *name, uint32_t type,
                           uint32_t requested)
{
    void *base = NULL;
    SceKernelMemBlockInfo info;
    uint32_t bytes = requested;
    uint8_t domain = dov_domain_of_type(type);
    unsigned i;
    int group;

    /* The kernel's own figure for the mapping when it answers. */
    if (sceKernelGetMemBlockBase(uid, &base) >= 0 && base) {
        memset(&info, 0, sizeof info);
        info.size = sizeof info;
        if (sceKernelGetMemBlockInfoByAddr(base, &info) >= 0 &&
                info.mappedSize != 0u)
            bytes = (uint32_t)info.mappedSize;
    }
    dov_lock();
    ++s_ledger_allocs;
    group = dov_group_find_locked(name, domain);
    for (i = 0u; group >= 0 && i < DOV_BLOCK_SLOTS; ++i) {
        if (s_blocks[i].live)
            continue;
        s_blocks[i].uid = (int32_t)uid;
        s_blocks[i].group = (uint16_t)group;
        s_blocks[i].bytes = bytes;
        s_blocks[i].live = 1u;
        ++s_groups[group].live_count;
        s_groups[group].live_bytes += bytes;
        if (s_groups[group].live_bytes > s_groups[group].peak_bytes)
            s_groups[group].peak_bytes = s_groups[group].live_bytes;
        dov_unlock();
        return;
    }
    ++s_ledger_dropped;
    dov_unlock();
}

static void dov_ledger_remove(SceUID uid)
{
    unsigned i;

    dov_lock();
    for (i = 0u; i < DOV_BLOCK_SLOTS; ++i) {
        dov_group *group;

        if (!s_blocks[i].live || s_blocks[i].uid != (int32_t)uid)
            continue;
        group = &s_groups[s_blocks[i].group];
        group->live_bytes -= s_blocks[i].bytes;
        if (group->live_count)
            --group->live_count;
        s_blocks[i].live = 0u;
        ++s_ledger_frees;
        break;
    }
    dov_unlock();
}

static void dov_thread_add(SceUID thid, const char *name, uint32_t stack)
{
    unsigned i;

    dov_lock();
    for (i = 0u; i < DOV_THREAD_SLOTS; ++i) {
        if (s_threads[i].live)
            continue;
        s_threads[i].thid = (int32_t)thid;
        s_threads[i].stack_request = stack;
        dov_copy_name(s_threads[i].name, name);
        s_threads[i].live = 1u;
        dov_unlock();
        return;
    }
    ++s_ledger_dropped;
    dov_unlock();
}

static void dov_thread_remove(SceUID thid)
{
    unsigned i;

    dov_lock();
    for (i = 0u; i < DOV_THREAD_SLOTS; ++i) {
        if (s_threads[i].live && s_threads[i].thid == (int32_t)thid) {
            s_threads[i].live = 0u;
            break;
        }
    }
    dov_unlock();
}

/* ---- the --wrap entry points (CMake: ISAAC_VITA_DEBUG_OVERLAY only) ----- */

SceUID __real_sceKernelAllocMemBlock(const char *name,
                                     SceKernelMemBlockType type, SceSize size,
                                     SceKernelAllocMemBlockOpt *opt);
SceUID __wrap_sceKernelAllocMemBlock(const char *name,
                                     SceKernelMemBlockType type, SceSize size,
                                     SceKernelAllocMemBlockOpt *opt);
SceUID __real_kuKernelAllocMemBlock(const char *name,
                                    SceKernelMemBlockType type, SceSize size,
                                    void *opt);
SceUID __wrap_kuKernelAllocMemBlock(const char *name,
                                    SceKernelMemBlockType type, SceSize size,
                                    void *opt);
int __real_sceKernelFreeMemBlock(SceUID uid);
int __wrap_sceKernelFreeMemBlock(SceUID uid);
SceUID __real_sceKernelCreateThread(const char *name,
                                    SceKernelThreadEntry entry,
                                    int init_priority, SceSize stack_size,
                                    SceUInt32 attr, int cpu_affinity_mask,
                                    const SceKernelThreadOptParam *option);
SceUID __wrap_sceKernelCreateThread(const char *name,
                                    SceKernelThreadEntry entry,
                                    int init_priority, SceSize stack_size,
                                    SceUInt32 attr, int cpu_affinity_mask,
                                    const SceKernelThreadOptParam *option);
int __real_sceKernelDeleteThread(SceUID thid);
int __wrap_sceKernelDeleteThread(SceUID thid);
int __real_sceKernelExitDeleteThread(int status);
int __wrap_sceKernelExitDeleteThread(int status);

SceUID __wrap_sceKernelAllocMemBlock(const char *name,
                                     SceKernelMemBlockType type, SceSize size,
                                     SceKernelAllocMemBlockOpt *opt)
{
    SceUID uid;

    dov_boot_probe();
    uid = __real_sceKernelAllocMemBlock(name, type, size, opt);
    if (uid >= 0)
        dov_ledger_add(uid, name, (uint32_t)type, (uint32_t)size);
    return uid;
}

SceUID __wrap_kuKernelAllocMemBlock(const char *name,
                                    SceKernelMemBlockType type, SceSize size,
                                    void *opt)
{
    SceUID uid;

    dov_boot_probe();
    uid = __real_kuKernelAllocMemBlock(name, type, size, opt);
    if (uid >= 0)
        dov_ledger_add(uid, name, (uint32_t)type, (uint32_t)size);
    return uid;
}

#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
# include "gl_record_backend.h"
#endif
int __wrap_sceKernelFreeMemBlock(SceUID uid)
{
#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
    /* Before the free: the GL recorder's admission cache must not outlive it. */
    isaac_gl_record_memblock_freeing();
#endif
    int result = __real_sceKernelFreeMemBlock(uid);

    if (result >= 0)
        dov_ledger_remove(uid);
    return result;
}

SceUID __wrap_sceKernelCreateThread(const char *name,
                                    SceKernelThreadEntry entry,
                                    int init_priority, SceSize stack_size,
                                    SceUInt32 attr, int cpu_affinity_mask,
                                    const SceKernelThreadOptParam *option)
{
    SceUID thid;

    dov_boot_probe();
    thid = __real_sceKernelCreateThread(name, entry, init_priority,
                                        stack_size, attr, cpu_affinity_mask,
                                        option);
    if (thid >= 0)
        dov_thread_add(thid, name, (uint32_t)stack_size);
    return thid;
}

int __wrap_sceKernelDeleteThread(SceUID thid)
{
    int result = __real_sceKernelDeleteThread(thid);

    if (result >= 0)
        dov_thread_remove(thid);
    return result;
}

int __wrap_sceKernelExitDeleteThread(int status)
{
    /* Does not return on success: drop the slot first. */
    dov_thread_remove((SceUID)sceKernelGetThreadId());
    return __real_sceKernelExitDeleteThread(status);
}

/* ---- known owners ------------------------------------------------------ */

enum dov_used_source {
    DOV_USED_NONE = 0,
    DOV_USED_NEWLIB,
    DOV_USED_VGL_RAM,
    DOV_USED_VGL_VRAM,
    DOV_USED_VGL_PHYCONT,
    DOV_USED_VGL_BUDGET,
    DOV_USED_LUA,
    DOV_USED_OVERFLOW,
    DOV_USED_OPENAL
};

/* Memblock names as their owners spell them: "Newlib heap" is vitasdk
 * newlib sbrk.c, the *_mempool names are vitaGL mem_utils.c vgl_mem_init,
 * the isaac_* names are the runtime's sceKernelAllocMemBlock call sites. */
static const struct dov_known {
    const char *name;
    const char *label;
    uint8_t source;
} k_dov_known[] = {
    { "Newlib heap", "NEWLIB HEAP", DOV_USED_NEWLIB },
    { "ram_mempool", "VITAGL RAM", DOV_USED_VGL_RAM },
    { "cdram_mempool", "VITAGL VRAM", DOV_USED_VGL_VRAM },
    { "phycont_mempool", "VITAGL PHYCONT", DOV_USED_VGL_PHYCONT },
    { "cdlg_mempool", "VITAGL CDLG", DOV_USED_VGL_BUDGET },
    { "isaac_lua_arena", "LUA ARENA", DOV_USED_LUA },
    { "isaac_heap_overflow", "HEAP OVERFLOW", DOV_USED_OVERFLOW },
    { "isaac_openal_pool", "OPENAL POOL", DOV_USED_OPENAL },
    { "isaac_heap_ledger", "HEAP LEDGER", DOV_USED_NONE },
    { "isaac_pe_image", "PE IMAGE", DOV_USED_NONE },
    { "isaac_guest_stack", "GUEST STACK", DOV_USED_NONE },
    { "isaac_np_reserve", "PNG RESERVE", DOV_USED_NONE },
    { "isaac_np_reuse", "PNG REUSE", DOV_USED_NONE },
    { "isaac_np_scratch", "PNG SCRATCH", DOV_USED_NONE },
    { "isaac_texel_scratch", "TEXEL SCRATCH", DOV_USED_NONE },
    { "isaac_texel_probe", "TEXEL PROBE", DOV_USED_NONE },
    { "isaac_anm2_scratch", "ANM2 SCRATCH", DOV_USED_NONE },
    { "isaac_theora_pool", "THEORA POOL", DOV_USED_NONE },
    { "isaac_nv_vorbis", "VORBIS POOL", DOV_USED_NONE },
    { "isaac_ogg_emergency", "OGG EMERGENCY", DOV_USED_NONE },
    { "isaac-fios-cache", "FIOS CACHE", DOV_USED_NONE },
    { "isaac_room_entries", "ROOM ENTRIES", DOV_USED_NONE },
    { "isaac_save_writer", "SAVE WRITER", DOV_USED_NONE },
};

#define DOV_ARRAY_COUNT(a) (sizeof (a) / sizeof (a)[0])

/* vitaGL's ordinal 2 is VGL_MEM_PHYCONT in the pinned overlay header and
 * VGL_MEM_SLOW in older SDKs; derive it like kage_vita_backend.c does. */
#define DOV_VGL_MEM_PHYCONT ((vglMemType)(VGL_MEM_RAM + 1))
_Static_assert(VGL_MEM_VRAM + 1 == VGL_MEM_RAM &&
               VGL_MEM_RAM + 2 == VGL_MEM_BUDGET,
               "vitaGL memory-domain ABI changed");

/* ---- state ------------------------------------------------------------- */

typedef struct dov_frame {
    uint64_t at_us;
    uint32_t frame_us;
    uint32_t swap_us;
    uint32_t update_us;
    uint32_t render_us;
} dov_frame;

typedef struct dov_thread_view {
    int32_t thid;
    uint32_t stack_bytes;
    uint64_t run_us;
    char name[DOV_NAME_BYTES];
} dov_thread_view;

volatile uint8_t g_isaac_vita_debug_overlay_mode;

static isaac_vita_debug_overlay_model s_model;
static isaac_vita_debug_overlay_page s_pages[3];
static uint32_t s_published = 0xffu; /* index into s_pages, 0xff none */
static uint32_t s_previous = 0xffu;

static dov_frame s_frames[DOV_FRAME_RING];
static uint32_t s_frame_head;
static uint32_t s_frame_count;
static uint64_t s_swap_enter_us;
static uint64_t s_last_return_us;
static int32_t s_game_thid;

static uint64_t s_last_sample_us;
static uint64_t s_last_memory_us;
static uint64_t s_last_log_us;
static uint32_t s_samples;
static uint32_t s_memory_samples;

static uint64_t s_cpu_at[DOV_CPU_RING];
static uint64_t s_cpu_idle[DOV_CPU_RING][DOV_CPU_CORES];
static uint32_t s_cpu_count;

static dov_thread_view s_thread_prev[DOV_THREAD_SLOTS + 1u];
static uint32_t s_thread_prev_count;
static uint64_t s_thread_prev_at;

static uint32_t s_peak_sampled[9];  /* by enum dov_used_source */

static uint64_t s_cost_frame_sum_us;
static uint32_t s_cost_frame_count;
static uint32_t s_cost_sample_max_us;

static uint32_t s_draw_sum_us;
static uint32_t s_draw_count;
static uint32_t s_draw_max_us;

static uint32_t s_eboot_rx;
static uint32_t s_eboot_rw;
static uint32_t s_main_stack;
static int32_t s_eboot_result;

/* ---- small formatting helpers ------------------------------------------ */

typedef struct dov_line {
    char *text;
    unsigned length;
    unsigned cap;
} dov_line;

static void dov_put(dov_line *line, const char *text)
{
    while (*text && line->length < line->cap)
        line->text[line->length++] = *text++;
}

static void dov_putc(dov_line *line, char c)
{
    if (line->length < line->cap)
        line->text[line->length++] = c;
}

static void dov_pad_to(dov_line *line, unsigned column)
{
    while (line->length < column && line->length < line->cap)
        line->text[line->length++] = ' ';
}

static unsigned dov_utoa(char *out, uint64_t value)
{
    char digits[24];
    unsigned n = 0u;
    unsigned i;

    do {
        digits[n++] = (char)('0' + (unsigned)(value % 10u));
        value /= 10u;
    } while (value != 0u && n < sizeof digits);
    for (i = 0u; i < n; ++i)
        out[i] = digits[n - 1u - i];
    out[n] = '\0';
    return n;
}

static void dov_put_u(dov_line *line, uint64_t value)
{
    char text[24];

    (void)dov_utoa(text, value);
    dov_put(line, text);
}

/* Tenths: 1234 -> "123.4". */
static void dov_put_tenths(dov_line *line, uint64_t tenths)
{
    dov_put_u(line, tenths / 10u);
    dov_putc(line, '.');
    dov_putc(line, (char)('0' + (unsigned)(tenths % 10u)));
}

/* Microseconds as milliseconds with one decimal, rounded. */
static void dov_put_ms(dov_line *line, uint32_t microseconds)
{
    dov_put_tenths(line, ((uint64_t)microseconds + 50u) / 100u);
}

unsigned isaac_vita_debug_overlay_format_bytes(char *out, unsigned width,
                                               uint64_t bytes)
{
    char text[24];
    unsigned n;
    unsigned pad;
    unsigned i;

    if (bytes >= 1048576u) {
        uint64_t tenths = (bytes * 10u + 524288u) / 1048576u;

        if (tenths >= 10000u) {
            n = dov_utoa(text, (tenths + 5u) / 10u);
        } else {
            n = dov_utoa(text, tenths / 10u);
            text[n++] = '.';
            text[n++] = (char)('0' + (unsigned)(tenths % 10u));
            text[n] = '\0';
        }
    } else if (bytes == 0u) {
        text[0] = '0';
        text[1] = '\0';
        n = 1u;
    } else {
        /* Ceiling: a non-zero size never prints as 0K. */
        n = dov_utoa(text, (bytes + 1023u) / 1024u);
        text[n++] = 'K';
        text[n] = '\0';
    }
    if (n > width)
        n = width;
    pad = width - n;
    for (i = 0u; i < pad; ++i)
        out[i] = ' ';
    memcpy(out + pad, text, n);
    return width;
}

static void dov_put_bytes(dov_line *line, unsigned width, uint64_t bytes)
{
    char text[24];
    unsigned n;

    if (width > 16u)
        width = 16u;
    n = isaac_vita_debug_overlay_format_bytes(text, width, bytes);
    text[n] = '\0';
    dov_put(line, text);
}

/* Same figure without the left padding. */
static void dov_put_bytes_tight(dov_line *line, uint64_t bytes)
{
    char text[24];
    unsigned n = isaac_vita_debug_overlay_format_bytes(text, 16u, bytes);
    unsigned i = 0u;

    text[n] = '\0';
    while (text[i] == ' ')
        ++i;
    dov_put(line, text + i);
}

static void dov_put_signed_bytes(dov_line *line, unsigned width, int64_t bytes)
{
    char text[24];
    char value[24];
    unsigned n;
    unsigned i;
    unsigned pad;

    if (bytes >= 0) {
        if (width <= 1u)
            dov_put_bytes_tight(line, (uint64_t)bytes);
        else
            dov_put_bytes(line, width, (uint64_t)bytes);
        return;
    }
    n = isaac_vita_debug_overlay_format_bytes(value, 16u, (uint64_t)(-bytes));
    i = 0u;
    while (i < n && value[i] == ' ')
        ++i;
    text[0] = '-';
    memcpy(text + 1, value + i, n - i);
    n = n - i + 1u;
    /* Right-aligned in width; a longer figure is never cut (the line cap
     * still bounds it). */
    pad = n < width ? width - n : 0u;
    for (i = 0u; i < pad; ++i)
        dov_putc(line, ' ');
    for (i = 0u; i < n; ++i)
        dov_putc(line, text[i]);
}

static void dov_put_dash(dov_line *line, unsigned width)
{
    unsigned i;

    for (i = 0u; i + 1u < width; ++i)
        dov_putc(line, ' ');
    dov_putc(line, '-');
}

/* ---- model math -------------------------------------------------------- */

uint64_t isaac_vita_debug_overlay_domain_rows(
    const isaac_vita_debug_overlay_model *model, unsigned domain)
{
    uint64_t sum = 0u;
    uint32_t i;

    for (i = 0u; i < model->row_count && i < ISAAC_VITA_DEBUG_OVERLAY_MAX_ROWS;
         ++i) {
        if (model->rows[i].domain == domain)
            sum += model->rows[i].resv;
    }
    return sum;
}

int64_t isaac_vita_debug_overlay_unaccounted(
    const isaac_vita_debug_overlay_model *model, unsigned domain)
{
    if (domain >= ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_RECONCILED)
        return 0;
    return (int64_t)model->base_bytes[domain] -
           (int64_t)isaac_vita_debug_overlay_domain_rows(model, domain) -
           (int64_t)model->free_bytes[domain];
}

/* ---- formatter --------------------------------------------------------- */

#define DOV_LABEL_COLS 14u
#define DOV_VALUE_COLS 6u

static dov_line dov_begin_line(isaac_vita_debug_overlay_page *page,
                               uint8_t color)
{
    dov_line line;
    unsigned index = page->line_count;

    if (index >= ISAAC_VITA_DEBUG_OVERLAY_LINES)
        index = ISAAC_VITA_DEBUG_OVERLAY_LINES - 1u;
    memset(page->text[index], ' ', ISAAC_VITA_DEBUG_OVERLAY_COLS);
    page->color[index] = color;
    page->length[index] = 0u;
    line.text = page->text[index];
    line.length = 0u;
    line.cap = ISAAC_VITA_DEBUG_OVERLAY_COLS;
    return line;
}

static void dov_end_line(isaac_vita_debug_overlay_page *page,
                         const dov_line *line)
{
    if (page->line_count >= ISAAC_VITA_DEBUG_OVERLAY_LINES)
        return;
    page->length[page->line_count] = (uint8_t)line->length;
    ++page->line_count;
}

static uint8_t dov_fps_color(const isaac_vita_debug_overlay_model *model)
{
    if (!model->have_frames)
        return ISAAC_VITA_DEBUG_OVERLAY_COLOR_DIM;
    if (model->fps_presents < 40u)
        return ISAAC_VITA_DEBUG_OVERLAY_COLOR_BAD;
    if (model->fps_presents < 55u || model->frame_max_us > 33333u)
        return ISAAC_VITA_DEBUG_OVERLAY_COLOR_WARN;
    return ISAAC_VITA_DEBUG_OVERLAY_COLOR_TEXT;
}

static uint8_t dov_free_color(uint32_t free_user)
{
    if (free_user < 8u * 1048576u)
        return ISAAC_VITA_DEBUG_OVERLAY_COLOR_BAD;
    if (free_user < 16u * 1048576u)
        return ISAAC_VITA_DEBUG_OVERLAY_COLOR_WARN;
    return ISAAC_VITA_DEBUG_OVERLAY_COLOR_TEXT;
}

static void dov_format_compact(const isaac_vita_debug_overlay_model *model,
                               isaac_vita_debug_overlay_page *page)
{
    dov_line line = dov_begin_line(page, dov_fps_color(model));

    if (model->have_frames) {
        dov_put(&line, "FPS ");
        dov_put_u(&line, model->fps_presents);
        dov_put(&line, "  ");
        dov_put_ms(&line, model->frame_p50_us);
        dov_putc(&line, '/');
        dov_put_ms(&line, model->frame_max_us);
        dov_put(&line, " MS");
    } else {
        dov_put(&line, "FPS --");
    }
    dov_end_line(page, &line);

    line = dov_begin_line(page, model->have_free
        ? dov_free_color(model->free_bytes[ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_USER])
        : ISAAC_VITA_DEBUG_OVERLAY_COLOR_DIM);
    if (model->have_free) {
        dov_put(&line, "FREE U ");
        dov_put_bytes_tight(&line,
            model->free_bytes[ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_USER]);
        dov_put(&line, "M C ");
        dov_put_bytes_tight(&line,
            model->free_bytes[ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_CDRAM]);
        dov_putc(&line, 'M');
    } else {
        dov_put(&line, "FREE --");
    }
    dov_put(&line, " OVL ");
    dov_put_ms(&line, model->cost_frame_avg_us + model->cost_draw_avg_us);
    dov_end_line(page, &line);
}

static void dov_put_row(isaac_vita_debug_overlay_page *page, uint8_t color,
                        const char *label, int have_used, uint64_t used,
                        uint64_t resv, int have_peak, uint64_t peak,
                        int peak_sampled)
{
    dov_line line = dov_begin_line(page, color);

    dov_put(&line, label);
    dov_pad_to(&line, DOV_LABEL_COLS);
    if (have_used)
        dov_put_bytes(&line, DOV_VALUE_COLS, used);
    else
        dov_put_dash(&line, DOV_VALUE_COLS);
    dov_putc(&line, ' ');
    dov_put_bytes(&line, DOV_VALUE_COLS, resv);
    dov_putc(&line, ' ');
    if (have_peak) {
        dov_put_bytes(&line, DOV_VALUE_COLS - 1u, peak);
        dov_putc(&line, peak_sampled ? '*' : ' ');
    } else {
        dov_put_dash(&line, DOV_VALUE_COLS - 1u);
    }
    dov_end_line(page, &line);
}

/* Indices of the USER rows sorted by resv, descending (stable). */
static unsigned dov_user_order(const isaac_vita_debug_overlay_model *model,
                               uint8_t *order)
{
    unsigned count = 0u;
    uint32_t i;

    for (i = 0u; i < model->row_count && i < ISAAC_VITA_DEBUG_OVERLAY_MAX_ROWS;
         ++i) {
        unsigned at;

        if (model->rows[i].domain != ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_USER)
            continue;
        at = count++;
        while (at > 0u && model->rows[order[at - 1u]].resv < model->rows[i].resv) {
            order[at] = order[at - 1u];
            --at;
        }
        order[at] = (uint8_t)i;
    }
    return count;
}

/* "CDRAM VGL 98.0/104.0 F 2.0 U 6.0 O 1.0" */
static void dov_put_domain_line(const isaac_vita_debug_overlay_model *model,
                                isaac_vita_debug_overlay_page *page,
                                unsigned domain, const char *name)
{
    dov_line line = dov_begin_line(page, ISAAC_VITA_DEBUG_OVERLAY_COLOR_TEXT);
    uint64_t vgl_used = 0u;
    uint64_t vgl_resv = 0u;
    uint64_t other = 0u;
    int have_vgl = 0;
    uint32_t i;

    for (i = 0u; i < model->row_count && i < ISAAC_VITA_DEBUG_OVERLAY_MAX_ROWS;
         ++i) {
        const isaac_vita_debug_overlay_row *row = &model->rows[i];

        if (row->domain != domain)
            continue;
        if (row->flags & ISAAC_VITA_DEBUG_OVERLAY_ROW_VGL) {
            have_vgl = 1;
            vgl_resv += row->resv;
            if (row->flags & ISAAC_VITA_DEBUG_OVERLAY_ROW_USED)
                vgl_used += row->used;
        } else {
            other += row->resv;
        }
    }
    dov_put(&line, name);
    dov_put(&line, " VGL ");
    if (have_vgl) {
        dov_put_bytes_tight(&line, vgl_used);
        dov_putc(&line, '/');
        dov_put_bytes_tight(&line, vgl_resv);
    } else {
        dov_put(&line, "-");
    }
    if (domain < ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_RECONCILED) {
        dov_put(&line, " F ");
        if (model->have_free)
            dov_put_bytes_tight(&line, model->free_bytes[domain]);
        else
            dov_put(&line, "N/A");
        dov_put(&line, " U ");
        if (model->have_free && model->have_base)
            dov_put_signed_bytes(&line, 1u,
                isaac_vita_debug_overlay_unaccounted(model, domain));
        else
            dov_put(&line, "N/A");
    }
    if (other) {
        dov_put(&line, " O ");
        dov_put_bytes_tight(&line, other);
    }
    dov_end_line(page, &line);
}

static void dov_format_full(const isaac_vita_debug_overlay_model *model,
                            isaac_vita_debug_overlay_page *page)
{
    dov_line line;
    uint8_t order[ISAAC_VITA_DEBUG_OVERLAY_MAX_ROWS];
    unsigned user_rows;
    unsigned shown;
    unsigned fixed;
    unsigned budget;
    unsigned i;
    int have_cdlg = 0;

    /* 1: frames */
    line = dov_begin_line(page, dov_fps_color(model));
    if (model->have_frames) {
        dov_put(&line, "FPS ");
        dov_put_u(&line, model->fps_presents);
        dov_put(&line, "  FRAME ");
        dov_put_ms(&line, model->frame_p50_us);
        dov_putc(&line, '/');
        dov_put_ms(&line, model->frame_p95_us);
        dov_putc(&line, '/');
        dov_put_ms(&line, model->frame_max_us);
        dov_put(&line, " MS");
    } else {
        dov_put(&line, "FPS --  NO PRESENTS YET");
    }
    dov_end_line(page, &line);

    /* 2: breakdown, p50/max ms over the same second */
    line = dov_begin_line(page, ISAAC_VITA_DEBUG_OVERLAY_COLOR_TEXT);
    if (model->have_frames && model->have_scheduler) {
        dov_put(&line, "UPD ");
        dov_put_ms(&line, model->update_p50_us);
        dov_putc(&line, '/');
        dov_put_ms(&line, model->update_max_us);
        dov_put(&line, " RND ");
        dov_put_ms(&line, model->render_p50_us);
        dov_putc(&line, '/');
        dov_put_ms(&line, model->render_max_us);
        dov_putc(&line, ' ');
    } else {
        dov_put(&line, "UPD N/A RND N/A ");
    }
    dov_put(&line, "SWP ");
    if (model->have_frames) {
        dov_put_ms(&line, model->swap_p50_us);
        dov_putc(&line, '/');
        dov_put_ms(&line, model->swap_max_us);
    } else {
        dov_put(&line, "N/A");
    }
    dov_end_line(page, &line);

    /* 3: cores */
    line = dov_begin_line(page, ISAAC_VITA_DEBUG_OVERLAY_COLOR_TEXT);
    if (model->have_cpu) {
        for (i = 0u; i < DOV_CPU_CORES; ++i) {
            dov_put(&line, "CPU");
            dov_putc(&line, (char)('0' + i));
            dov_putc(&line, ' ');
            dov_put_u(&line, model->cpu_load[i]);
            dov_put(&line, "% ");
        }
    } else {
        dov_put(&line, "CPU N/A ");
    }
    dov_put(&line, "GAME ");
    if (model->have_game_thread) {
        dov_put_u(&line, model->game_thread_load);
        dov_putc(&line, '%');
    } else {
        dov_put(&line, "N/A");
    }
    dov_end_line(page, &line);

    /* 4: busiest other threads (optional) */
    if (model->top_thread_count) {
        line = dov_begin_line(page, ISAAC_VITA_DEBUG_OVERLAY_COLOR_DIM);
        dov_put(&line, "THR");
        for (i = 0u; i < model->top_thread_count && i < 3u; ++i) {
            dov_putc(&line, ' ');
            dov_put(&line, model->top_thread_name[i]);
            dov_putc(&line, ' ');
            dov_put_u(&line, model->top_thread_load[i]);
            dov_putc(&line, '%');
        }
        dov_end_line(page, &line);
    }

    /* table header */
    line = dov_begin_line(page, ISAAC_VITA_DEBUG_OVERLAY_COLOR_DIM);
    dov_put(&line, "RAM MIB");
    dov_pad_to(&line, DOV_LABEL_COLS);
    dov_put(&line, "  USED   RESV   PEAK");
    dov_end_line(page, &line);

    if (!model->have_memory) {
        line = dov_begin_line(page, ISAAC_VITA_DEBUG_OVERLAY_COLOR_DIM);
        dov_put(&line, "NO MEMORY SAMPLE YET");
        dov_end_line(page, &line);
        goto cost;
    }

    for (i = 0u; i < model->row_count && i < ISAAC_VITA_DEBUG_OVERLAY_MAX_ROWS;
         ++i) {
        if (model->rows[i].domain == ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_CDLG)
            have_cdlg = 1;
    }
    /* Everything below the user rows: FREE, UNACCOUNTED, BASE, CDRAM,
     * PHYCONT, [CDLG], OVL, [ledger warning]. */
    fixed = 3u + 2u + (have_cdlg ? 1u : 0u) + 1u +
            (model->ledger_dropped ? 1u : 0u);
    budget = page->line_count + fixed < ISAAC_VITA_DEBUG_OVERLAY_LINES
        ? ISAAC_VITA_DEBUG_OVERLAY_LINES - page->line_count - fixed : 0u;
    user_rows = dov_user_order(model, order);
    shown = user_rows;
    if (shown > budget)
        shown = budget > 0u ? budget - 1u : 0u; /* one line for OTHER */
    for (i = 0u; i < shown; ++i) {
        const isaac_vita_debug_overlay_row *row = &model->rows[order[i]];

        dov_put_row(page, ISAAC_VITA_DEBUG_OVERLAY_COLOR_TEXT, row->label,
                    (row->flags & ISAAC_VITA_DEBUG_OVERLAY_ROW_USED) != 0u,
                    row->used, row->resv,
                    (row->flags & ISAAC_VITA_DEBUG_OVERLAY_ROW_PEAK) != 0u,
                    row->peak,
                    (row->flags & ISAAC_VITA_DEBUG_OVERLAY_ROW_PEAK_SAMPLED) != 0u);
    }
    if (shown < user_rows) {
        uint64_t rest = 0u;
        char label[DOV_LABEL_COLS + 1u];
        dov_line text;

        for (i = shown; i < user_rows; ++i)
            rest += model->rows[order[i]].resv;
        text.text = label;
        text.length = 0u;
        text.cap = DOV_LABEL_COLS;
        dov_put(&text, "OTHER (");
        dov_put_u(&text, user_rows - shown);
        dov_putc(&text, ')');
        label[text.length] = '\0';
        dov_put_row(page, ISAAC_VITA_DEBUG_OVERLAY_COLOR_TEXT, label,
                    0, 0u, rest, 0, 0u, 0);
    }

    /* FREE / UNACCOUNTED / BASE of the user domain */
    line = dov_begin_line(page, model->have_free
        ? dov_free_color(model->free_bytes[ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_USER])
        : ISAAC_VITA_DEBUG_OVERLAY_COLOR_DIM);
    dov_put(&line, "FREE");
    dov_pad_to(&line, DOV_LABEL_COLS + DOV_VALUE_COLS + 1u);
    if (model->have_free)
        dov_put_bytes(&line, DOV_VALUE_COLS,
            model->free_bytes[ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_USER]);
    else
        dov_put(&line, "   N/A");
    dov_end_line(page, &line);

    line = dov_begin_line(page, ISAAC_VITA_DEBUG_OVERLAY_COLOR_TEXT);
    dov_put(&line, "UNACCOUNTED");
    dov_pad_to(&line, DOV_LABEL_COLS + DOV_VALUE_COLS + 1u);
    if (model->have_free && model->have_base)
        dov_put_signed_bytes(&line, DOV_VALUE_COLS,
            isaac_vita_debug_overlay_unaccounted(
                model, ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_USER));
    else
        dov_put(&line, "   N/A");
    dov_end_line(page, &line);

    line = dov_begin_line(page, ISAAC_VITA_DEBUG_OVERLAY_COLOR_DIM);
    dov_put(&line, "BASE (BOOT)");
    dov_pad_to(&line, DOV_LABEL_COLS + DOV_VALUE_COLS + 1u);
    if (model->have_base)
        dov_put_bytes(&line, DOV_VALUE_COLS,
            model->base_bytes[ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_USER]);
    else
        dov_put(&line, "   N/A");
    dov_end_line(page, &line);

    dov_put_domain_line(model, page, ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_CDRAM,
                        "CDRAM");
    dov_put_domain_line(model, page, ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_PHYCONT,
                        "PHYC");
    if (have_cdlg)
        dov_put_domain_line(model, page, ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_CDLG,
                            "CDLG");
    if (model->ledger_dropped) {
        line = dov_begin_line(page, ISAAC_VITA_DEBUG_OVERLAY_COLOR_BAD);
        dov_put(&line, "LEDGER FULL: ");
        dov_put_u(&line, model->ledger_dropped);
        dov_put(&line, " NOT BOOKED");
        dov_end_line(page, &line);
    }

cost:
    line = dov_begin_line(page, ISAAC_VITA_DEBUG_OVERLAY_COLOR_DIM);
    dov_put(&line, "OVL G ");
    dov_put_ms(&line, model->cost_frame_avg_us);
    dov_put(&line, " S ");
    dov_put_ms(&line, model->cost_sample_us);
    dov_putc(&line, '/');
    dov_put_ms(&line, model->cost_sample_max_us);
    dov_put(&line, " D ");
    dov_put_ms(&line, model->cost_draw_avg_us);
    dov_putc(&line, '/');
    dov_put_ms(&line, model->cost_draw_max_us);
    dov_put(&line, " MS");
    dov_end_line(page, &line);
}

unsigned isaac_vita_debug_overlay_format(
    const isaac_vita_debug_overlay_model *model, uint8_t mode,
    isaac_vita_debug_overlay_page *page)
{
    memset(page, 0, sizeof *page);
    page->mode = mode;
    if (mode == ISAAC_VITA_DEBUG_OVERLAY_MODE_COMPACT)
        dov_format_compact(model, page);
    else if (mode == ISAAC_VITA_DEBUG_OVERLAY_MODE_FULL)
        dov_format_full(model, page);
    return page->line_count;
}

/* ---- font -------------------------------------------------------------- */

/* 5x7 glyphs for ASCII 0x20..0x5F, bit 4 = leftmost column.  Digits and
 * letters are copied from kage_vita_loading.c (s_loading_digits,
 * s_loading_letters) by tools in this commit's test; punctuation is drawn
 * for this overlay.  Lowercase maps to uppercase; anything else is blank. */
static const uint8_t k_dov_font[64][7] = {
    { 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u }, /* space */
    { 0x04u, 0x04u, 0x04u, 0x04u, 0x04u, 0x00u, 0x04u }, /* ! */
    { 0x0au, 0x0au, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u }, /* " */
    { 0x0au, 0x0au, 0x1fu, 0x0au, 0x1fu, 0x0au, 0x0au }, /* # */
    { 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u }, /* $ */
    { 0x18u, 0x19u, 0x02u, 0x04u, 0x08u, 0x13u, 0x03u }, /* % */
    { 0x0cu, 0x12u, 0x14u, 0x08u, 0x15u, 0x12u, 0x0du }, /* & */
    { 0x04u, 0x04u, 0x08u, 0x00u, 0x00u, 0x00u, 0x00u }, /* quote */
    { 0x02u, 0x04u, 0x08u, 0x08u, 0x08u, 0x04u, 0x02u }, /* ( */
    { 0x08u, 0x04u, 0x02u, 0x02u, 0x02u, 0x04u, 0x08u }, /* ) */
    { 0x00u, 0x04u, 0x15u, 0x0eu, 0x15u, 0x04u, 0x00u }, /* * */
    { 0x00u, 0x04u, 0x04u, 0x1fu, 0x04u, 0x04u, 0x00u }, /* + */
    { 0x00u, 0x00u, 0x00u, 0x00u, 0x0cu, 0x04u, 0x08u }, /* , */
    { 0x00u, 0x00u, 0x00u, 0x0eu, 0x00u, 0x00u, 0x00u }, /* - */
    { 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x0cu, 0x0cu }, /* . */
    { 0x01u, 0x01u, 0x02u, 0x04u, 0x08u, 0x10u, 0x10u }, /* / */
    { 0x0eu, 0x11u, 0x13u, 0x15u, 0x19u, 0x11u, 0x0eu }, /* 0 */
    { 0x04u, 0x0cu, 0x04u, 0x04u, 0x04u, 0x04u, 0x0eu }, /* 1 */
    { 0x0eu, 0x11u, 0x01u, 0x02u, 0x04u, 0x08u, 0x1fu }, /* 2 */
    { 0x1eu, 0x01u, 0x01u, 0x0eu, 0x01u, 0x01u, 0x1eu }, /* 3 */
    { 0x02u, 0x06u, 0x0au, 0x12u, 0x1fu, 0x02u, 0x02u }, /* 4 */
    { 0x1fu, 0x10u, 0x10u, 0x1eu, 0x01u, 0x01u, 0x1eu }, /* 5 */
    { 0x06u, 0x08u, 0x10u, 0x1eu, 0x11u, 0x11u, 0x0eu }, /* 6 */
    { 0x1fu, 0x01u, 0x02u, 0x04u, 0x08u, 0x08u, 0x08u }, /* 7 */
    { 0x0eu, 0x11u, 0x11u, 0x0eu, 0x11u, 0x11u, 0x0eu }, /* 8 */
    { 0x0eu, 0x11u, 0x11u, 0x0fu, 0x01u, 0x02u, 0x0cu }, /* 9 */
    { 0x00u, 0x0cu, 0x0cu, 0x00u, 0x0cu, 0x0cu, 0x00u }, /* : */
    { 0x00u, 0x0cu, 0x0cu, 0x00u, 0x0cu, 0x04u, 0x08u }, /* ; */
    { 0x02u, 0x04u, 0x08u, 0x10u, 0x08u, 0x04u, 0x02u }, /* < */
    { 0x00u, 0x00u, 0x1fu, 0x00u, 0x1fu, 0x00u, 0x00u }, /* = */
    { 0x08u, 0x04u, 0x02u, 0x01u, 0x02u, 0x04u, 0x08u }, /* > */
    { 0x0eu, 0x11u, 0x01u, 0x02u, 0x04u, 0x00u, 0x04u }, /* ? */
    { 0x0eu, 0x11u, 0x17u, 0x15u, 0x17u, 0x10u, 0x0fu }, /* @ */
    { 0x0eu, 0x11u, 0x11u, 0x1fu, 0x11u, 0x11u, 0x11u }, /* A */
    { 0x1eu, 0x11u, 0x11u, 0x1eu, 0x11u, 0x11u, 0x1eu }, /* B */
    { 0x0fu, 0x10u, 0x10u, 0x10u, 0x10u, 0x10u, 0x0fu }, /* C */
    { 0x1eu, 0x11u, 0x11u, 0x11u, 0x11u, 0x11u, 0x1eu }, /* D */
    { 0x1fu, 0x10u, 0x10u, 0x1eu, 0x10u, 0x10u, 0x1fu }, /* E */
    { 0x1fu, 0x10u, 0x10u, 0x1eu, 0x10u, 0x10u, 0x10u }, /* F */
    { 0x0fu, 0x10u, 0x10u, 0x17u, 0x11u, 0x11u, 0x0fu }, /* G */
    { 0x11u, 0x11u, 0x11u, 0x1fu, 0x11u, 0x11u, 0x11u }, /* H */
    { 0x1fu, 0x04u, 0x04u, 0x04u, 0x04u, 0x04u, 0x1fu }, /* I */
    { 0x07u, 0x02u, 0x02u, 0x02u, 0x12u, 0x12u, 0x0cu }, /* J */
    { 0x11u, 0x12u, 0x14u, 0x18u, 0x14u, 0x12u, 0x11u }, /* K */
    { 0x10u, 0x10u, 0x10u, 0x10u, 0x10u, 0x10u, 0x1fu }, /* L */
    { 0x11u, 0x1bu, 0x15u, 0x15u, 0x11u, 0x11u, 0x11u }, /* M */
    { 0x11u, 0x19u, 0x15u, 0x13u, 0x11u, 0x11u, 0x11u }, /* N */
    { 0x0eu, 0x11u, 0x11u, 0x11u, 0x11u, 0x11u, 0x0eu }, /* O */
    { 0x1eu, 0x11u, 0x11u, 0x1eu, 0x10u, 0x10u, 0x10u }, /* P */
    { 0x0eu, 0x11u, 0x11u, 0x11u, 0x15u, 0x12u, 0x0du }, /* Q */
    { 0x1eu, 0x11u, 0x11u, 0x1eu, 0x14u, 0x12u, 0x11u }, /* R */
    { 0x0fu, 0x10u, 0x10u, 0x0eu, 0x01u, 0x01u, 0x1eu }, /* S */
    { 0x1fu, 0x04u, 0x04u, 0x04u, 0x04u, 0x04u, 0x04u }, /* T */
    { 0x11u, 0x11u, 0x11u, 0x11u, 0x11u, 0x11u, 0x0eu }, /* U */
    { 0x11u, 0x11u, 0x11u, 0x11u, 0x11u, 0x0au, 0x04u }, /* V */
    { 0x11u, 0x11u, 0x15u, 0x15u, 0x15u, 0x1bu, 0x11u }, /* W */
    { 0x11u, 0x0au, 0x04u, 0x04u, 0x04u, 0x0au, 0x11u }, /* X */
    { 0x11u, 0x0au, 0x04u, 0x04u, 0x04u, 0x04u, 0x04u }, /* Y */
    { 0x1fu, 0x01u, 0x02u, 0x04u, 0x08u, 0x10u, 0x1fu }, /* Z */
    { 0x0eu, 0x08u, 0x08u, 0x08u, 0x08u, 0x08u, 0x0eu }, /* [ */
    { 0x10u, 0x10u, 0x08u, 0x04u, 0x02u, 0x01u, 0x01u }, /* backslash */
    { 0x0eu, 0x02u, 0x02u, 0x02u, 0x02u, 0x02u, 0x0eu }, /* ] */
    { 0x04u, 0x0au, 0x11u, 0x00u, 0x00u, 0x00u, 0x00u }, /* ^ */
    { 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x1fu }, /* _ */
};

static const uint8_t *dov_glyph(char c)
{
    unsigned code = (unsigned char)c;

    if (code >= 'a' && code <= 'z')
        code -= 'a' - 'A';
    if (code < 0x20u || code > 0x5fu)
        code = 0x20u;
    return k_dov_font[code - 0x20u];
}

/* ---- drawing ----------------------------------------------------------- */

typedef struct dov_canvas {
    uint32_t *pixels;
    unsigned width;
    unsigned height;
    unsigned stride;
    uint32_t written;
} dov_canvas;

static void dov_fill(dov_canvas *canvas, unsigned x, unsigned y,
                     unsigned w, unsigned h, uint32_t color)
{
    unsigned row;

    if (x >= canvas->width || y >= canvas->height)
        return;
    if (w > canvas->width - x)
        w = canvas->width - x;
    if (h > canvas->height - y)
        h = canvas->height - y;
    for (row = 0u; row < h; ++row) {
        uint32_t *p = canvas->pixels + (size_t)(y + row) * canvas->stride + x;
        unsigned col;

        for (col = 0u; col < w; ++col)
            p[col] = color;
    }
    canvas->written += w * h;
}

/* Screen door, 3 of every 2x2 pixels dark: the game stays visible through
 * the fourth.  Writes only; the scanout buffer is uncached CDRAM, so a
 * read-modify-write blend would cost a CPU read per pixel. */
static void dov_stipple(dov_canvas *canvas, unsigned x, unsigned y,
                        unsigned w, unsigned h)
{
    unsigned row;

    if (x >= canvas->width || y >= canvas->height)
        return;
    if (w > canvas->width - x)
        w = canvas->width - x;
    if (h > canvas->height - y)
        h = canvas->height - y;
    for (row = 0u; row < h; ++row) {
        uint32_t *p = canvas->pixels + (size_t)(y + row) * canvas->stride;
        unsigned step = (row & 1u) ? 2u : 1u;
        unsigned col;

        for (col = x; col < x + w; col += step) {
            p[col] = DOV_BACK;
            ++canvas->written;
        }
    }
}

static void dov_glyph_draw(dov_canvas *canvas, char c, unsigned x, unsigned y,
                           unsigned scale, uint32_t color)
{
    const uint8_t *rows = dov_glyph(c);
    unsigned row;

    for (row = 0u; row < 7u; ++row) {
        unsigned column = 0u;

        while (column < 5u) {
            unsigned first;

            while (column < 5u && !(rows[row] & (uint8_t)(1u << (4u - column))))
                ++column;
            first = column;
            while (column < 5u && (rows[row] & (uint8_t)(1u << (4u - column))))
                ++column;
            if (column != first)
                dov_fill(canvas, x + first * scale, y + row * scale,
                         (column - first) * scale, scale, color);
        }
    }
}

uint32_t isaac_vita_debug_overlay_draw(
    uint32_t *framebuffer, unsigned width, unsigned height, unsigned stride,
    const isaac_vita_debug_overlay_page *page)
{
    dov_canvas canvas;
    unsigned scale;
    unsigned cell_w;
    unsigned line_h;
    unsigned pad;
    unsigned cols = 0u;
    unsigned lines;
    unsigned panel_w;
    unsigned panel_h;
    unsigned x0;
    unsigned y0;
    unsigned i;

    if (!framebuffer || !page || stride < width || page->line_count == 0u)
        return 0u;
    canvas.pixels = framebuffer;
    canvas.width = width;
    canvas.height = height;
    canvas.stride = stride;
    canvas.written = 0u;
    /* vitaGL's own debugger doubles its font from 720 pixels up. */
    scale = width >= 720u ? 2u : 1u;
    lines = page->line_count;
    if (lines > ISAAC_VITA_DEBUG_OVERLAY_LINES)
        lines = ISAAC_VITA_DEBUG_OVERLAY_LINES;
    for (i = 0u; i < lines; ++i) {
        unsigned n = page->length[i];

        if (n > ISAAC_VITA_DEBUG_OVERLAY_COLS)
            n = ISAAC_VITA_DEBUG_OVERLAY_COLS;
        while (n > 0u && page->text[i][n - 1u] == ' ')
            --n;
        if (n > cols)
            cols = n;
    }
    if (cols == 0u)
        return 0u;
    for (;;) {
        cell_w = 6u * scale;
        line_h = 8u * scale;
        pad = 3u * scale;
        panel_w = cols * cell_w - scale + 2u * pad;
        panel_h = lines * line_h - scale + 2u * pad;
        /* Top-right corner, clear of the two black border rows above the
         * guest's 960x540 viewport; a full table that does not fit a scaled
         * raster at the doubled font falls back to the 1x font. */
        if (panel_w + 2u * scale <= width &&
                panel_h + 2u + 2u * scale <= height)
            break;
        if (scale == 1u)
            return 0u;
        scale = 1u;
    }
    x0 = width - 2u * scale - panel_w;
    y0 = 2u + 2u * scale;
    dov_stipple(&canvas, x0, y0, panel_w, panel_h);
    for (i = 0u; i < lines; ++i) {
        unsigned n = page->length[i];
        uint8_t color_index = page->color[i];
        uint32_t color;
        unsigned j;

        if (n > ISAAC_VITA_DEBUG_OVERLAY_COLS)
            n = ISAAC_VITA_DEBUG_OVERLAY_COLS;
        if (color_index >= ISAAC_VITA_DEBUG_OVERLAY_COLOR_COUNT)
            color_index = ISAAC_VITA_DEBUG_OVERLAY_COLOR_TEXT;
        color = k_dov_colors[color_index];
        for (j = 0u; j < n; ++j) {
            unsigned gx = x0 + pad + j * cell_w;
            unsigned gy = y0 + pad + i * line_h;
            char c = page->text[i][j];

            if (c == ' ')
                continue;
            dov_glyph_draw(&canvas, c, gx + scale, gy + scale, scale,
                           DOV_SHADOW);
            dov_glyph_draw(&canvas, c, gx, gy, scale, color);
        }
    }
    return canvas.written;
}

/* ---- display callback (vitaGL display-queue thread) -------------------- */

static void dov_display_callback(void *framebuffer)
{
    uint32_t index = __atomic_load_n(&s_published, __ATOMIC_ACQUIRE);
    uint64_t begin;
    uint32_t spent;
    uint32_t max;

    if (index >= 3u || !framebuffer)
        return;
    begin = dov_now();
    (void)isaac_vita_debug_overlay_draw((uint32_t *)framebuffer,
                                        DOV_DISPLAY_WIDTH, DOV_DISPLAY_HEIGHT,
                                        DOV_DISPLAY_STRIDE, &s_pages[index]);
    spent = (uint32_t)(dov_now() - begin);
    __atomic_fetch_add(&s_draw_sum_us, spent, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_draw_count, 1u, __ATOMIC_RELAXED);
    max = __atomic_load_n(&s_draw_max_us, __ATOMIC_RELAXED);
    if (spent > max)
        __atomic_store_n(&s_draw_max_us, spent, __ATOMIC_RELAXED);
}

/* ---- sampling (game thread) -------------------------------------------- */

static void dov_sort(uint32_t *values, unsigned count)
{
    unsigned i;

    for (i = 1u; i < count; ++i) {
        uint32_t v = values[i];
        unsigned j = i;

        while (j > 0u && values[j - 1u] > v) {
            values[j] = values[j - 1u];
            --j;
        }
        values[j] = v;
    }
}

/* Nearest rank: the smallest value with at least q percent at or below. */
static uint32_t dov_rank(const uint32_t *sorted, unsigned count, unsigned q)
{
    unsigned rank;

    if (count == 0u)
        return 0u;
    rank = (q * count + 99u) / 100u;
    if (rank == 0u)
        rank = 1u;
    return sorted[rank - 1u];
}

static void dov_frame_stats(uint64_t now)
{
    uint32_t frames[DOV_FRAME_RING];
    uint32_t swaps[DOV_FRAME_RING];
    uint32_t updates[DOV_FRAME_RING];
    uint32_t renders[DOV_FRAME_RING];
    unsigned presents = 0u;
    unsigned intervals = 0u;
    unsigned i;

    for (i = 0u; i < s_frame_count && i < DOV_FRAME_RING; ++i) {
        const dov_frame *f =
            &s_frames[(s_frame_head + DOV_FRAME_RING - 1u - i) % DOV_FRAME_RING];

        if (now - f->at_us >= DOV_WINDOW_US)
            break;
        swaps[presents] = f->swap_us;
        updates[presents] = f->update_us;
        renders[presents] = f->render_us;
        ++presents;
        if (f->frame_us)
            frames[intervals++] = f->frame_us;
    }
    s_model.fps_presents = presents;
    s_model.have_frames = presents != 0u;
    dov_sort(frames, intervals);
    dov_sort(swaps, presents);
    dov_sort(updates, presents);
    dov_sort(renders, presents);
    s_model.frame_p50_us = dov_rank(frames, intervals, 50u);
    s_model.frame_p95_us = dov_rank(frames, intervals, 95u);
    s_model.frame_max_us = intervals ? frames[intervals - 1u] : 0u;
    s_model.swap_p50_us = dov_rank(swaps, presents, 50u);
    s_model.swap_max_us = presents ? swaps[presents - 1u] : 0u;
    s_model.update_p50_us = dov_rank(updates, presents, 50u);
    s_model.update_max_us = presents ? updates[presents - 1u] : 0u;
    s_model.render_p50_us = dov_rank(renders, presents, 50u);
    s_model.render_max_us = presents ? renders[presents - 1u] : 0u;
#if defined(ISAAC_VITA_DEBUG_OVERLAY_SCHEDULER)
    s_model.have_scheduler = 1u;
#endif
}

/* Core load = 100 - idle share over the last ~1 s, from the per-CPU idle
 * clocks of sceKernelGetSystemInfo (PSVshell's method; it is system-wide:
 * other processes on a core count as load). */
static void dov_cpu_sample(uint64_t now)
{
    SceKernelSystemInfo info;
    unsigned slot = s_cpu_count % DOV_CPU_RING;
    unsigned oldest;
    unsigned core;
    uint64_t wall;

    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    if (sceKernelGetSystemInfo(&info) < 0) {
        s_model.have_cpu = 0u;
        return;
    }
    s_cpu_at[slot] = now;
    for (core = 0u; core < DOV_CPU_CORES; ++core)
        s_cpu_idle[slot][core] = (uint64_t)info.cpuInfo[core].idleClock;
    ++s_cpu_count;
    if (s_cpu_count < 2u) {
        s_model.have_cpu = 0u;
        return;
    }
    oldest = s_cpu_count > DOV_CPU_RING
        ? s_cpu_count % DOV_CPU_RING : 0u;
    wall = now - s_cpu_at[oldest];
    if (wall == 0u) {
        s_model.have_cpu = 0u;
        return;
    }
    for (core = 0u; core < DOV_CPU_CORES; ++core) {
        uint64_t idle = s_cpu_idle[slot][core] - s_cpu_idle[oldest][core];
        uint64_t idle_pct = idle * 100u / wall;

        s_model.cpu_load[core] = (uint8_t)(idle_pct >= 100u ? 0u : 100u - idle_pct);
    }
    s_model.have_cpu = 1u;
}

static void dov_add_row(const char *label, uint8_t domain, uint8_t flags,
                        uint16_t count, uint32_t used, uint32_t resv,
                        uint32_t peak)
{
    isaac_vita_debug_overlay_row *row;
    unsigned i = 0u;

    if (s_model.row_count >= ISAAC_VITA_DEBUG_OVERLAY_MAX_ROWS)
        return;
    row = &s_model.rows[s_model.row_count++];
    memset(row, 0, sizeof *row);
    while (label[i] != '\0' && i + 1u < ISAAC_VITA_DEBUG_OVERLAY_LABEL_BYTES) {
        row->label[i] = label[i];
        ++i;
    }
    row->domain = domain;
    row->flags = flags;
    row->count = count;
    row->used = used;
    row->resv = resv;
    row->peak = peak;
}

static void dov_upper_label(char *out, const char *name, unsigned cap)
{
    unsigned i = 0u;

    while (name[i] != '\0' && i + 1u < cap) {
        char c = name[i];

        out[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
        ++i;
    }
    out[i] = '\0';
}

static uint32_t dov_sampled_peak(unsigned source, uint32_t used)
{
    if (source < DOV_ARRAY_COUNT(s_peak_sampled) && used > s_peak_sampled[source])
        s_peak_sampled[source] = used;
    return source < DOV_ARRAY_COUNT(s_peak_sampled) ? s_peak_sampled[source] : used;
}

static vglMemType dov_vgl_type(unsigned source)
{
    switch (source) {
    case DOV_USED_VGL_VRAM: return VGL_MEM_VRAM;
    case DOV_USED_VGL_PHYCONT: return DOV_VGL_MEM_PHYCONT;
    case DOV_USED_VGL_BUDGET: return VGL_MEM_BUDGET;
    case DOV_USED_VGL_RAM:
    default: return VGL_MEM_RAM;
    }
}

#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
/* vitaGL's allocator is single-threaded (SINGLE_THREADED_GC): its statistics
 * are read only on the GL render thread, at PRESENT, when this sampler asked
 * for them; the sampler uses the last published values (<= 1 frame old). */
static const vglMemType s_dov_vgl_types[4] = {
    VGL_MEM_RAM, VGL_MEM_VRAM, DOV_VGL_MEM_PHYCONT, VGL_MEM_BUDGET };
static volatile uint32_t s_dov_vgl_request;
static volatile uint32_t s_dov_vgl_total[4], s_dov_vgl_free[4];
static unsigned dov_vgl_index(unsigned source)
{
    switch (source) {
    case DOV_USED_VGL_VRAM: return 1u;
    case DOV_USED_VGL_PHYCONT: return 2u;
    case DOV_USED_VGL_BUDGET: return 3u;
    case DOV_USED_VGL_RAM:
    default: return 0u;
    }
}
#endif
/* Fill used/peak of a known owner; returns the row flags. */
static uint8_t dov_owner_usage(unsigned source, uint32_t *used, uint32_t *peak)
{
    *used = 0u;
    *peak = 0u;
    switch (source) {
    case DOV_USED_NEWLIB: {
        /* newlib: walks the free bins under its own lock; 1 Hz. */
        struct mallinfo info = mallinfo();

        *used = (uint32_t)info.uordblks;
        *peak = dov_sampled_peak(source, *used);
        return ISAAC_VITA_DEBUG_OVERLAY_ROW_USED |
               ISAAC_VITA_DEBUG_OVERLAY_ROW_PEAK |
               ISAAC_VITA_DEBUG_OVERLAY_ROW_PEAK_SAMPLED;
    }
    case DOV_USED_VGL_RAM:
    case DOV_USED_VGL_VRAM:
    case DOV_USED_VGL_PHYCONT:
    case DOV_USED_VGL_BUDGET: {
#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
        unsigned index = dov_vgl_index(source);
        size_t total, free_bytes;
        (void)dov_vgl_type(source); /* same source mapping as the direct path */
        __atomic_store_n(&s_dov_vgl_request, 1u, __ATOMIC_RELEASE);
        total = __atomic_load_n(&s_dov_vgl_total[index], __ATOMIC_ACQUIRE);
        free_bytes = __atomic_load_n(&s_dov_vgl_free[index], __ATOMIC_ACQUIRE);
#else
        vglMemType type = dov_vgl_type(source);
        size_t total = vglMemTotal(type);
        size_t free_bytes = vglMemFree(type);
#endif

        *used = total > free_bytes ? (uint32_t)(total - free_bytes) : 0u;
        *peak = dov_sampled_peak(source, *used);
        return ISAAC_VITA_DEBUG_OVERLAY_ROW_USED |
               ISAAC_VITA_DEBUG_OVERLAY_ROW_PEAK |
               ISAAC_VITA_DEBUG_OVERLAY_ROW_PEAK_SAMPLED |
               ISAAC_VITA_DEBUG_OVERLAY_ROW_VGL;
    }
#if defined(ISAAC_VITA_DEBUG_OVERLAY_LUA_ARENA)
    case DOV_USED_LUA: {
        uint32_t live_kb = 0u;
        uint32_t peak_kb = 0u;
        uint32_t fallbacks = 0u;

        isaac_vita_lua_arena_stats(&live_kb, &peak_kb, &fallbacks);
        *used = live_kb * 1024u;
        *peak = peak_kb * 1024u;
        return ISAAC_VITA_DEBUG_OVERLAY_ROW_USED |
               ISAAC_VITA_DEBUG_OVERLAY_ROW_PEAK;
    }
#endif
#if defined(ISAAC_VITA_DEBUG_OVERLAY_HEAP_TELEMETRY)
    case DOV_USED_OVERFLOW: {
        isaac_vita_guest_heap_telemetry_snapshot snapshot;

        memset(&snapshot, 0, sizeof snapshot);
        if (!isaac_vita_guest_heap_telemetry_snapshot_get(&snapshot))
            return 0u;
        *used = snapshot.owned_requested_bytes;
        *peak = snapshot.peak_requested_bytes;
        return ISAAC_VITA_DEBUG_OVERLAY_ROW_USED |
               ISAAC_VITA_DEBUG_OVERLAY_ROW_PEAK;
    }
#endif
#if defined(ISAAC_VITA_DEBUG_OVERLAY_OPENAL_POOL)
    case DOV_USED_OPENAL: {
        isaac_vita_openal_pool_snapshot snapshot;

        memset(&snapshot, 0, sizeof snapshot);
        if (!isaac_vita_openal_pool_snapshot_get(&snapshot))
            return 0u;
        *used = snapshot.live_requested_bytes;
        *peak = snapshot.peak_requested_bytes;
        return ISAAC_VITA_DEBUG_OVERLAY_ROW_USED |
               ISAAC_VITA_DEBUG_OVERLAY_ROW_PEAK;
    }
#endif
    default:
        return 0u;
    }
}

/* Once: the eboot's own segments and the main thread's stack, both mapped
 * before crt0 and therefore part of BASE rather than of the boot probe. */
static void dov_static_figures(void)
{
    SceKernelModuleInfo info;
    SceKernelThreadInfo thread;
    SceUID module;
    unsigned i;

    if (s_eboot_result != 0)
        return;
    s_eboot_result = -1;
    module = sceKernelGetModuleIdByAddr((void *)&isaac_vita_debug_overlay_set_mode);
    if (module >= 0) {
        memset(&info, 0, sizeof info);
        info.size = sizeof info;
        if (sceKernelGetModuleInfo(module, &info) >= 0) {
            s_eboot_result = 1;
            for (i = 0u; i < 4u; ++i) {
                /* Low byte of perms: SCE_KERNEL_MEMORY_ACCESS_W = 0x02. */
                if (info.segments[i].memsz == 0u)
                    continue;
                if (info.segments[i].perms & 0x02u)
                    s_eboot_rw += (uint32_t)info.segments[i].memsz;
                else
                    s_eboot_rx += (uint32_t)info.segments[i].memsz;
            }
        }
    }
    if (__atomic_load_n(&s_boot.state, __ATOMIC_ACQUIRE) == 2u) {
        memset(&thread, 0, sizeof thread);
        thread.size = sizeof thread;
        if (sceKernelGetThreadInfo((SceUID)s_boot.main_thid, &thread) >= 0)
            s_main_stack = (uint32_t)thread.stackSize;
    }
}

static void dov_thread_name(char *out, const char *name)
{
    const char *skip = name;
    unsigned i = 0u;

    if (strncmp(name, "isaac_", 6u) == 0)
        skip = name + 6;
    while (skip[i] != '\0' && i < 7u) {
        char c = skip[i];

        out[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
        ++i;
    }
    out[i] = '\0';
}

/* Thread stacks (USER rows) and per-thread CPU over the last memory period. */
static void dov_thread_sample(uint64_t now, uint32_t *stack_sum,
                              uint16_t *stack_count)
{
    dov_thread snapshot[DOV_THREAD_SLOTS];
    dov_thread_view views[DOV_THREAD_SLOTS + 1u];
    unsigned view_count = 0u;
    uint64_t wall = s_thread_prev_count ? now - s_thread_prev_at : 0u;
    unsigned i;
    unsigned j;
    uint8_t loads[DOV_THREAD_SLOTS + 1u];
    int32_t game = s_game_thid ? s_game_thid : s_boot.main_thid;

    dov_lock();
    memcpy(snapshot, s_threads, sizeof snapshot);
    dov_unlock();
    *stack_sum = 0u;
    *stack_count = 0u;
    s_model.have_game_thread = 0u;
    s_model.top_thread_count = 0u;
    for (i = 0u; i <= DOV_THREAD_SLOTS; ++i) {
        SceKernelThreadInfo info;
        int32_t thid;
        const char *name;

        if (i < DOV_THREAD_SLOTS) {
            if (!snapshot[i].live)
                continue;
            thid = snapshot[i].thid;
            name = snapshot[i].name;
        } else {
            /* the main thread: its stack is a row of its own */
            if (__atomic_load_n(&s_boot.state, __ATOMIC_ACQUIRE) != 2u)
                continue;
            thid = s_boot.main_thid;
            name = "main";
        }
        memset(&info, 0, sizeof info);
        info.size = sizeof info;
        if (sceKernelGetThreadInfo((SceUID)thid, &info) < 0)
            continue;
        views[view_count].thid = thid;
        views[view_count].stack_bytes = (uint32_t)info.stackSize;
        views[view_count].run_us = (uint64_t)info.runClocks;
        dov_copy_name(views[view_count].name, name);
        if (i < DOV_THREAD_SLOTS) {
            *stack_sum += (uint32_t)info.stackSize;
            ++*stack_count;
        }
        loads[view_count] = 0u;
        for (j = 0u; wall && j < s_thread_prev_count; ++j) {
            if (s_thread_prev[j].thid == thid &&
                    views[view_count].run_us >= s_thread_prev[j].run_us) {
                uint64_t pct = (views[view_count].run_us -
                                s_thread_prev[j].run_us) * 100u / wall;

                loads[view_count] = (uint8_t)(pct > 100u ? 100u : pct);
                if (thid == game) {
                    s_model.have_game_thread = 1u;
                    s_model.game_thread_load = loads[view_count];
                }
                break;
            }
        }
        ++view_count;
    }
    /* Top three other threads by load. */
    for (j = 0u; wall && j < 3u; ++j) {
        int best = -1;

        for (i = 0u; i < view_count; ++i) {
            if (views[i].thid == game || loads[i] == 0u)
                continue;
            if (best < 0 || loads[i] > loads[best])
                best = (int)i;
        }
        if (best < 0)
            break;
        dov_thread_name(s_model.top_thread_name[j], views[best].name);
        s_model.top_thread_load[j] = loads[best];
        loads[best] = 0u; /* consumed */
        s_model.top_thread_count = (uint8_t)(j + 1u);
    }
    memcpy(s_thread_prev, views, sizeof views[0] * view_count);
    s_thread_prev_count = view_count;
    s_thread_prev_at = now;
}

static void dov_free_sample(void)
{
    SceKernelFreeMemorySizeInfo info;

    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    s_model.have_free = sceKernelGetFreeMemorySize(&info) >= 0;
    s_model.free_bytes[ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_USER] =
        (uint32_t)info.size_user;
    s_model.free_bytes[ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_CDRAM] =
        (uint32_t)info.size_cdram;
    s_model.free_bytes[ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_PHYCONT] =
        (uint32_t)info.size_phycont;
}

static void dov_memory_sample(uint64_t now)
{
    dov_group groups[DOV_GROUP_SLOTS];
    uint32_t dropped;
    uint32_t stack_sum = 0u;
    uint16_t stack_count = 0u;
    unsigned i;
    unsigned k;

    dov_static_figures();
    dov_lock();
    memcpy(groups, s_groups, sizeof groups);
    dropped = s_ledger_dropped;
    dov_unlock();

    s_model.row_count = 0u;
    s_model.ledger_dropped = (uint8_t)(dropped > 255u ? 255u : dropped);
    if (s_eboot_result > 0) {
        dov_add_row("EBOOT CODE", ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_USER, 0u, 1u,
                    0u, s_eboot_rx, 0u);
        dov_add_row("EBOOT DATA", ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_USER, 0u, 1u,
                    0u, s_eboot_rw, 0u);
    }
    if (s_main_stack)
        dov_add_row("MAIN STACK", ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_USER, 0u, 1u,
                    0u, s_main_stack, 0u);
    dov_thread_sample(now, &stack_sum, &stack_count);
    if (stack_count)
        dov_add_row("THREAD STACKS", ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_USER, 0u,
                    stack_count, 0u, stack_sum, 0u);
    for (i = 0u; i < DOV_GROUP_SLOTS; ++i) {
        const struct dov_known *known = NULL;
        char label[ISAAC_VITA_DEBUG_OVERLAY_LABEL_BYTES];
        uint32_t used = 0u;
        uint32_t peak = groups[i].peak_bytes;
        uint8_t flags = ISAAC_VITA_DEBUG_OVERLAY_ROW_PEAK;

        if (!groups[i].used || groups[i].live_count == 0u)
            continue;
        for (k = 0u; k < DOV_ARRAY_COUNT(k_dov_known); ++k) {
            if (strncmp(groups[i].name, k_dov_known[k].name,
                        DOV_NAME_BYTES) == 0) {
                known = &k_dov_known[k];
                break;
            }
        }
        if (known) {
            uint32_t owner_used;
            uint32_t owner_peak;
            uint8_t owner_flags = dov_owner_usage(known->source, &owner_used,
                                                  &owner_peak);

            dov_upper_label(label, known->label, DOV_LABEL_COLS + 1u);
            if (owner_flags & ISAAC_VITA_DEBUG_OVERLAY_ROW_USED) {
                flags = owner_flags;
                used = owner_used;
                peak = owner_peak;
            } else {
                flags |= owner_flags & ISAAC_VITA_DEBUG_OVERLAY_ROW_VGL;
            }
        } else {
            dov_upper_label(label, groups[i].name, DOV_LABEL_COLS + 1u);
        }
        dov_add_row(label, groups[i].domain, flags, groups[i].live_count,
                    used, groups[i].live_bytes, peak);
    }

    dov_free_sample();

    s_model.have_base = 0u;
    if (__atomic_load_n(&s_boot.state, __ATOMIC_ACQUIRE) == 2u &&
            s_boot.result >= 0) {
        s_model.base_bytes[ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_USER] =
            s_boot.free_bytes[ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_USER] +
            s_eboot_rx + s_eboot_rw + s_main_stack;
        s_model.base_bytes[ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_CDRAM] =
            s_boot.free_bytes[ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_CDRAM];
        s_model.base_bytes[ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_PHYCONT] =
            s_boot.free_bytes[ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_PHYCONT];
        s_model.have_base = 1u;
    }
    s_model.have_memory = 1u;
    ++s_memory_samples;
}

static void dov_log_model(void)
{
    uint32_t i;
    unsigned domain;

    isaac_vita_log(
        "KAGE VITA DEBUG OVERLAY MEM: sample=%u rows=%u dropped=%u "
        "free(u/c/p)=%u/%u/%u base(u/c/p)=%u/%u/%u base_ok=%u",
        (unsigned)s_memory_samples, (unsigned)s_model.row_count,
        (unsigned)s_model.ledger_dropped,
        (unsigned)s_model.free_bytes[0], (unsigned)s_model.free_bytes[1],
        (unsigned)s_model.free_bytes[2], (unsigned)s_model.base_bytes[0],
        (unsigned)s_model.base_bytes[1], (unsigned)s_model.base_bytes[2],
        (unsigned)s_model.have_base);
    for (i = 0u; i < s_model.row_count; ++i) {
        const isaac_vita_debug_overlay_row *row = &s_model.rows[i];

        isaac_vita_log(
            "KAGE VITA DEBUG OVERLAY ROW: d=%u n=%u used=%u resv=%u peak=%u "
            "flags=0x%02x label=%.15s",
            (unsigned)row->domain, (unsigned)row->count, (unsigned)row->used,
            (unsigned)row->resv, (unsigned)row->peak, (unsigned)row->flags,
            row->label);
    }
    for (domain = 0u; domain < ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_RECONCILED;
         ++domain) {
        int64_t unacc = isaac_vita_debug_overlay_unaccounted(&s_model, domain);

        isaac_vita_log(
            "KAGE VITA DEBUG OVERLAY SUM: d=%u rows=%u free=%u base=%u "
            "unacc=%s%u",
            domain,
            (unsigned)isaac_vita_debug_overlay_domain_rows(&s_model, domain),
            (unsigned)s_model.free_bytes[domain],
            (unsigned)s_model.base_bytes[domain], unacc < 0 ? "-" : "",
            (unsigned)(unacc < 0 ? -unacc : unacc));
    }
}

static void dov_publish(const isaac_vita_debug_overlay_page *page)
{
    uint32_t current = __atomic_load_n(&s_published, __ATOMIC_ACQUIRE);
    uint32_t target;

    if (current < 3u && memcmp(&s_pages[current], page, sizeof *page) == 0)
        return; /* unchanged text: the display keeps the published page */
    for (target = 0u; target < 3u; ++target) {
        if (target != current && target != s_previous)
            break;
    }
    memcpy(&s_pages[target], page, sizeof *page);
    s_previous = current;
    __atomic_store_n(&s_published, target, __ATOMIC_RELEASE);
}

static void dov_sample(uint64_t now)
{
    static isaac_vita_debug_overlay_page scratch;
    uint8_t mode = g_isaac_vita_debug_overlay_mode;
    uint32_t draw_sum;
    uint32_t draw_count;
    uint64_t done;
    uint32_t spent;
    int memory = 0;

    s_last_sample_us = now;
    ++s_samples;
    dov_frame_stats(now);
    if (mode == ISAAC_VITA_DEBUG_OVERLAY_MODE_FULL)
        dov_cpu_sample(now);
    if (!s_last_memory_us || now - s_last_memory_us >= DOV_MEMORY_US) {
        s_last_memory_us = now;
        if (mode == ISAAC_VITA_DEBUG_OVERLAY_MODE_FULL) {
            /* the table: ledger, owners (mallinfo walks newlib's bins),
             * threads, free sizes */
            dov_memory_sample(now);
            memory = 1;
        } else {
            /* compact shows the two free sizes only */
            dov_free_sample();
        }
    }
    s_model.cost_frame_avg_us = s_cost_frame_count
        ? (uint32_t)(s_cost_frame_sum_us / s_cost_frame_count) : 0u;
    s_cost_frame_sum_us = 0u;
    s_cost_frame_count = 0u;
    draw_sum = __atomic_exchange_n(&s_draw_sum_us, 0u, __ATOMIC_RELAXED);
    draw_count = __atomic_exchange_n(&s_draw_count, 0u, __ATOMIC_RELAXED);
    s_model.cost_draw_avg_us = draw_count ? draw_sum / draw_count : 0u;
    s_model.cost_draw_max_us =
        __atomic_exchange_n(&s_draw_max_us, 0u, __ATOMIC_RELAXED);
    (void)isaac_vita_debug_overlay_format(&s_model, mode, &scratch);
    dov_publish(&scratch);
    if (memory && (s_memory_samples == 1u || now - s_last_log_us >= DOV_LOG_US)) {
        s_last_log_us = now;
        dov_log_model();
    }
    done = dov_now();
    spent = done >= now ? (uint32_t)(done - now) : 0u;
    s_model.cost_sample_us = spent;
    if (spent > s_cost_sample_max_us)
        s_cost_sample_max_us = spent;
    s_model.cost_sample_max_us = s_cost_sample_max_us;
}

/* ---- public entry points ----------------------------------------------- */

const char *isaac_vita_debug_overlay_mode_name(uint8_t mode)
{
    switch (mode) {
    case ISAAC_VITA_DEBUG_OVERLAY_MODE_COMPACT: return "compact";
    case ISAAC_VITA_DEBUG_OVERLAY_MODE_FULL: return "full";
    default: return "off";
    }
}

void isaac_vita_debug_overlay_set_mode(uint8_t mode)
{
    if (mode > ISAAC_VITA_DEBUG_OVERLAY_MODE_FULL)
        mode = ISAAC_VITA_DEBUG_OVERLAY_MODE_OFF;
    g_isaac_vita_debug_overlay_mode = mode;
    isaac_vita_log(
        "KAGE VITA DEBUG OVERLAY: mode=%s boot=%u rc=0x%08x "
        "free(u/c/p)=%u/%u/%u ledger(allocs,frees,dropped)=%u,%u,%u "
        "raster=%ux%u stride=%u",
        isaac_vita_debug_overlay_mode_name(mode),
        (unsigned)__atomic_load_n(&s_boot.state, __ATOMIC_ACQUIRE),
        (unsigned)s_boot.result,
        (unsigned)s_boot.free_bytes[0], (unsigned)s_boot.free_bytes[1],
        (unsigned)s_boot.free_bytes[2], (unsigned)s_ledger_allocs,
        (unsigned)s_ledger_frees, (unsigned)s_ledger_dropped,
        DOV_DISPLAY_WIDTH, DOV_DISPLAY_HEIGHT, DOV_DISPLAY_STRIDE);
}

void isaac_vita_debug_overlay_present_enter(void)
{
    s_swap_enter_us = dov_now();
    /* Re-assert every present: the loading presenter owns the callback while
     * it runs and clears it when it finishes (kage_vita_backend_present calls
     * kage_vita_loading_finish before this hook).  vitaGL stores a pointer. */
#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
    /* GL render thread: kage's present closure calls the render hook. */
#else
    vglSetDisplayCallback(dov_display_callback);
#endif
}

#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
void isaac_vita_debug_overlay_present_render(void)
{
    vglSetDisplayCallback(dov_display_callback);
    if (__atomic_exchange_n(&s_dov_vgl_request, 0u, __ATOMIC_ACQ_REL)) {
        unsigned i;
        for (i = 0u; i < 4u; ++i) {
            __atomic_store_n(&s_dov_vgl_total[i], (uint32_t)vglMemTotal(s_dov_vgl_types[i]),
                             __ATOMIC_RELEASE);
            __atomic_store_n(&s_dov_vgl_free[i], (uint32_t)vglMemFree(s_dov_vgl_types[i]),
                             __ATOMIC_RELEASE);
        }
    }
}
#endif
void isaac_vita_debug_overlay_present_return(void)
{
    uint64_t now = dov_now();
    dov_frame *frame = &s_frames[s_frame_head];
    uint32_t update_us = 0u;
    uint32_t render_us = 0u;

    if (!s_game_thid)
        s_game_thid = (int32_t)sceKernelGetThreadId();
#if defined(ISAAC_VITA_DEBUG_OVERLAY_SCHEDULER)
    kage_vita_fullspeed_scheduler_debug_overlay_timings(&update_us, &render_us);
#endif
    frame->at_us = now;
    frame->swap_us = now >= s_swap_enter_us ? (uint32_t)(now - s_swap_enter_us) : 0u;
    frame->frame_us = s_last_return_us && now > s_last_return_us
        ? (uint32_t)(now - s_last_return_us) : 0u;
    frame->update_us = update_us;
    frame->render_us = render_us;
    s_frame_head = (s_frame_head + 1u) % DOV_FRAME_RING;
    if (s_frame_count < DOV_FRAME_RING)
        ++s_frame_count;
    s_last_return_us = now;
    if (!s_last_sample_us || now - s_last_sample_us >= DOV_SAMPLE_US) {
        dov_sample(now);
    } else {
        uint64_t done = dov_now();

        s_cost_frame_sum_us += done >= now ? done - now : 0u;
        ++s_cost_frame_count;
    }
}

#if defined(ISAAC_VITA_DEBUG_OVERLAY_ORACLE)
void isaac_vita_debug_overlay_test_reset(void)
{
    memset(&s_model, 0, sizeof s_model);
    memset(s_pages, 0, sizeof s_pages);
    s_published = 0xffu;
    s_previous = 0xffu;
    memset(s_frames, 0, sizeof s_frames);
    s_frame_head = 0u;
    s_frame_count = 0u;
    s_swap_enter_us = 0u;
    s_last_return_us = 0u;
    s_game_thid = 0;
    s_last_sample_us = 0u;
    s_last_memory_us = 0u;
    s_last_log_us = 0u;
    s_samples = 0u;
    s_memory_samples = 0u;
    s_cpu_count = 0u;
    s_thread_prev_count = 0u;
    s_thread_prev_at = 0u;
    memset(s_peak_sampled, 0, sizeof s_peak_sampled);
    s_cost_frame_sum_us = 0u;
    s_cost_frame_count = 0u;
    s_cost_sample_max_us = 0u;
    s_draw_sum_us = 0u;
    s_draw_count = 0u;
    s_draw_max_us = 0u;
    s_eboot_rx = 0u;
    s_eboot_rw = 0u;
    s_main_stack = 0u;
    s_eboot_result = 0;
    memset(s_blocks, 0, sizeof s_blocks);
    memset(s_groups, 0, sizeof s_groups);
    memset(s_threads, 0, sizeof s_threads);
    s_ledger_dropped = 0u;
    s_ledger_allocs = 0u;
    s_ledger_frees = 0u;
    memset(&s_boot, 0, sizeof s_boot);
    g_isaac_vita_debug_overlay_mode = 0u;
}

void isaac_vita_debug_overlay_test_sample(void)
{
    dov_sample(dov_now());
}

const isaac_vita_debug_overlay_model *isaac_vita_debug_overlay_test_model(void)
{
    return &s_model;
}

const isaac_vita_debug_overlay_page *isaac_vita_debug_overlay_test_page(void)
{
    uint32_t index = __atomic_load_n(&s_published, __ATOMIC_ACQUIRE);

    return index < 3u ? &s_pages[index] : NULL;
}

void isaac_vita_debug_overlay_test_display(void *framebuffer)
{
    dov_display_callback(framebuffer);
}
#endif

#endif /* ISAAC_VITA_DEBUG_OVERLAY */
