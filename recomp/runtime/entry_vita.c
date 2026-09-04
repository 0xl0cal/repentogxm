/* Vita bring-up boundary: load the real PE, run loader TLS, enter its original
 * startup RVA, and accept only the exact controlled KAGE seam. */
#include <stdint.h>
#include <string.h>
#ifdef __vita__
#include <malloc.h>
#endif

#include "guest.h"
#include "host_vita_console.h"
#include "host_vita_com.h"
#include "host_vita_crt.h"
#include "host_vita_exception.h"
#include "host_vita_fls.h"
#include "host_vita_fios_cache.h"
#include "host_vita_heap.h"
#include "host_vita_memory.h"
#include "host_vita_post_com.h"
#include "host_vita_startup.h"
#include "host_vita_steam.h"
#include "host_vita_sync.h"
#include "kage_vita_io_profile.h"
#include "manual_kage.h"
#include "platform.h"
#include "vita_boot_imports.h"
#if defined(ISAAC_VITA_LOG_ASYNC)
#include "host_vita_log_async.h"
#endif
#ifdef ISAAC_VITA_TEXTURE_ALIGN8_POLICY
#include "kage_vita_texture_memory.h"
#endif

#ifndef ISAAC_VITA_AUDIO
#define ISAAC_VITA_AUDIO 0
#endif

#ifndef ISAAC_VITA_KAGE
#define ISAAC_VITA_KAGE 0
#endif

#ifndef ISAAC_VITA_FIOS_CACHE
#define ISAAC_VITA_FIOS_CACHE 0
#endif

#if ISAAC_VITA_AUDIO
#include "host_vita_audio.h"
#endif
#if defined(ISAAC_VITA_NATIVE_PNG)
#include "host_vita_native_png.h"
#endif

#if ISAAC_VITA_KAGE
#include <psp2/kernel/processmgr.h>

#include "kage_vita_backend.h"
#if defined(ISAAC_VITA_LUA)
#include "host_vita_lua.h"
#endif
#if defined(ISAAC_VITA_GUEST_SAMPLER)
#include "kage_vita_guest_sampler.h"
#endif
#endif

#if GUEST_IMAGE_BASE != 0x98000000u
#error Vita first-arm-fault requires generated code and runtime at 0x98000000
#endif

/* Generated advisory-SFX seams deliberately know nothing about the build's
 * OpenAL option or the mutable guest Sound::Manager bytes.  This entry-owned
 * wrapper gives every generated TU one stable ABI: Audio-OFF and failed init
 * retain the original eager materialisation, while a successfully published
 * native manager enables first-Play materialisation. */
int isaac_vita_audio_materialization_ready(void)
{
#if ISAAC_VITA_AUDIO
    return isaac_vita_audio_manager_is_active();
#else
    return 0;
#endif
}

/* Copied from the measured input PE and its generated entry body. */
#define ISAAC_ENTRY_RVA             0x005eb83eU
#define ISAAC_SENTINEL_RETURN       0xdeadbeefU

/* new() reaches this exact throw helper only after malloc returned NULL and
 * the installed new-handler declined the request.  Keep the diagnostic local
 * to that measured bad_alloc path; other C++ throws have different callers. */
#define ISAAC_VITA_CXX_THROW_IMPORT_RVA       0x00606480U
#define ISAAC_VITA_BAD_ALLOC_THROW_RETURN_RVA 0x005ebdb5U

/* Exact normal-success suffix after the measured 67-call frontier:
 * SetUnhandledExceptionFilter once, then nine heap calls and 27 already-
 * implemented `_crt_atexit` calls from the `_initterm` table.  The next
 * unresolved call is the table's first plain InitializeCriticalSection. */
#define ISAAC_VITA_LATE_HEAP_CALL_COUNT 9U
#define ISAAC_VITA_LATE_CRT_CALL_COUNT  27U
#define ISAAC_VITA_BOOT104_CALL_COUNT \
    (ISAAC_VITA_BOOT_BASE_CALL_COUNT + \
     ISAAC_VITA_PROCESSOR_FEATURE_IMPORT_COUNT + \
     ISAAC_VITA_CRT_BOOT_CALL_COUNT + \
     ISAAC_VITA_SYNC_CALL_COUNT + \
     ISAAC_VITA_MEMORY_BOOT_CALL_COUNT + \
     ISAAC_VITA_CONSOLE_BOOT_CALL_COUNT + \
     ISAAC_VITA_STARTUP_BOOT_CALL_COUNT + \
     ISAAC_VITA_FLS_BOOT_CALL_COUNT + \
     ISAAC_VITA_EXCEPTION_BOOT_CALL_COUNT + \
     ISAAC_VITA_LATE_HEAP_CALL_COUNT + \
     ISAAC_VITA_LATE_CRT_CALL_COUNT)
#define ISAAC_VITA_POST_ICS_TABLE_CALL_COUNT 150U
#define ISAAC_VITA_BOOT254_CALL_COUNT \
    (ISAAC_VITA_BOOT104_CALL_COUNT + ISAAC_VITA_POST_ICS_TABLE_CALL_COUNT)

/* Exact normal-success suffix after Steam callback registration: seven
 * existing `_crt_atexit` registrations, three CRT argc/argv accessors, three
 * already-supported main-entry memsets, then the absent USERENV module/proc
 * fallback, one zero-length memmove and one 24-byte vector allocation.  The
 * next attempted import is getenv("USERPROFILE"). */
#define ISAAC_VITA_MAIN_ENTRY_MEMSET_CALL_COUNT 3U
#define ISAAC_VITA_POST_STEAM_CRT_ACCESSOR_CALL_COUNT 3U
#define ISAAC_VITA_USERENV_FALLBACK_CALL_COUNT \
    (ISAAC_VITA_STARTUP_USERENV_OWNED_CALL_COUNT + \
     ISAAC_VITA_SYNC_LATE_GET_PROC_CALL_COUNT)
#define ISAAC_VITA_BOOT273_CALL_COUNT \
    (ISAAC_VITA_BOOT254_CALL_COUNT + \
     ISAAC_VITA_STEAM_BOOT_CALL_COUNT + \
     ISAAC_VITA_STEAM_POST_REGISTER_ATEXIT_CALL_COUNT + \
     ISAAC_VITA_POST_STEAM_CRT_ACCESSOR_CALL_COUNT + \
     ISAAC_VITA_MAIN_ENTRY_MEMSET_CALL_COUNT + \
     ISAAC_VITA_USERENV_FALLBACK_CALL_COUNT + \
     ISAAC_VITA_MEMORY_USERENV_MEMMOVE_BOOT_CALL_COUNT + \
     ISAAC_VITA_HEAP_USERENV_VECTOR_BOOT_CALL_COUNT)

/* Fixed USERPROFILE path from getenv through the first directory walker:
 * #274..307 build/save the path, #308 initializes COM, #309..312 apply the
 * target-local Steam/power/timer policy, and #313..345 initialize logging and
 * the walker.  Keep the two larger suffixes local until their owners expose a
 * narrower semantic count than the complete #274..345 batch. */
#define ISAAC_VITA_SAVEPATH_PREFIX_CALL_COUNT 34U
#define ISAAC_VITA_LOGGER_WALKER_CALL_COUNT   33U
#define ISAAC_VITA_BOOT345_CALL_COUNT \
    (ISAAC_VITA_BOOT273_CALL_COUNT + \
     ISAAC_VITA_SAVEPATH_PREFIX_CALL_COUNT + \
     ISAAC_VITA_COM_BOOT_CALL_COUNT + \
     ISAAC_VITA_POST_COM_BOOT_CALL_COUNT + \
     ISAAC_VITA_LOGGER_WALKER_CALL_COUNT)

/* The now-implemented directory walk and remaining startup path add 203
 * handled imports before the deliberate high-level graphics seam.  Keep this
 * measured suffix whole until its subsystem owners expose narrower counts. */
#define ISAAC_VITA_POST_FIND_TO_KAGE_CALL_COUNT 203U
#define ISAAC_VITA_BOOT548_CALL_COUNT \
    (ISAAC_VITA_BOOT345_CALL_COUNT + \
     ISAAC_VITA_POST_FIND_TO_KAGE_CALL_COUNT)

#define ISAAC_VITA_KAGE_RETURN_RVA      0x005bfa12U

_Static_assert(ISAAC_VITA_BOOT104_CALL_COUNT == 104U,
               "Vita post-heap boot-call denominator drifted");
_Static_assert(ISAAC_VITA_BOOT254_CALL_COUNT == 254U &&
               ISAAC_VITA_SYNC_LATE_NEXT_ORDINAL == 255U,
               "Vita post-critical-section denominator drifted");
_Static_assert(ISAAC_VITA_BOOT273_CALL_COUNT == 273U &&
               ISAAC_VITA_STEAM_REGISTER_CALLBACK_ORDINAL == 255U &&
               ISAAC_VITA_STEAM_NEXT_ORDINAL == 263U &&
               ISAAC_VITA_CRT_GET_INITIAL_ENVIRONMENT_BOOT_ORDINAL == 263U &&
               ISAAC_VITA_CRT_P_ARGC_BOOT_ORDINAL == 265U &&
               ISAAC_VITA_STARTUP_LOAD_USERENV_ORDINAL == 269U &&
               ISAAC_VITA_SYNC_GET_USER_PROFILE_ORDINAL == 270U &&
               ISAAC_VITA_STARTUP_FREE_USERENV_ORDINAL == 271U &&
               ISAAC_VITA_MEMORY_MEMMOVE_CALL_RVA == 0x00007ca7U &&
               ISAAC_VITA_HEAP_USERENV_VECTOR_ALLOC_CALL_RVA == 0x00022095U &&
               ISAAC_VITA_HEAP_WRAPPER_MALLOC_CALL_RVA == 0x005ead0dU &&
               ISAAC_VITA_STARTUP_USERENV_NEXT_ATTEMPT_ORDINAL == 274U,
               "Vita post-USERENV boot-call denominator drifted");
_Static_assert(ISAAC_VITA_BOOT345_CALL_COUNT == 345U &&
               ISAAC_VITA_CRT_FIXED_CALL_COUNT ==
                   ISAAC_VITA_SAVEPATH_PREFIX_CALL_COUNT +
                   ISAAC_VITA_COM_BOOT_CALL_COUNT +
                   ISAAC_VITA_POST_COM_BOOT_CALL_COUNT +
                   ISAAC_VITA_LOGGER_WALKER_CALL_COUNT,
               "Vita fixed-path boot-call denominator drifted");
_Static_assert(ISAAC_VITA_CRT_FIXED_FIRST_ORDINAL == 274U &&
               ISAAC_VITA_CRT_STRNCPY_S_LOG_PATH_ORDINAL == 307U &&
               ISAAC_VITA_COM_INITIALIZE_ORDINAL == 308U &&
               ISAAC_VITA_POST_COM_STEAM_INIT_ORDINAL == 309U &&
               ISAAC_VITA_POST_COM_BEGIN_PERIOD_ORDINAL == 312U &&
               ISAAC_VITA_POST_COM_NEXT_ORDINAL == 313U &&
               ISAAC_VITA_CRT_FIXED_LAST_ORDINAL == 345U &&
               ISAAC_VITA_CRT_FIXED_NEXT_ATTEMPT_ORDINAL == 346U,
               "Vita fixed-path import ordinals drifted");
_Static_assert(ISAAC_VITA_BOOT548_CALL_COUNT == 548U,
               "Vita final KAGE-boundary denominator drifted");

void guest_register_all(void);
void guest_register_all_imports(void);
extern const uint32_t guest_table_len;
extern const uint32_t guest_import_table_len;

static int s_tls_attach_status;

static void attach_static_tls(CPU *__restrict c)
{
    s_tls_attach_status = guest_tls_process_attach(c);
}

int isaac_vita_run_first_fault(const char *pe_path)
{
    CPU cpu;
    guest_fn entry;
    uint32_t return_to = 0U;
    int return_readable = 0;
    int boundary_shape_matches = 0;
    int stopped;
    int result = 2;

    guest_cpu_init(&cpu);
    g_host_import_calls = 0U;
    g_host_dynamic_calls = 0U;
    g_guest_gl_inventory_mode = 0;
    g_guest_checkpoint_rva = 0U;

    isaac_vita_log("guest boundary begin: PE=%s base=0x%08x entry=0x%08x",
                   pe_path ? pe_path : "<null>",
                   (unsigned)GUEST_IMAGE_BASE,
                   (unsigned)ISAAC_ENTRY_RVA);
    if (!pe_path) {
        isaac_vita_log("guest setup FAILED: null PE path");
        goto done;
    }

    guest_register_all_imports();
    if (guest_image_load(pe_path) != 0) {
        isaac_vita_log("guest setup FAILED: PE load/relocate/IAT/TLS metadata");
        goto done;
    }
#ifdef ISAAC_VITA_TEXTURE_ALIGN8_POLICY
    if (!kage_vita_texture_align8_policy_apply()) {
        isaac_vita_log("guest setup FAILED: KAGE texture align8 policy");
        goto done;
    }
#endif
    if (guest_stack_init(&cpu) != 0) {
        isaac_vita_log("guest setup FAILED: 4 MiB guest stack allocation");
        goto done;
    }
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    if (!isaac_vita_guest_heap_init(cpu.stack_floor, cpu.stack_ceiling)) {
        isaac_vita_log(
            "guest setup FAILED: fixed ledger/overflow mspace transaction");
        goto done;
    }
    isaac_vita_log(
        "guest heap overflow PASS: ledger_capacity=%u pool_bytes=%u",
        (unsigned)ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_CAPACITY,
        (unsigned)ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES);
#endif
#if defined(ISAAC_VITA_LUA) && defined(ISAAC_VITA_LUA_ARENA_MB) &&     (ISAAC_VITA_LUA_ARENA_MB > 0)
    /* Before the guest runs (and vitaGL takes all USER_RW above its
     * threshold): a failed reserve is logged and leaves the guest heap path. */
    (void)isaac_vita_lua_arena_reserve();
#endif
#if ISAAC_VITA_FIOS_CACHE
    {
        isaac_vita_fios_cache_snapshot fios;
        int active = isaac_vita_fios_cache_initialize();

        isaac_vita_fios_cache_snapshot_get(&fios);
        isaac_vita_log(
            "FIOS cache: active=%u state=%u failure=%u uid=%d "
            "memblock=%u cache=%u init=0x%08x filter=0x%08x",
            (unsigned)(active != 0), (unsigned)fios.state,
            (unsigned)fios.failure, (int)fios.uid,
            (unsigned)fios.memblock_bytes, (unsigned)fios.cache_bytes,
            (unsigned)fios.initialize_result,
            (unsigned)fios.filter_add_result);
    }
#endif
#if ISAAC_VITA_AUDIO
    /* Same contract as the Lua arena above: the OpenAL pool's 0xbd6000
     * USER_RW memblock must exist before the guest's first KAGE call runs
     * vglInit*, which hands every USER_RW byte above its 16 MiB threshold to
     * the vitaGL RAM pool.  Requested after vitaGL (as the Sound::Manager
     * boundary did) it competed with the 0xbd6000 texel scratch inside that
     * threshold and lost on every audio-ON boot (0x5aa000 free, state=3
     * init=5), which routed the core-2 mixer's allocations into the guest's
     * newlib heap for the whole session.  A failure here is logged and the
     * manager path keeps its measured whole-session fallback. */
    isaac_vita_log("OpenAL pool reserve before vitaGL: ready=%d",
                   isaac_vita_audio_openal_pool_reserve());
#endif
#if defined(ISAAC_VITA_NATIVE_PNG)
    /* Same contract once more: the native PNG decoder's whole-image scratch
     * (ISAAC_VITA_NATIVE_PNG_RESERVE_MB) must exist before vglInit* hands
     * every USER_RW byte above the threshold to the vitaGL RAM pool.
     * Requested per image at the first row (perf:wf-flags-v5-png) it shared
     * the post-init window with the 0xbd6000 texel scratch and the 0x2a2000
     * ANM2 scratch and was refused for every sheet above 1 MiB (1024x2048,
     * 1024x1024, 960x800, 1024x880 -> translated libpng, 120-340 ms each).
     * A failure is logged and leaves the per-image path. */
    isaac_vita_log("native PNG scratch reserve before vitaGL: ready=%d",
                   isaac_vita_native_png_reserve());
#endif
    guest_register_all();
    if (guest_coverage_init_from_env() != 0) {
        isaac_vita_log("guest setup FAILED: coverage boundary: %s",
                       guest_coverage_error() ? guest_coverage_error()
                                              : "<unknown>");
        goto done;
    }
    isaac_vita_log("guest tables registered: functions=%u imports=%u %s",
                   (unsigned)guest_table_len,
                   (unsigned)guest_import_table_len,
                   guest_generation_id());
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    /* The sampler only reads this stack-owned CPU's ESP and the fixed guest
     * stack it owns; both outlive every guest_run_until_stop below. */
    kage_vita_guest_sampler_start(&cpu);
#endif

    /* This stack-owned CPU never escapes the synchronous guest driver. */
    isaac_vita_sync_bind_current_thread(&cpu);
    s_tls_attach_status = -1;
    stopped = guest_run_until_stop(&cpu, attach_static_tls);
    if (stopped == GUEST_RUN_EXIT) {
        isaac_vita_log(
            "guest process exit: phase=static-tls api=%s code=%d",
            cpu.exit_api ? cpu.exit_api : "<unknown>",
            (int)cpu.exit_code);
        result = (int)cpu.exit_code;
        goto done;
    }
    if (stopped != GUEST_RUN_RETURNED || s_tls_attach_status != 0) {
        isaac_vita_log(
            "static TLS attach FAILED: run=%d status=%d addr=0x%08x fault=%s",
            stopped, s_tls_attach_status, (unsigned)cpu.fault_addr,
            cpu.fault ? cpu.fault : "<none>");
        result = 1;
        goto done;
    }
    isaac_vita_log("static TLS attach PASS");

    entry = guest_lookup(ISAAC_ENTRY_RVA);
    if (!entry) {
        isaac_vita_log("guest setup FAILED: entry RVA 0x%08x is absent",
                       (unsigned)ISAAC_ENTRY_RVA);
        goto done;
    }

    gpush(&cpu, ISAAC_SENTINEL_RETURN);
    stopped = guest_run_until_stop(&cpu, entry);
    if (guest_stack_contains(&cpu, cpu.esp, 4U)) {
        return_to = ld32(cpu.esp);
        return_readable = 1;
    }

    isaac_vita_log(
        "guest stop: run=%d addr=0x%08x fault=%s return=%s0x%08x",
        stopped, (unsigned)cpu.fault_addr,
        cpu.fault ? cpu.fault : "<none>",
        return_readable ? "" : "unreadable/",
        (unsigned)return_to);
    isaac_vita_log(
        "guest regs: esp=%08x ebp=%08x eax=%08x ebx=%08x ecx=%08x "
        "edx=%08x esi=%08x edi=%08x",
        (unsigned)cpu.esp, (unsigned)cpu.ebp, (unsigned)cpu.eax,
        (unsigned)cpu.ebx, (unsigned)cpu.ecx, (unsigned)cpu.edx,
        (unsigned)cpu.esi, (unsigned)cpu.edi);

    if (stopped == GUEST_RUN_EXIT) {
        isaac_vita_log("guest process exit: phase=entry api=%s code=%d",
                       cpu.exit_api ? cpu.exit_api : "<unknown>",
                       (int)cpu.exit_code);
        result = (int)cpu.exit_code;
        goto done;
    }

    if (stopped == GUEST_RUN_FAULT &&
        cpu.fault_addr ==
            GUEST_IMAGE_BASE + ISAAC_VITA_CXX_THROW_IMPORT_RVA &&
        return_readable &&
        return_to == ISAAC_VITA_BAD_ALLOC_THROW_RETURN_RVA) {
        uint32_t allocator_frame = 0U;
        uint32_t owner_return_rva = 0U;
        uint32_t failed_request = 0U;
        int owner_readable = 0;
        int request_readable = 0;

        /* sub_5ebd99 pushes the EBP of the failing sub_5eacf8 malloc
         * wrapper.  Its original size argument is therefore [saved EBP+8]. */
        if (guest_stack_contains(&cpu, cpu.ebp, 4U)) {
            allocator_frame = ld32(cpu.ebp);
            if (allocator_frame <= UINT32_MAX - 4U &&
                guest_stack_contains(&cpu, allocator_frame + 4U, 4U)) {
                owner_return_rva = ld32(allocator_frame + 4U);
                owner_readable = 1;
            }
            if (allocator_frame <= UINT32_MAX - 8U &&
                guest_stack_contains(&cpu, allocator_frame + 8U, 4U)) {
                failed_request = ld32(allocator_frame + 8U);
                request_readable = 1;
            }
        }
#ifdef __vita__
        {
            struct mallinfo heap = mallinfo();
            isaac_vita_log(
                "bad_alloc diagnostic: request=%s%u owner=%s0x%08x arena=%u "
                "allocated=%u free=%u chunks=%u",
                request_readable ? "" : "unreadable/",
                (unsigned)failed_request,
                owner_readable ? "" : "unreadable/",
                (unsigned)owner_return_rva, (unsigned)heap.arena,
                (unsigned)heap.uordblks, (unsigned)heap.fordblks,
                (unsigned)heap.ordblks);
        }
#else
        isaac_vita_log(
                       "bad_alloc diagnostic: request=%s%u "
                       "owner=%s0x%08x",
                       request_readable ? "" : "unreadable/",
                       (unsigned)failed_request,
                       owner_readable ? "" : "unreadable/",
                       (unsigned)owner_return_rva);
#endif
    }

    boundary_shape_matches =
        stopped == GUEST_RUN_FAULT && cpu.fault &&
        strcmp(cpu.fault, GUEST_KAGE_RENDER_DISPLAY_FAULT) == 0 &&
        cpu.fault_addr == GUEST_KAGE_RENDER_DISPLAY_RVA &&
        return_readable &&
        return_to == ISAAC_VITA_KAGE_RETURN_RVA;

    if (boundary_shape_matches &&
        g_host_import_calls == ISAAC_VITA_BOOT548_CALL_COUNT &&
        g_host_dynamic_calls == 0U) {
        isaac_vita_log(
            "KAGE BOUNDARY PASS: import calls actual=%u expected=%u; "
            "fault=%s addr=0x%08x return=0x%08x",
            g_host_import_calls, (unsigned)ISAAC_VITA_BOOT548_CALL_COUNT,
            cpu.fault, (unsigned)cpu.fault_addr, (unsigned)return_to);
        result = 0;
    } else {
        isaac_vita_log(
            "KAGE BOUNDARY FAIL: import calls actual=%u expected=%u; "
            "dynamic actual=%u expected=0; run actual=%d expected=%d; "
            "expected fault=%s addr=0x%08x return=0x%08x; "
            "actual fault=%s addr=0x%08x return=%s0x%08x",
            g_host_import_calls, (unsigned)ISAAC_VITA_BOOT548_CALL_COUNT,
            g_host_dynamic_calls, stopped, GUEST_RUN_FAULT,
            GUEST_KAGE_RENDER_DISPLAY_FAULT,
            (unsigned)GUEST_KAGE_RENDER_DISPLAY_RVA,
            (unsigned)ISAAC_VITA_KAGE_RETURN_RVA,
            cpu.fault ? cpu.fault : "<none>", (unsigned)cpu.fault_addr,
            return_readable ? "" : "unreadable/", (unsigned)return_to);
        result = 1;
    }

done:
#if ISAAC_VITA_KAGE
    /* The loading-complete path normally closes this startup-only profile.
     * Any earlier controlled guest fault reaches this common tail instead;
     * keep that failure just as observable as a GXM reserve failure. */
    kage_vita_io_profile_report("entry-tail");
#endif
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    isaac_vita_guest_heap_telemetry_log_final();
#endif
#if ISAAC_VITA_KAGE
    /* Remove the diagnostic/display callbacks while their owners are live.
     * The process-wide vitaGL/GXM context deliberately remains intact: kernel
     * process teardown owns it after the terminal handoff below. */
    kage_vita_backend_deactivate();
#endif
#if ISAAC_VITA_AUDIO
    {
        int audio_shutdown_status;

        /* alcDestroyContext reaches the Vita backend stop path, which sets
         * its kill flag and joins the mixer thread.  This must precede every
         * guest or process-memory release below. */
        audio_shutdown_status = isaac_vita_audio_shutdown_active();
        if (audio_shutdown_status == ISAAC_VITA_AUDIO_SHUTDOWN_COMPLETE) {
            isaac_vita_log("KAGE VITA AUDIO SHUTDOWN: complete");
        } else if (audio_shutdown_status ==
                   ISAAC_VITA_AUDIO_SHUTDOWN_INCOMPLETE) {
            isaac_vita_log(
                "KAGE VITA AUDIO SHUTDOWN: incomplete "
                "(device close failed; owner retained)");
        }
        isaac_vita_audio_log_final();
    }
#endif
    /* Pending save images must reach the card before the FILE cache and the
     * FIOS buffers go away; the drain blocks only while a job is in flight. */
    isaac_vita_crt_async_write_shutdown();
    isaac_vita_crt_archive_cache_shutdown();
#if ISAAC_VITA_FIOS_CACHE
    /* No FIOS-owned worker may retain the dedicated buffer after this edge.
     * It precedes guest stack/image release and is safe on every early tail. */
    isaac_vita_fios_cache_shutdown();
#endif
    guest_coverage_shutdown();
    if (cpu.stack_owner || cpu.stack_floor || cpu.stack_ceiling ||
        cpu.stack_fault_kind != GUEST_STACK_FAULT_NONE) {
        isaac_vita_log(
            "guest stack: floor=%08x ceiling=%08x low=%08x "
            "capacity=%u high_water=%u fault_kind=%u fault_addr=%08x "
            "fault_size=%u fault_pc=%08x fault_native=%p",
            (unsigned)cpu.stack_floor, (unsigned)cpu.stack_ceiling,
            (unsigned)cpu.stack_low_water,
            (unsigned)guest_stack_capacity(&cpu),
            (unsigned)guest_stack_high_water(&cpu),
            (unsigned)cpu.stack_fault_kind,
            (unsigned)cpu.stack_fault_address,
            (unsigned)cpu.stack_fault_size,
            (unsigned)cpu.stack_fault_pc,
            (void *)cpu.stack_fault_native_site);
    }
    guest_stack_free(&cpu);
    guest_image_free();
#if ISAAC_VITA_KAGE
    /* Returning from main would first run newlib teardown while vitaGL still
     * owns process-memory-backed GXM state.  Do not enqueue a final swap or
     * call sceGxmTerminate here: the process-exit owner tears those resources
     * down while their allocation arena is still mapped.  Vita3K implements
     * sceKernelExitProcess as an asynchronous request, so keep this boundary
     * compiler-visible and terminal exactly like newlib's _exit path. */
#if defined(ISAAC_VITA_LOG_ASYNC)
    {
        IsaacVitaLogAsyncStats log_stats;

        isaac_vita_log_async_get_stats(&log_stats);
        isaac_vita_log(
            "KAGE VITA LOG ASYNC: exit enqueued=%u sunk=%u dropped=%u "
            "high_water=%u",
            (unsigned)log_stats.enqueued, (unsigned)log_stats.sunk,
            (unsigned)log_stats.dropped, (unsigned)log_stats.high_water);
    }
#endif
    isaac_vita_log("KAGE VITA PROCESS EXIT HANDOFF: result=%d", result);
#if defined(ISAAC_VITA_LOG_ASYNC)
    /* Every queued line (guest stop/regs/fault diagnostics included)
     * reaches its sink before the process-exit request. */
    isaac_vita_log_async_flush();
#endif
    sceKernelExitProcess(result);
    for (;;) {
    }
#else
#if defined(ISAAC_VITA_LOG_ASYNC)
    isaac_vita_log_async_flush();
#endif
    return result;
#endif
}
