/* Run the real PE entry point until the first controlled guest stop.
 *
 * This is deliberately separate from full_test.c.  The oracle suite must end
 * green; startup bring-up must expose the next missing dependency or process
 * exit without
 * converting it into a host access violation.  The expected dependency is
 * advanced whenever a host shim is added.
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "guest.h"
#include "guest_coverage_generated.h"
#include "gl_pc_backend.h"
#include "host_frontier.h"
#include "kage_pc_backend.h"
#include "manual_kage.h"

#ifndef EXE_PATH
#define EXE_PATH  "isaac-ng.exe.unpacked.exe"
#endif
#define ENTRY_RVA 0x005eb83eu
#define APPLICATION_MAIN_RVA 0x0048bc50u

/* The frontier, measured -- not predicted.  The default probe stops at the
 * first instruction of application main; `--past-main` leaves the checkpoint
 * inert and is an exploratory run to the next loud dependency. */
#define EXPECTED_TOKEN  APPLICATION_MAIN_RVA
#define EXPECTED_NAME   "bring-up checkpoint"
#define EXPECTED_RETURN 0x005eb7b6u
#define EXPECTED_PAST_KAGE_RETURN 0x005bfa12u

void guest_register_all(void);
void guest_register_all_imports(void);
extern const uint32_t guest_table_len;
extern const uint32_t guest_import_table_len;

static CPU cpu;
static int tls_attach_status;

static void attach_static_tls(CPU *__restrict c)
{
    tls_attach_status = guest_tls_process_attach(c);
}

static int copy_process_words(uintptr_t address, uint32_t *words, size_t count)
{
    SIZE_T copied = 0U;
    size_t bytes = count * sizeof words[0];

    memset(words, 0, bytes);
    if (!address || address > UINTPTR_MAX - bytes)
        return 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)address,
                             words, bytes, &copied) && copied == bytes;
}

static void report_guest_frame_chain(void)
{
    uint32_t frame = cpu.ebp;
    unsigned depth;
    for (depth = 0U; depth < 12U && frame; ++depth) {
        uint32_t pair[2];
        if (!copy_process_words((uintptr_t)frame, pair, 2U)) {
            fprintf(stderr, "guest call frame %02u         : ebp=%08x unreadable\n",
                    depth, frame);
            return;
        }
        fprintf(stderr,
                "guest call frame %02u         : ebp=%08x parent=%08x ret=%08x\n",
                depth, frame, pair[0], pair[1]);
        if (pair[0] <= frame || pair[0] - frame > 0x100000U)
            return;
        frame = pair[0];
    }
}

static void report_guest_stop_state(void)
{
    uint32_t stack[12], frame[9];
    int stack_readable = copy_process_words(
        (uintptr_t)cpu.esp, stack, sizeof stack / sizeof stack[0]);
    int frame_readable = cpu.ebp >= 16U && copy_process_words(
        (uintptr_t)(cpu.ebp - 16U), frame, sizeof frame / sizeof frame[0]);

    fprintf(stderr,
            "guest stop registers        : esp=%08x ebp=%08x eax=%08x ebx=%08x "
            "ecx=%08x edx=%08x esi=%08x edi=%08x\n"
            "guest stack +00..+2c        : %s %08x %08x %08x %08x "
            "%08x %08x %08x %08x %08x %08x %08x %08x\n"
            "guest frame -10..+10        : %s %08x %08x %08x %08x | "
            "%08x %08x %08x %08x %08x\n",
            cpu.esp, cpu.ebp, cpu.eax, cpu.ebx, cpu.ecx, cpu.edx,
            cpu.esi, cpu.edi,
            stack_readable ? "readable" : "unreadable",
            stack[0], stack[1], stack[2], stack[3], stack[4], stack[5],
            stack[6], stack[7], stack[8], stack[9], stack[10], stack[11],
            frame_readable ? "readable" : "unreadable",
            frame[0], frame[1], frame[2], frame[3], frame[4], frame[5],
            frame[6], frame[7], frame[8]);
    report_guest_frame_chain();
    fflush(stderr);
}

static LONG CALLBACK report_host_exception(EXCEPTION_POINTERS *info)
{
    EXCEPTION_RECORD *record = info->ExceptionRecord;
    CONTEXT *context = info->ContextRecord;
    ULONG_PTR access = record->NumberParameters >= 2
        ? record->ExceptionInformation[1] : 0;
    ULONG_PTR operation = record->NumberParameters >= 1
        ? record->ExceptionInformation[0] : ~(ULONG_PTR)0;
    uint32_t native_stack[4], guest_frame[9];
    int stack_readable = copy_process_words(
        (uintptr_t)context->Esp, native_stack, 4U);
    int frame_readable = cpu.ebp >= 16U && copy_process_words(
        (uintptr_t)(cpu.ebp - 16U), guest_frame, 9U);
    fprintf(stderr,
            "host exception              : code=%08lx pc=%08lx access=%08lx op=%lu\n"
            "native stack                : esp=%08lx %s ret=%08x a1=%08x a2=%08x a3=%08x\n"
            "guest diagnostic registers  : esp=%08x ebp=%08x eax=%08x ecx=%08x edx=%08x\n"
            "guest frame -10..+10        : %s %08x %08x %08x %08x | %08x %08x %08x %08x %08x\n",
            record->ExceptionCode, context->Eip, (unsigned long)access,
            (unsigned long)operation, context->Esp,
            stack_readable ? "readable" : "unreadable",
            native_stack[0], native_stack[1], native_stack[2], native_stack[3],
            cpu.esp, cpu.ebp, cpu.eax, cpu.ecx, cpu.edx,
            frame_readable ? "readable" : "unreadable",
            guest_frame[0], guest_frame[1], guest_frame[2], guest_frame[3],
            guest_frame[4], guest_frame[5], guest_frame[6], guest_frame[7],
            guest_frame[8]);
    report_guest_frame_chain();
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), record->ExceptionCode);
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char **argv)
{
    guest_fn entry;
    uint32_t return_to = 0;
    size_t pc_gl_resolved = 0, pc_gl_missing = 0;
    int pc_kage_ready = 0, pc_gl_installed = 0, pc_gl_complete = 0;
    int stopped, ok;
    int gl_inventory = argc == 2 && strcmp(argv[1], "--gl-inventory") == 0;
    int pc_kage = argc == 2 && strcmp(argv[1], "--pc-kage") == 0;
    int pc_kage_visible = pc_kage &&
        GetEnvironmentVariableA("REPENTOGXM_PC_VISIBLE", NULL, 0) != 0;
    int past_main = gl_inventory || pc_kage ||
        (argc == 2 && strcmp(argv[1], "--past-main") == 0);

    setvbuf(stdout, NULL, _IONBF, 0);
    /* Top-level only: a vectored handler sees first-chance exceptions that a
     * Win32 API may handle itself and would turn a valid recovery into a kill. */
    SetUnhandledExceptionFilter(report_host_exception);
    printf("=== real entry-point smoke ===\n");
    guest_register_all_imports();
    if (guest_image_load(EXE_PATH)) return 2;
    if (guest_stack_init(&cpu)) return 2;
    guest_register_all();
    if (guest_coverage_init_from_env() != 0) {
        printf("semantic coverage          : FAILED %s\n",
               guest_coverage_error() ? guest_coverage_error() : "<unknown>");
        return 2;
    }

    tls_attach_status = -1;
    stopped = guest_run_until_stop(&cpu, attach_static_tls);
    if (stopped != 0 || tls_attach_status != 0) {
        printf("static TLS attach           : FAILED status=%d %08x %s\n",
               tls_attach_status, cpu.fault_addr,
               cpu.fault ? cpu.fault : "<none>");
        return 1;
    }
    printf("static TLS attach           : PASS\n");

    printf("dispatch/import tables      : %u / %u entries\n",
           guest_table_len, guest_import_table_len);
    entry = guest_lookup(ENTRY_RVA);
    if (!entry) {
        printf("entry lookup                : FAILED for %08x\n", ENTRY_RVA);
        return 2;
    }

    /* The native process loader would not expect this entry point to return,
     * but a sentinel makes an accidental return deterministic. */
    g_guest_checkpoint_rva = past_main ? 0U : APPLICATION_MAIN_RVA;
    g_guest_gl_inventory_mode = gl_inventory;
    if (pc_kage && !kage_pc_backend_set_mode(
            pc_kage_visible ? KAGE_PC_BACKEND_VISIBLE
                            : KAGE_PC_BACKEND_HIDDEN)) {
        printf("PC KAGE mode               : FAILED to arm backend\n");
        return 2;
    }
    printf("entry probe mode            : %s\n",
           gl_inventory ? "headless GL resolver inventory"
                         : (pc_kage ? (pc_kage_visible
                             ? "visible PC KAGE/GL handoff"
                             : "hidden PC KAGE/GL handoff")
                        : (past_main ? "past application main"
                                     : "stop at application main")));
    gpush(&cpu, 0xDEADBEEFu);
    stopped = guest_run_until_stop(&cpu, entry);
    if (cpu.esp)
        return_to = ld32(cpu.esp);

    printf("dynamic resolver requests  : %u calls, %u logged\n",
           g_guest_getproc_calls, g_guest_getproc_logged);
    {
        unsigned i;
        for (i = 0; i < g_guest_getproc_logged; ++i)
            printf("resolver request [%03u]    : %s\n", i,
                   guest_host_getproc_name(i));
    }

    printf("guarded run status          : %d (0 return, 1 fault, 2 exit)\n",
           stopped);
    if (stopped == GUEST_RUN_EXIT) {
        printf("guest process exit          : %s(%d)\n",
               cpu.exit_api ? cpu.exit_api : "<unknown>", cpu.exit_code);
    } else {
        printf("first startup fault         : %08x %s\n",
               cpu.fault_addr, cpu.fault ? cpu.fault : "<none>");
        report_guest_stop_state();
    }
    printf("guest return at stop        : %08x\n", return_to);

    if (pc_kage) {
        pc_kage_ready = kage_pc_backend_ready();
        pc_gl_installed = gl_pc_backend_installed();
        pc_gl_complete = gl_pc_backend_complete();
        pc_gl_resolved = gl_pc_backend_resolved_count();
        pc_gl_missing = gl_pc_backend_missing_count();
        printf("PC KAGE backend             : ready=%d GL=%s\n",
               pc_kage_ready, kage_pc_backend_gl_version());
        printf("PC typed GL backend         : installed=%d complete=%d resolved=%u missing=%u\n",
               pc_gl_installed, pc_gl_complete,
               (unsigned)pc_gl_resolved, (unsigned)pc_gl_missing);
        gl_pc_backend_uninstall();
        kage_pc_backend_shutdown();
    }

    if (past_main) {
        if (pc_kage) {
            const char *frontier = guest_import_name(cpu.fault_addr);
            /* Discovery accepts only a real named IAT boundary after the
             * complete PC graphics handoff.  It never blesses a translated
             * address miss, native exception, return, exit, or partial GL
             * backend as progress; frontier.py repeats and records the exact
             * observed tuple. */
            ok = stopped == GUEST_RUN_FAULT && cpu.fault && frontier &&
                 strcmp(cpu.fault, frontier) == 0 && return_to != 0U &&
                 pc_kage_ready && pc_gl_installed && pc_gl_complete &&
                 pc_gl_resolved != 0U && pc_gl_missing == 0U;
        } else if (gl_inventory) {
            const char *frontier = guest_import_name(cpu.fault_addr);
            /* Inventory may advance to a different named host boundary, but
             * it must not bless an arbitrary translated-code fault, guest
             * exit, native return, or timeout as progress.  Dynamic-token
             * frontiers get their own exact registry check when introduced. */
            ok = stopped == GUEST_RUN_FAULT && cpu.fault && frontier &&
                 strcmp(cpu.fault, frontier) == 0 && return_to != 0U;
        } else {
            ok = stopped == GUEST_RUN_FAULT && cpu.fault &&
                 cpu.fault_addr == GUEST_KAGE_RENDER_DISPLAY_RVA &&
                 strcmp(cpu.fault, GUEST_KAGE_RENDER_DISPLAY_FAULT) == 0 &&
                 return_to == EXPECTED_PAST_KAGE_RETURN;
        }
        printf("past-main frontier          : %s\n",
               pc_kage ? (ok ? "controlled named-import PC KAGE stop"
                              : "UNEXPECTED PC KAGE STOP")
               : ok ? (gl_inventory ? "controlled named-import inventory stop"
                                  : "high-level KAGE graphics boundary reached")
                  : "UNEXPECTED STOP");
        printf("VERDICT                     : %s\n", ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }

    ok = stopped == GUEST_RUN_FAULT && cpu.fault &&
         cpu.fault_addr == EXPECTED_TOKEN &&
         strcmp(cpu.fault, EXPECTED_NAME) == 0 &&
         return_to == EXPECTED_RETURN;
    printf("entry gate/path             : %s\n",
           ok ? "CRT init done; application main reached" : "UNEXPECTED");
    printf("VERDICT                     : %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
