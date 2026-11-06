/* Host oracle for host_vita_debug_overlay.c (ISAAC_VITA_DEBUG_OVERLAY).
 *
 * A fake kernel keeps the free sizes honest: every wrapped memblock and
 * thread stack is taken out of its domain's free counter, so the boot probe,
 * the ledger and the reconciliation are exercised end to end.  Checks:
 *   - MiB/KiB formatting (rounding, ceiling below 1 MiB, width, >= 1000 MiB);
 *   - boot probe taken before the first real allocation, ledger add/free/
 *     peak, kuKernelAllocMemBlock, table overflow -> "not booked" warning,
 *     thread create/delete/exit-delete;
 *   - full model: owner "used" figures, sampled vs owner peaks, frame
 *     p50/p95/max, update/render/swap, per-core load, game-thread load, top
 *     threads; per domain BASE == rows + FREE + UNACCOUNTED with the injected
 *     out-of-ledger consumption showing up exactly as UNACCOUNTED;
 *   - compact and full page goldens, every line <= 40 columns, <= 31 lines;
 *   - drawing at 960x544/960, 720x408/768 and 480x272/512 with redzones
 *     around the buffer and in the stride padding, nothing outside the panel;
 *   - off path: mode off -> the present macros never call in, no callback,
 *     no page, the display callback draws nothing;
 *   - host cost of format and draw (printed, informational).
 * With an output directory argument it also writes raw RGBA mock-ups of the
 * compact and full modes over a synthetic background (test_vita_debug_
 * overlay.py turns them into PNGs). */
#define _CRT_SECURE_NO_WARNINGS 1
/* host_vita_heap.h declares the telemetry snapshot under the pool macro. */
#define ISAAC_VITA_HEAP_OVERFLOW_MSPACE 1
#include "host_vita_debug_overlay.h"
#include "host_vita_debug_overlay_test_platform.h"
#include "host_vita_heap.h"
#include "host_vita_openal_pool.h"
#include "kage_vita_fullspeed_scheduler.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MIB 1048576u
#define KIB 1024u
#define REDZONE 0x6a17c35du
#define MAIN_THID 0x40010003

static unsigned s_checks;
static unsigned s_failures;

static void check(int condition, const char *what)
{
    ++s_checks;
    if (!condition) {
        ++s_failures;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}

static void check_u(uint64_t got, uint64_t expected, const char *what)
{
    ++s_checks;
    if (got != expected) {
        ++s_failures;
        fprintf(stderr, "FAIL: %s: got %llu expected %llu\n", what,
                (unsigned long long)got, (unsigned long long)expected);
    }
}

static void check_s(int64_t got, int64_t expected, const char *what)
{
    ++s_checks;
    if (got != expected) {
        ++s_failures;
        fprintf(stderr, "FAIL: %s: got %lld expected %lld\n", what,
                (long long)got, (long long)expected);
    }
}

static void check_str(const char *got, const char *expected, const char *what)
{
    ++s_checks;
    if (strcmp(got, expected) != 0) {
        ++s_failures;
        fprintf(stderr, "FAIL: %s:\n  got      \"%s\"\n  expected \"%s\"\n",
                what, got, expected);
    }
}

/* ---- fake kernel ------------------------------------------------------- */

static uint64_t s_now = 10000000u;
static int s_current_thid = MAIN_THID;
static int64_t s_free[3];
static int s_free_rc;
static int s_free_queries;
static int s_free_queries_at_first_real;
static int s_real_allocs;

typedef struct fake_block { int live; int uid; uint32_t type; uint32_t bytes; }
    fake_block;
static fake_block s_fake_blocks[256];
static int s_next_uid = 0x100;

typedef struct fake_thread {
    int live;
    int thid;
    uint32_t stack;
    unsigned load_pct;
} fake_thread;
static fake_thread s_fake_threads[48];
static int s_next_thid = 0x40010100;
static unsigned s_main_load = 78u;
static unsigned s_idle_pct[3] = { 15u, 88u, 70u };
static int s_sysinfo_rc;

static size_t s_vgl_total[4];
static size_t s_vgl_free[4];
static size_t s_uordblks;
static uint32_t s_lua_live_kb, s_lua_peak_kb;
static uint32_t s_ovf_owned, s_ovf_peak;
static uint32_t s_oal_live, s_oal_peak;
static uint32_t s_sched_update, s_sched_render;

static void (*s_callback)(void *);
static unsigned s_callback_sets;

static char s_logs[256][400];
static unsigned s_log_count;

static unsigned domain_of(uint32_t type)
{
    if ((type >> 24) == 0x09u)
        return 1u;
    if (type == 0x0C80D060u || type == 0x0D808060u)
        return 2u;
    if (type == 0x0CA0D060u || type == 0x0CA08060u)
        return 3u;
    return 0u;
}

uint64_t sceKernelGetProcessTimeWide(void) { return s_now; }
int sceKernelDelayThread(SceUInt32 delay) { (void)delay; return 0; }

int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *info)
{
    ++s_free_queries;
    if (s_free_rc < 0)
        return s_free_rc;
    info->size_user = (int)s_free[0];
    info->size_cdram = (int)s_free[1];
    info->size_phycont = (int)s_free[2];
    return 0;
}

static fake_block *find_block(int uid)
{
    unsigned i;

    for (i = 0u; i < sizeof s_fake_blocks / sizeof s_fake_blocks[0]; ++i)
        if (s_fake_blocks[i].live && s_fake_blocks[i].uid == uid)
            return &s_fake_blocks[i];
    return NULL;
}

int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    if (!find_block(uid))
        return -1;
    *base = (void *)(uintptr_t)(0x81000000u + (uint32_t)uid * 0x1000u);
    return 0;
}

int sceKernelGetMemBlockInfoByAddr(void *base, SceKernelMemBlockInfo *info)
{
    int uid = (int)(((uintptr_t)base - 0x81000000u) / 0x1000u);
    fake_block *block = find_block(uid);

    if (!block)
        return -1;
    info->mappedBase = base;
    info->mappedSize = block->bytes;
    info->type = block->type;
    return 0;
}

int sceKernelGetThreadId(void) { return s_current_thid; }

int sceKernelGetThreadInfo(SceUID thid, SceKernelThreadInfo *info)
{
    unsigned i;

    if (thid == MAIN_THID) {
        info->stackSize = 4 * (int)MIB;
        info->runClocks = s_now * s_main_load / 100u;
        return 0;
    }
    for (i = 0u; i < sizeof s_fake_threads / sizeof s_fake_threads[0]; ++i) {
        if (s_fake_threads[i].live && s_fake_threads[i].thid == thid) {
            info->stackSize = (SceInt32)s_fake_threads[i].stack;
            info->runClocks = s_now * s_fake_threads[i].load_pct / 100u;
            return 0;
        }
    }
    return -1;
}

int sceKernelGetSystemInfo(SceKernelSystemInfo *info)
{
    unsigned c;

    if (s_sysinfo_rc < 0)
        return s_sysinfo_rc;
    for (c = 0u; c < 3u; ++c)
        info->cpuInfo[c].idleClock = s_now * s_idle_pct[c] / 100u;
    info->cpuInfo[3].idleClock = s_now;
    return 0;
}

SceUID sceKernelGetModuleIdByAddr(void *addr) { (void)addr; return 0x1234; }

int sceKernelGetModuleInfo(SceUID uid, SceKernelModuleInfo *info)
{
    if (uid != 0x1234)
        return -1;
    info->segments[0].perms = 0x05u;      /* R X */
    info->segments[0].memsz = 0x00c40000u;
    info->segments[1].perms = 0x06u;      /* R W */
    info->segments[1].memsz = 0x00310000u;
    return 0;
}

size_t vglMemFree(vglMemType type) { return type < 4 ? s_vgl_free[type] : 0u; }
size_t vglMemTotal(vglMemType type) { return type < 4 ? s_vgl_total[type] : 0u; }

void vglSetDisplayCallback(void (*callback)(void *framebuffer))
{
    s_callback = callback;
    ++s_callback_sets;
}

struct mallinfo mallinfo(void)
{
    struct mallinfo info;

    memset(&info, 0, sizeof info);
    info.uordblks = s_uordblks;
    return info;
}

void isaac_vita_log(const char *format, ...)
{
    va_list args;

    if (s_log_count >= sizeof s_logs / sizeof s_logs[0])
        return;
    va_start(args, format);
    vsnprintf(s_logs[s_log_count++], sizeof s_logs[0], format, args);
    va_end(args);
}

static int fake_alloc(const char *name, uint32_t type, uint32_t size)
{
    unsigned i;
    unsigned d = domain_of(type);

    (void)name;
    if (s_real_allocs++ == 0)
        s_free_queries_at_first_real = s_free_queries;
    if (d < 3u && s_free[d] < (int64_t)size)
        return (int)0x80020206u;
    for (i = 0u; i < sizeof s_fake_blocks / sizeof s_fake_blocks[0]; ++i) {
        if (s_fake_blocks[i].live)
            continue;
        s_fake_blocks[i].live = 1;
        s_fake_blocks[i].uid = s_next_uid++;
        s_fake_blocks[i].type = type;
        s_fake_blocks[i].bytes = size;
        if (d < 3u)
            s_free[d] -= size;
        return s_fake_blocks[i].uid;
    }
    return -1;
}

SceUID __real_sceKernelAllocMemBlock(const char *name,
                                     SceKernelMemBlockType type, SceSize size,
                                     SceKernelAllocMemBlockOpt *opt)
{
    (void)opt;
    return fake_alloc(name, type, size);
}

SceUID __real_kuKernelAllocMemBlock(const char *name,
                                    SceKernelMemBlockType type, SceSize size,
                                    void *opt)
{
    (void)opt;
    return fake_alloc(name, type, size);
}

int __real_sceKernelFreeMemBlock(SceUID uid)
{
    fake_block *block = find_block(uid);
    unsigned d;

    if (!block)
        return -1;
    d = domain_of(block->type);
    if (d < 3u)
        s_free[d] += block->bytes;
    block->live = 0;
    return 0;
}

SceUID __real_sceKernelCreateThread(const char *name,
                                    SceKernelThreadEntry entry,
                                    int init_priority, SceSize stack_size,
                                    SceUInt32 attr, int cpu_affinity_mask,
                                    const SceKernelThreadOptParam *option)
{
    unsigned i;

    (void)name; (void)entry; (void)init_priority; (void)attr;
    (void)cpu_affinity_mask; (void)option;
    for (i = 0u; i < sizeof s_fake_threads / sizeof s_fake_threads[0]; ++i) {
        if (s_fake_threads[i].live)
            continue;
        s_fake_threads[i].live = 1;
        s_fake_threads[i].thid = s_next_thid++;
        s_fake_threads[i].stack = (stack_size + 0xfffu) & ~0xfffu;
        s_fake_threads[i].load_pct = 0u;
        s_free[0] -= s_fake_threads[i].stack;
        return s_fake_threads[i].thid;
    }
    return -1;
}

static fake_thread *find_thread(int thid)
{
    unsigned i;

    for (i = 0u; i < sizeof s_fake_threads / sizeof s_fake_threads[0]; ++i)
        if (s_fake_threads[i].live && s_fake_threads[i].thid == thid)
            return &s_fake_threads[i];
    return NULL;
}

int __real_sceKernelDeleteThread(SceUID thid)
{
    fake_thread *thread = find_thread(thid);

    if (!thread)
        return -1;
    s_free[0] += thread->stack;
    thread->live = 0;
    return 0;
}

int __real_sceKernelExitDeleteThread(int status)
{
    (void)status;
    return __real_sceKernelDeleteThread(s_current_thid);
}

/* owners */
void isaac_vita_lua_arena_stats(uint32_t *live_kb, uint32_t *peak_kb,
                                uint32_t *fallbacks);
void isaac_vita_lua_arena_stats(uint32_t *live_kb, uint32_t *peak_kb,
                                uint32_t *fallbacks)
{
    *live_kb = s_lua_live_kb;
    *peak_kb = s_lua_peak_kb;
    *fallbacks = 0u;
}

int isaac_vita_guest_heap_telemetry_snapshot_get(
    isaac_vita_guest_heap_telemetry_snapshot *snapshot_out)
{
    memset(snapshot_out, 0, sizeof *snapshot_out);
    snapshot_out->owned_requested_bytes = s_ovf_owned;
    snapshot_out->peak_requested_bytes = s_ovf_peak;
    return 1;
}

int isaac_vita_openal_pool_snapshot_get(
    isaac_vita_openal_pool_snapshot *snapshot_out)
{
    memset(snapshot_out, 0, sizeof *snapshot_out);
    snapshot_out->live_requested_bytes = s_oal_live;
    snapshot_out->peak_requested_bytes = s_oal_peak;
    return 1;
}

void kage_vita_fullspeed_scheduler_debug_overlay_timings(
    uint32_t *update_us, uint32_t *render_us)
{
    *update_us = s_sched_update;
    *render_us = s_sched_render;
}

/* the wrapped entry points under test */
SceUID __wrap_sceKernelAllocMemBlock(const char *name,
                                     SceKernelMemBlockType type, SceSize size,
                                     SceKernelAllocMemBlockOpt *opt);
SceUID __wrap_kuKernelAllocMemBlock(const char *name,
                                    SceKernelMemBlockType type, SceSize size,
                                    void *opt);
int __wrap_sceKernelFreeMemBlock(SceUID uid);
SceUID __wrap_sceKernelCreateThread(const char *name,
                                    SceKernelThreadEntry entry,
                                    int init_priority, SceSize stack_size,
                                    SceUInt32 attr, int cpu_affinity_mask,
                                    const SceKernelThreadOptParam *option);
int __wrap_sceKernelDeleteThread(SceUID thid);
int __wrap_sceKernelExitDeleteThread(int status);

/* ---- helpers ----------------------------------------------------------- */

static void reset_world(void)
{
    isaac_vita_debug_overlay_test_reset();
    memset(s_fake_blocks, 0, sizeof s_fake_blocks);
    memset(s_fake_threads, 0, sizeof s_fake_threads);
    memset(s_vgl_total, 0, sizeof s_vgl_total);
    memset(s_vgl_free, 0, sizeof s_vgl_free);
    s_now = 10000000u;
    s_current_thid = MAIN_THID;
    s_free[0] = 360 * (int64_t)MIB;
    s_free[1] = 112 * (int64_t)MIB;
    s_free[2] = 26 * (int64_t)MIB;
    s_free_rc = 0;
    s_free_queries = 0;
    s_free_queries_at_first_real = -1;
    s_real_allocs = 0;
    s_sysinfo_rc = 0;
    s_callback = NULL;
    s_callback_sets = 0u;
    s_log_count = 0u;
    s_uordblks = 0u;
    s_lua_live_kb = s_lua_peak_kb = 0u;
    s_ovf_owned = s_ovf_peak = 0u;
    s_oal_live = s_oal_peak = 0u;
    s_sched_update = s_sched_render = 0u;
}

static int alloc(const char *name, uint32_t type, uint32_t size)
{
    return __wrap_sceKernelAllocMemBlock(name, type, size, NULL);
}

static void page_line(const isaac_vita_debug_overlay_page *page, unsigned i,
                      char *out)
{
    unsigned n = page->length[i];

    memcpy(out, page->text[i], n);
    out[n] = '\0';
}

static const isaac_vita_debug_overlay_row *row_named(const char *label)
{
    const isaac_vita_debug_overlay_model *m = isaac_vita_debug_overlay_test_model();
    uint32_t i;

    for (i = 0u; i < m->row_count; ++i)
        if (strcmp(m->rows[i].label, label) == 0)
            return &m->rows[i];
    return NULL;
}

/* One present: enter, the swap takes swap_us, return; then idle to the next. */
static void present(uint32_t interval_us, uint32_t swap_us)
{
    ISAAC_VITA_DEBUG_OVERLAY_PRESENT_ENTER();
    s_now += swap_us;
    ISAAC_VITA_DEBUG_OVERLAY_PRESENT_RETURN();
    s_now += interval_us - swap_us;
}

#define USER_RW   0x0C20D060u
#define USER_NC   0x0C208060u
#define CDRAM_RW  0x09408060u
#define PHYC_NC   0x0D808060u
#define CDLG_NC   0x0CA08060u

/* A plausible boot: crt0's newlib heap first, then the runtime's
 * reservations, vitaGL's pools last.  Sizes are synthetic. */
static void boot_world(void)
{
    s_current_thid = MAIN_THID;
    check(alloc("Newlib heap", USER_RW, 0x05100000u) > 0, "newlib heap");
    check(__wrap_kuKernelAllocMemBlock("isaac_pe_image", USER_RW,
                                       0x00a3b000u, NULL) > 0, "pe image");
    check(alloc("isaac_heap_ledger", USER_RW, 0x00492000u) > 0, "ledger");
    check(alloc("isaac_heap_overflow", USER_RW, 0x007d5000u) > 0, "overflow");
    check(alloc("isaac_lua_arena", USER_RW, 48u * MIB) > 0, "lua");
    check(alloc("isaac_openal_pool", USER_RW, 0x00bd6000u) > 0, "openal");
    check(alloc("isaac_np_reserve", USER_RW, 16u * MIB) > 0, "np reserve");
    check(alloc("isaac_np_reuse", USER_RW, 4u * MIB) > 0, "np reuse");
    check(alloc("isaac_texel_scratch", USER_RW, 8u * MIB) > 0, "texel");
    check(alloc("isaac_anm2_scratch", USER_RW, 2u * MIB) > 0, "anm2");
    check(alloc("isaac_save_writer", USER_RW, 1u * MIB) > 0, "save");
    check(alloc("some_lib_block_with_a_long_name", USER_RW, 256u * KIB) > 0,
          "unknown");
    {
        /* transient: peak stays, live row disappears */
        int uid = alloc("isaac_np_scratch", USER_RW, 3u * MIB);

        check(uid > 0, "np scratch");
        check(__wrap_sceKernelFreeMemBlock(uid) == 0, "np scratch free");
        check(__wrap_sceKernelFreeMemBlock(uid) < 0, "double free refused");
    }
    {
        int t1 = __wrap_sceKernelCreateThread("isaac_nv_vorbis", NULL, 0,
                                              256u * KIB, 0u, 0, NULL);
        int t2 = __wrap_sceKernelCreateThread("isaac_save_writer", NULL, 0,
                                              64u * KIB, 0u, 0, NULL);
        int t3 = __wrap_sceKernelCreateThread("openal_mixer", NULL, 0,
                                              128u * KIB, 0u, 0, NULL);
        int t4 = __wrap_sceKernelCreateThread("isaac_lua_beat", NULL, 0,
                                              16u * KIB, 0u, 0, NULL);
        int t5 = __wrap_sceKernelCreateThread("short_lived", NULL, 0,
                                              32u * KIB, 0u, 0, NULL);

        check(t1 > 0 && t2 > 0 && t3 > 0 && t4 > 0 && t5 > 0, "threads");
        find_thread(t1)->load_pct = 12u;
        find_thread(t2)->load_pct = 1u;
        find_thread(t3)->load_pct = 3u;
        /* exit-delete from inside the thread, delete from outside */
        s_current_thid = t4;
        (void)__wrap_sceKernelExitDeleteThread(0);
        s_current_thid = MAIN_THID;
        check(__wrap_sceKernelDeleteThread(t5) == 0, "delete thread");
    }
    /* vitaGL: every USER byte above a 32 MiB threshold, CDRAM above 8 MiB,
     * all phycont, the common-dialog budget */
    {
        uint32_t ram = (uint32_t)(s_free[0] - 32 * (int64_t)MIB);
        uint32_t vram = (uint32_t)(s_free[1] - 8 * (int64_t)MIB);
        uint32_t phyc = (uint32_t)s_free[2];

        check(alloc("ram_mempool", USER_NC, ram) > 0, "vgl ram");
        check(alloc("cdram_mempool", CDRAM_RW, vram) > 0, "vgl vram");
        check(alloc("phycont_mempool", PHYC_NC, phyc) > 0, "vgl phyc");
        check(alloc("cdlg_mempool", CDLG_NC, 0x008c6000u) > 0, "vgl cdlg");
        s_vgl_total[VGL_MEM_RAM] = ram;
        s_vgl_free[VGL_MEM_RAM] = ram - 121u * MIB;
        s_vgl_total[VGL_MEM_VRAM] = vram;
        s_vgl_free[VGL_MEM_VRAM] = vram - 61u * MIB;
        s_vgl_total[VGL_MEM_PHYCONT] = phyc;
        s_vgl_free[VGL_MEM_PHYCONT] = phyc - 5u * MIB;
        s_vgl_total[VGL_MEM_BUDGET] = 0x008c6000u;
        s_vgl_free[VGL_MEM_BUDGET] = 0x008c6000u;
    }
    /* owners */
    s_uordblks = 68u * MIB + 700u * KIB;
    s_lua_live_kb = 12u * 1024u + 300u;
    s_lua_peak_kb = 20u * 1024u;
    s_ovf_owned = 512u * KIB;
    s_ovf_peak = 1200u * KIB;
    s_oal_live = 9u * MIB;
    s_oal_peak = 10u * MIB;
    s_sched_update = 4100u;
    s_sched_render = 9800u;
    /* outside the ledger: a system module took 1.5 MiB of user RAM and
     * 256 KiB of CDRAM after the boot probe */
    s_free[0] -= 1536 * (int64_t)KIB;
    s_free[1] -= 256 * (int64_t)KIB;
}

/* ---- tests ------------------------------------------------------------- */

static void test_format_bytes(void)
{
    static const struct { uint64_t bytes; const char *text; } cases[] = {
        { 0u, "     0" },
        { 1u, "    1K" },
        { 1023u, "    1K" },
        { 1024u, "    1K" },
        { 1025u, "    2K" },
        { 1048575u, " 1024K" },
        { 1048576u, "   1.0" },
        { 1101005u, "   1.1" },   /* >= 1.05 MiB rounds up */
        { 1101004u, "   1.0" },
        { 0x05100000u, "  81.0" },
        { 0x00a3b000u, "  10.2" },
        { 999u * 1048576u + 996148u, "1000" "  " },
        { 1000u * 1048576u, "  1000" },
        { 4095u * 1048576u, "  4095" },
    };
    char out[16];
    unsigned i;

    for (i = 0u; i < sizeof cases / sizeof cases[0]; ++i) {
        unsigned n = isaac_vita_debug_overlay_format_bytes(out, 6u,
                                                           cases[i].bytes);
        char what[64];

        out[n] = '\0';
        snprintf(what, sizeof what, "format_bytes(%llu)",
                 (unsigned long long)cases[i].bytes);
        if (i == 11u) {
            /* 999.95 MiB rounds to 1000.0 -> printed as integer MiB */
            check_str(out, "  1000", what);
            continue;
        }
        check_str(out, cases[i].text, what);
    }
    check_u(isaac_vita_debug_overlay_format_bytes(out, 3u, 0x05100000u), 3u,
            "narrow width never overflows");
    out[3] = '\0';
    check_str(out, "81.", "narrow width truncates, never writes past");
}

static void test_ledger_and_model(void)
{
    const isaac_vita_debug_overlay_model *m;
    const isaac_vita_debug_overlay_row *row;
    int64_t boot_user = 360 * (int64_t)MIB;
    unsigned i;
    unsigned d;

    reset_world();
    boot_world();
    check(s_free_queries_at_first_real == 1,
          "boot probe queried the free size before the first real alloc");
    isaac_vita_debug_overlay_set_mode(ISAAC_VITA_DEBUG_OVERLAY_MODE_FULL);
    check_u(g_isaac_vita_debug_overlay_mode, 2u, "mode full");
    check(s_log_count == 1u && strstr(s_logs[0],
          "KAGE VITA DEBUG OVERLAY: mode=full boot=2 rc=0x00000000 "
          "free(u/c/p)=377487360/117440512/27262976 "
          "ledger(allocs,frees,dropped)=17,1,0 raster=960x544 stride=960") != NULL,
          "set_mode receipt");
    if (s_log_count)
        printf("receipt: %s\n", s_logs[0]);

    /* 2 s of presents: 16.7 ms frames, 1.2 ms swaps, one 33.4 ms hitch in
     * the last second, one slow render */
    for (i = 0u; i < 120u; ++i) {
        uint32_t interval = (i == 100u) ? 33400u : 16700u;

        s_sched_render = (i == 110u) ? 14000u : 9800u;
        present(interval, 1200u);
    }
    s_sched_render = 9800u;
    /* sample right after the last present returned */
    s_now -= 16700u - 1200u;
    isaac_vita_debug_overlay_test_sample();
    m = isaac_vita_debug_overlay_test_model();

    check(s_callback != NULL, "display callback installed in full mode");
    check(m->have_frames && m->have_memory && m->have_free && m->have_base,
          "model complete");
    /* window: presents whose return is < 1 s before now */
    check_u(m->fps_presents, 59u, "presents in the last second");
    check_u(m->frame_p50_us, 16700u, "frame p50");
    check_u(m->frame_max_us, 33400u, "frame max");
    check_u(m->frame_p95_us, 16700u, "frame p95 (one hitch in 59)");
    check_u(m->swap_p50_us, 1200u, "swap p50");
    check_u(m->update_p50_us, 4100u, "update p50");
    check_u(m->render_max_us, 14000u, "render max");
    check(m->have_cpu, "cpu");
    check_u(m->cpu_load[0], 85u, "cpu0");
    check_u(m->cpu_load[1], 12u, "cpu1");
    check_u(m->cpu_load[2], 30u, "cpu2");
    check(m->have_game_thread, "game thread");
    check_u(m->game_thread_load, 78u, "game thread load");
    check_u(m->top_thread_count, 3u, "top threads");
    check_str(m->top_thread_name[0], "NV_VORB", "top thread 0");
    check_u(m->top_thread_load[0], 12u, "top thread 0 load");
    check_str(m->top_thread_name[1], "OPENAL_", "top thread 1");
    check_str(m->top_thread_name[2], "SAVE_WR", "top thread 2");

    row = row_named("NEWLIB HEAP");
    check(row != NULL, "newlib row");
    if (row) {
        check_u(row->resv, 0x05100000u, "newlib resv");
        check_u(row->used, 68u * MIB + 700u * KIB, "newlib used = uordblks");
        check(row->flags & ISAAC_VITA_DEBUG_OVERLAY_ROW_PEAK_SAMPLED,
              "newlib peak is sampled");
    }
    row = row_named("LUA ARENA");
    check(row && row->used == (12u * 1024u + 300u) * 1024u &&
          row->peak == 20u * MIB && row->resv == 48u * MIB &&
          !(row->flags & ISAAC_VITA_DEBUG_OVERLAY_ROW_PEAK_SAMPLED),
          "lua row: owner used/peak");
    row = row_named("HEAP OVERFLOW");
    check(row && row->used == 512u * KIB && row->peak == 1200u * KIB &&
          row->resv == 0x007d5000u, "overflow row");
    row = row_named("OPENAL POOL");
    check(row && row->used == 9u * MIB && row->peak == 10u * MIB,
          "openal row");
    row = row_named("VITAGL RAM");
    check(row && row->used == 121u * MIB &&
          (row->flags & ISAAC_VITA_DEBUG_OVERLAY_ROW_VGL), "vgl ram row");
    row = row_named("PE IMAGE");
    check(row && row->resv == 0x00a3b000u && !(row->flags &
          ISAAC_VITA_DEBUG_OVERLAY_ROW_USED), "pe image row (ku alloc)");
    check(row_named("PNG SCRATCH") == NULL,
          "freed memblock leaves no live row");
    row = row_named("SOME_LIB_BLOCK");
    check(row && row->resv == 256u * KIB, "unknown name, uppercased, cut");
    row = row_named("THREAD STACKS");
    check(row && row->count == 3u &&
          row->resv == 256u * KIB + 64u * KIB + 128u * KIB,
          "thread stacks: deleted threads gone");
    row = row_named("MAIN STACK");
    check(row && row->resv == 4u * MIB, "main stack");
    row = row_named("EBOOT CODE");
    check(row && row->resv == 0x00c40000u, "eboot rx");
    row = row_named("EBOOT DATA");
    check(row && row->resv == 0x00310000u, "eboot rw");
    row = row_named("VITAGL CDLG");
    check(row && row->domain == ISAAC_VITA_DEBUG_OVERLAY_DOMAIN_CDLG,
          "cdlg row in its own domain");

    /* reconciliation */
    check_u(m->base_bytes[0],
            (uint64_t)boot_user + 0x00c40000u + 0x00310000u + 4u * MIB,
            "user base = boot free + eboot + main stack");
    check_s(isaac_vita_debug_overlay_unaccounted(m, 0u), 1536 * (int64_t)KIB,
            "user unaccounted = the out-of-ledger 1.5 MiB");
    check_s(isaac_vita_debug_overlay_unaccounted(m, 1u), 256 * (int64_t)KIB,
            "cdram unaccounted = the out-of-ledger 256 KiB");
    check_s(isaac_vita_debug_overlay_unaccounted(m, 2u), 0,
            "phycont unaccounted = 0");
    for (d = 0u; d < 3u; ++d) {
        int64_t sum = (int64_t)isaac_vita_debug_overlay_domain_rows(m, d) +
                      (int64_t)m->free_bytes[d] +
                      isaac_vita_debug_overlay_unaccounted(m, d);

        check_s(sum, (int64_t)m->base_bytes[d],
                "rows + free + unaccounted == base");
    }
    check(s_log_count > 3u &&
          strstr(s_logs[1], "KAGE VITA DEBUG OVERLAY MEM: sample=1") != NULL,
          "first memory sample dumped to the log");
}

static void print_page(const char *title)
{
    const isaac_vita_debug_overlay_page *page = isaac_vita_debug_overlay_test_page();
    char line[64];
    unsigned i;

    printf("--- %s page ---\n", title);
    if (!page)
        return;
    for (i = 0u; i < page->line_count; ++i) {
        page_line(page, i, line);
        printf("|%-40s| c%u\n", line, (unsigned)page->color[i]);
    }
}

static void test_full_page(void)
{
    const isaac_vita_debug_overlay_page *page = isaac_vita_debug_overlay_test_page();
    char line[64];
    unsigned i;
    int saw_free = 0;
    int saw_unacc = 0;

    check(page != NULL, "full page published");
    if (!page)
        return;
    print_page("full");
    check(page->line_count <= ISAAC_VITA_DEBUG_OVERLAY_LINES, "<= 31 lines");
    for (i = 0u; i < page->line_count; ++i) {
        check(page->length[i] <= ISAAC_VITA_DEBUG_OVERLAY_COLS, "<= 40 cols");
        page_line(page, i, line);
        if (strncmp(line, "FREE", 4u) == 0)
            saw_free = 1;
        if (strncmp(line, "UNACCOUNTED", 11u) == 0) {
            saw_unacc = 1;
            check_str(line, "UNACCOUNTED" "             " "1.5",
                      "unaccounted line");
        }
    }
    check(saw_free && saw_unacc, "free and unaccounted rows");
    page_line(page, 0u, line);
    check_str(line, "FPS 59  FRAME 16.7/16.7/33.4 MS", "full line 1");
    page_line(page, 1u, line);
    check_str(line, "UPD 4.1/4.1 RND 9.8/14.0 SWP 1.2/1.2", "full line 2");
    page_line(page, 2u, line);
    check_str(line, "CPU0 85% CPU1 12% CPU2 30% GAME 78%", "full line 3");
    page_line(page, 3u, line);
    check_str(line, "THR NV_VORB 12% OPENAL_ 3% SAVE_WR 1%", "full line 4");
    page_line(page, 4u, line);
    check_str(line, "RAM MIB         USED   RESV   PEAK", "table header");
    page_line(page, 5u, line);
    check_str(line, "VITAGL RAM     121.0  132.8 121.0*", "largest row first");
}

static void test_compact_page(void)
{
    const isaac_vita_debug_overlay_page *page;
    char line[64];
    unsigned i;

    reset_world();
    boot_world();
    isaac_vita_debug_overlay_set_mode(ISAAC_VITA_DEBUG_OVERLAY_MODE_COMPACT);
    for (i = 0u; i < 70u; ++i)
        present(16700u, 1200u);
    s_now -= 16700u - 1200u;
    isaac_vita_debug_overlay_test_sample();
    page = isaac_vita_debug_overlay_test_page();
    check(page != NULL && page->line_count == 2u, "compact: two lines");
    if (!page)
        return;
    print_page("compact");
    page_line(page, 0u, line);
    check_str(line, "FPS 60  16.7/16.7 MS", "compact line 1");
    page_line(page, 1u, line);
    check(strncmp(line, "FREE U 30.5M C 7.8M OVL ", 24u) == 0,
          "compact line 2 prefix");
    check(!isaac_vita_debug_overlay_test_model()->have_memory,
          "compact never runs the table sample (no mallinfo)");
}

static uint32_t *s_fb;

static int draw_into(unsigned width, unsigned height, unsigned stride,
                     const isaac_vita_debug_overlay_page *page,
                     uint32_t *written_out, unsigned *x_min, unsigned *y_max)
{
    size_t pixels = (size_t)stride * height;
    size_t i;
    uint32_t written;
    unsigned x;
    unsigned y;
    int ok = 1;

    s_fb = (uint32_t *)malloc((pixels + 2u) * sizeof *s_fb);
    if (!s_fb)
        return 0;
    for (i = 0u; i < pixels + 2u; ++i)
        s_fb[i] = REDZONE;
    written = isaac_vita_debug_overlay_draw(&s_fb[1], width, height, stride, page);
    if (s_fb[0] != REDZONE || s_fb[pixels + 1u] != REDZONE)
        ok = 0;
    *x_min = width;
    *y_max = 0u;
    for (y = 0u; y < height; ++y) {
        for (x = 0u; x < stride; ++x) {
            uint32_t v = s_fb[1u + (size_t)y * stride + x];

            if (v == REDZONE)
                continue;
            if (x >= width)
                ok = 0;   /* wrote into the stride padding */
            if (x < *x_min)
                *x_min = x;
            if (y > *y_max)
                *y_max = y;
        }
    }
    *written_out = written;
    free(s_fb);
    s_fb = NULL;
    return ok;
}

static void test_draw(void)
{
    const isaac_vita_debug_overlay_page *page;
    isaac_vita_debug_overlay_page compact;
    static const struct { unsigned w, h, s; } rasters[] = {
        { 960u, 544u, 960u }, { 720u, 408u, 768u }, { 480u, 272u, 512u },
    };
    unsigned r;

    reset_world();
    boot_world();
    isaac_vita_debug_overlay_set_mode(ISAAC_VITA_DEBUG_OVERLAY_MODE_FULL);
    for (r = 0u; r < 70u; ++r)
        present(16700u, 1200u);
    isaac_vita_debug_overlay_test_sample();
    page = isaac_vita_debug_overlay_test_page();
    check(page != NULL, "page for drawing");
    if (!page)
        return;
    (void)isaac_vita_debug_overlay_format(isaac_vita_debug_overlay_test_model(),
                                          ISAAC_VITA_DEBUG_OVERLAY_MODE_COMPACT,
                                          &compact);
    for (r = 0u; r < sizeof rasters / sizeof rasters[0]; ++r) {
        uint32_t written = 0u;
        unsigned x_min = 0u;
        unsigned y_max = 0u;
        char what[96];
        int ok = draw_into(rasters[r].w, rasters[r].h, rasters[r].s, &compact,
                           &written, &x_min, &y_max);

        snprintf(what, sizeof what, "compact %ux%u/%u redzones+padding",
                 rasters[r].w, rasters[r].h, rasters[r].s);
        check(ok && written > 0u, what);
        snprintf(what, sizeof what, "compact %ux%u stays top-right",
                 rasters[r].w, rasters[r].h);
        check(x_min > rasters[r].w / 2u && y_max < rasters[r].h / 4u, what);
        ok = draw_into(rasters[r].w, rasters[r].h, rasters[r].s, page,
                       &written, &x_min, &y_max);
        snprintf(what, sizeof what, "full %ux%u/%u redzones+padding",
                 rasters[r].w, rasters[r].h, rasters[r].s);
        check(ok, what);
        printf("draw %ux%u/%u: full written=%u x_min=%u y_max=%u\n",
               rasters[r].w, rasters[r].h, rasters[r].s, written, x_min, y_max);
    }
    /* the display callback draws the published page into the native raster */
    {
        uint32_t *fb = (uint32_t *)malloc((960u * 544u + 2u) * sizeof *fb);
        size_t i;
        size_t touched = 0u;

        if (fb) {
            for (i = 0u; i < 960u * 544u + 2u; ++i)
                fb[i] = REDZONE;
            isaac_vita_debug_overlay_test_display(&fb[1]);
            for (i = 1u; i <= 960u * 544u; ++i)
                touched += fb[i] != REDZONE;
            check(fb[0] == REDZONE && fb[960u * 544u + 1u] == REDZONE &&
                  touched > 0u, "display callback draws inside the buffer");
            free(fb);
        }
        isaac_vita_debug_overlay_test_display(NULL);
        check(1, "display callback ignores NULL");
    }
}

static void test_off_path(void)
{
    unsigned i;
    uint32_t *fb;
    size_t touched = 0u;

    reset_world();
    boot_world();
    isaac_vita_debug_overlay_set_mode(ISAAC_VITA_DEBUG_OVERLAY_MODE_OFF);
    for (i = 0u; i < 100u; ++i)
        present(16700u, 1200u);
    check(s_callback == NULL && s_callback_sets == 0u,
          "off: display callback never installed");
    check(isaac_vita_debug_overlay_test_page() == NULL, "off: no page");
    check(isaac_vita_debug_overlay_test_model()->fps_presents == 0u,
          "off: present hooks never ran");
    fb = (uint32_t *)malloc(960u * 544u * sizeof *fb);
    if (fb) {
        for (i = 0u; i < 960u * 544u; ++i)
            fb[i] = REDZONE;
        isaac_vita_debug_overlay_test_display(fb);
        for (i = 0u; i < 960u * 544u; ++i)
            touched += fb[i] != REDZONE;
        check(touched == 0u, "off: callback draws nothing without a page");
        free(fb);
    }
    isaac_vita_debug_overlay_set_mode(7u);
    check_u(g_isaac_vita_debug_overlay_mode, 0u, "out-of-range mode -> off");
    check_str(isaac_vita_debug_overlay_mode_name(9u), "off", "name of junk");
    check_str(isaac_vita_debug_overlay_mode_name(1u), "compact", "compact name");
    check_str(isaac_vita_debug_overlay_mode_name(2u), "full", "full name");
}

static void test_ledger_overflow_and_failures(void)
{
    const isaac_vita_debug_overlay_page *page;
    char line[64];
    unsigned i;
    int found = 0;

    reset_world();
    s_free[0] = 2000 * (int64_t)MIB;
    for (i = 0u; i < 170u; ++i)
        (void)alloc("isaac_many", USER_RW, 64u * KIB);
    s_sysinfo_rc = (int)0x80020001u;
    isaac_vita_debug_overlay_set_mode(ISAAC_VITA_DEBUG_OVERLAY_MODE_FULL);
    present(16700u, 1200u);
    page = isaac_vita_debug_overlay_test_page();
    check(isaac_vita_debug_overlay_test_model()->ledger_dropped == 10u,
          "160 slots: ten allocations not booked");
    check(!isaac_vita_debug_overlay_test_model()->have_cpu,
          "sceKernelGetSystemInfo failure -> cpu n/a");
    for (i = 0u; page && i < page->line_count; ++i) {
        page_line(page, i, line);
        if (strcmp(line, "LEDGER FULL: 10 NOT BOOKED") == 0)
            found |= 1;
        if (strncmp(line, "CPU N/A", 7u) == 0)
            found |= 2;
    }
    if (found != 3)
        print_page("overflow");
    check(found == 3, "ledger warning and CPU N/A lines");
}

static double host_seconds(void)
{
    struct timespec ts;

    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void test_host_cost(void)
{
    const isaac_vita_debug_overlay_model *m;
    isaac_vita_debug_overlay_page page;
    uint32_t *fb = (uint32_t *)malloc(960u * 544u * sizeof *fb);
    unsigned i;
    double t0;
    double t1;
    double t2;
    volatile uint32_t sink = 0u;

    reset_world();
    boot_world();
    isaac_vita_debug_overlay_set_mode(ISAAC_VITA_DEBUG_OVERLAY_MODE_FULL);
    for (i = 0u; i < 70u; ++i)
        present(16700u, 1200u);
    m = isaac_vita_debug_overlay_test_model();
    if (!fb)
        return;
    t0 = host_seconds();
    for (i = 0u; i < 2000u; ++i)
        sink += isaac_vita_debug_overlay_format(m, 2u, &page);
    t1 = host_seconds();
    for (i = 0u; i < 500u; ++i)
        sink += isaac_vita_debug_overlay_draw(fb, 960u, 544u, 960u, &page);
    t2 = host_seconds();
    printf("host cost (x86-64, informational): format(full) %.2f us, "
           "draw(full) %.1f us, pixels/draw %u\n",
           (t1 - t0) * 1e6 / 2000.0, (t2 - t1) * 1e6 / 500.0,
           (unsigned)isaac_vita_debug_overlay_draw(fb, 960u, 544u, 960u, &page));
    (void)sink;
    free(fb);
}

/* Synthetic background (gradient + bright blocks) so the stipple shows. */
static void background(uint32_t *fb)
{
    unsigned x;
    unsigned y;

    for (y = 0u; y < 544u; ++y) {
        for (x = 0u; x < 960u; ++x) {
            uint32_t r = 40u + x * 120u / 960u;
            uint32_t g = 70u + y * 100u / 544u;
            uint32_t b = 90u;

            if (((x / 80u) + (y / 80u)) % 3u == 0u) {
                r = 230u; g = 200u; b = 150u;
            }
            if (y < 2u || y >= 542u)
                r = g = b = 0u;
            fb[y * 960u + x] = 0xff000000u | (b << 16) | (g << 8) | r;
        }
    }
}

static void write_mockup(const char *dir, const char *name, uint8_t mode)
{
    isaac_vita_debug_overlay_page page;
    uint32_t *fb = (uint32_t *)malloc(960u * 544u * sizeof *fb);
    char path[512];
    FILE *file;

    if (!fb)
        return;
    background(fb);
    (void)isaac_vita_debug_overlay_format(isaac_vita_debug_overlay_test_model(),
                                          mode, &page);
    (void)isaac_vita_debug_overlay_draw(fb, 960u, 544u, 960u, &page);
    {
        /* Bottom-left label, drawn by the same code into a strip that is
         * then copied in (the panel itself always anchors top-right). */
        static const char label[] = "SYNTHETIC VALUES - HOST ORACLE MOCK-UP";
        static uint32_t strip[960u * 40u];
        isaac_vita_debug_overlay_page note;
        unsigned x;
        unsigned y;
        unsigned x_min = 960u;

        memset(&note, 0, sizeof note);
        note.line_count = 1u;
        note.length[0] = (uint8_t)(sizeof label - 1u);
        note.color[0] = ISAAC_VITA_DEBUG_OVERLAY_COLOR_WARN;
        memcpy(note.text[0], label, sizeof label - 1u);
        for (x = 0u; x < 960u * 40u; ++x)
            strip[x] = REDZONE;
        (void)isaac_vita_debug_overlay_draw(strip, 960u, 40u, 960u, &note);
        for (y = 0u; y < 40u; ++y)
            for (x = 0u; x < 960u; ++x)
                if (strip[y * 960u + x] != REDZONE && x < x_min)
                    x_min = x;
        for (y = 0u; y < 40u; ++y)
            for (x = x_min; x < 960u; ++x)
                if (strip[y * 960u + x] != REDZONE)
                    fb[(y + 504u) * 960u + (x - x_min + 8u)] =
                        strip[y * 960u + x];
    }
    snprintf(path, sizeof path, "%s/%s.rgba", dir, name);
    file = fopen(path, "wb");
    if (file) {
        fwrite(fb, sizeof *fb, 960u * 544u, file);
        fclose(file);
        printf("mockup: %s (960x544 RGBA, SYNTHETIC values)\n", path);
    }
    free(fb);
}

int main(int argc, char **argv)
{
    test_format_bytes();
    test_ledger_and_model();
    test_full_page();
    test_compact_page();
    test_draw();
    test_off_path();
    test_ledger_overflow_and_failures();
    test_host_cost();
    if (argc > 1) {
        unsigned i;

        reset_world();
        boot_world();
        isaac_vita_debug_overlay_set_mode(ISAAC_VITA_DEBUG_OVERLAY_MODE_FULL);
        for (i = 0u; i < 120u; ++i) {
            s_sched_render = (i == 110u) ? 14000u : 9800u;
            present(i == 100u ? 33400u : 16700u, 1200u);
        }
        isaac_vita_debug_overlay_test_sample();
        write_mockup(argv[1], "overlay-full", 2u);
        write_mockup(argv[1], "overlay-compact", 1u);
    }
    if (s_failures) {
        fprintf(stderr, "host_vita_debug_overlay_oracle: FAIL (%u of %u checks)\n",
                s_failures, s_checks);
        return 1;
    }
    printf("host_vita_debug_overlay_oracle: PASS (%u checks; format, ledger, "
           "boot probe, reconciliation, frame/cpu/thread stats, compact+full "
           "pages, redzone draw at 3 rasters, off path)\n", s_checks);
    return 0;
}
