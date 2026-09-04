/* Host executable proving that host_vita_first_fault.c's same-TU weak no-op
 * is interposed by kage_vita_loading.c's strong fread-progress hook. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "guest.h"
#include "host_vita_com.h"
#include "host_vita_console.h"
#include "host_vita_crt.h"
#include "host_vita_exception.h"
#include "host_vita_file_lock.h"
#include "host_vita_filesystem.h"
#include "host_vita_find.h"
#include "host_vita_fls.h"
#include "host_vita_gl.h"
#include "host_vita_heap.h"
#include "host_vita_import_id.h"
#include "host_vita_math.h"
#include "host_vita_memory.h"
#include "host_vita_post_com.h"
#include "host_vita_rtti.h"
#include "host_vita_startup.h"
#include "host_vita_steam.h"
#include "host_vita_sync.h"
#include "host_vita_user32.h"
#include "kage_vita_loading.h"
#include "kage_vita_loading_test_vitagl.h"
#include "platform.h"
#include "vita_host_services.h"

extern int guest_host_import(CPU *__restrict c, const char *name);

#define ORACLE_FRAME_INTERVAL_US 233854u

static uint64_t s_time;
static unsigned s_swap_calls;
static unsigned s_stack_violation_calls;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Vita loading override oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int sceKernelGetThreadId(void) { return 7; }
uint64_t sceKernelGetProcessTimeWide(void) { return s_time; }
int sceGxmDisplayQueueFinish(void) { return 0; }
int sceClibPrintf(const char *format, ...)
{
    (void)format;
    return 0;
}
void vglSetDisplayCallback(void (*callback)(void *framebuffer))
{
    (void)callback;
}
void vglSwapBuffers(GLboolean has_common_dialog)
{
    (void)has_common_dialog;
    ++s_swap_calls;
}

int isaac_vita_crt_import_counted(CPU *__restrict c, const char *name,
                                  unsigned *call_count)
{
    (void)c;
    if (strcmp(name, ISAAC_VITA_CRT_FREAD_NAME) != 0)
        return 0;
    ++*call_count;
    return 1;
}

#define STUB_IMPORT(function) \
    int function(CPU *__restrict c, const char *name, unsigned *call_count) \
    { \
        (void)c; (void)name; (void)call_count; return 0; \
    }

STUB_IMPORT(isaac_vita_com_import_counted)
STUB_IMPORT(isaac_vita_console_import_counted)
STUB_IMPORT(isaac_vita_exception_import_counted)
STUB_IMPORT(isaac_vita_file_lock_import_counted)
STUB_IMPORT(isaac_vita_filesystem_import_counted)
STUB_IMPORT(isaac_vita_find_import_counted)
STUB_IMPORT(isaac_vita_fls_import_counted)
STUB_IMPORT(isaac_vita_gl_import_counted)
STUB_IMPORT(isaac_vita_heap_import_counted)
STUB_IMPORT(isaac_vita_math_import_counted)
STUB_IMPORT(isaac_vita_memory_import_counted)
STUB_IMPORT(isaac_vita_post_com_import_counted)
STUB_IMPORT(isaac_vita_rtti_import_counted)
STUB_IMPORT(isaac_vita_startup_import_counted)
STUB_IMPORT(isaac_vita_steam_import_counted)
STUB_IMPORT(isaac_vita_sync_import_counted)
STUB_IMPORT(isaac_vita_user32_import_counted)

int isaac_vita_gl_dynamic_counted(CPU *__restrict c, uint32_t token,
                                  unsigned *call_count)
{
    (void)c; (void)token; (void)call_count; return 0;
}

int isaac_vita_get_win32_filetime(uint64_t *filetime)
{
    *filetime = 0u;
    return 0;
}
uint32_t isaac_vita_get_thread_id(void) { return 7u; }
uint32_t isaac_vita_get_process_id(void) { return 1u; }
uint64_t isaac_vita_get_process_time(void) { return s_time; }
void isaac_vita_log(const char *format, ...)
{
    (void)format;
}
void guest_fault(CPU *__restrict c, uint32_t address, const char *message)
{
    c->fault_addr = address;
    c->fault = message;
}

/* The current generated-stack boundary keeps these cold helpers out of line.
 * This oracle never enters a stack-based import, but must still satisfy the
 * complete host_vita_first_fault.c link contract and prove neither path ran. */
int guest_stack_violation(CPU *__restrict c, uint32_t pc, uint32_t kind,
                          uint32_t address, uint32_t size)
{
    (void)c;
    (void)pc;
    (void)kind;
    (void)address;
    (void)size;
    ++s_stack_violation_calls;
    return 0;
}

int guest_stack_owner_violation(CPU *__restrict c, uint32_t pc)
{
    (void)c;
    (void)pc;
    ++s_stack_violation_calls;
    return 0;
}

int main(void)
{
    CPU cpu;
    unsigned index;

    memset(&cpu, 0, sizeof cpu);
    isaac_vita_reset_archive_fread_epoch();
    kage_vita_loading_start();
    CHECK(s_swap_calls == 1u &&
          kage_vita_loading_stage() == KAGE_VITA_LOADING_BOOT);

    /* guest_host_import is a single guest-CPU-owner boundary (CPU/gmem and
     * its public call counter are deliberately non-atomic).  Exercise the
     * fread cadence under that same production ownership contract. */
    s_time = ORACLE_FRAME_INTERVAL_US;
    for (index = 0u; index < 511u; ++index)
        CHECK(guest_host_import(&cpu, ISAAC_VITA_CRT_FREAD_NAME));
    CHECK(s_swap_calls == 1u &&
          kage_vita_loading_stage() == KAGE_VITA_LOADING_BOOT);
    CHECK(guest_host_import(&cpu, ISAAC_VITA_CRT_FREAD_NAME));
    CHECK(s_swap_calls == 2u &&
          kage_vita_loading_stage() == KAGE_VITA_LOADING_ARCHIVES);

    s_time = 2u * ORACLE_FRAME_INTERVAL_US;
    for (index = 512u; index < 24576u; ++index)
        CHECK(guest_host_import(&cpu, ISAAC_VITA_CRT_FREAD_NAME));

    /* The first archive stage at exactly call 512 pins the production polling
     * boundary and proves the strong hook, rather than the same-TU weak no-op,
     * ran from the real dispatcher.  Read count never becomes a percentage;
     * the time jump catches up frame state with one swap rather than a burst. */
    CHECK(kage_vita_loading_stage() == KAGE_VITA_LOADING_ARCHIVES);
    CHECK(kage_vita_loading_swap_count() == 3u && s_swap_calls == 3u);
    CHECK(g_host_import_calls == 24576u);
    CHECK(s_stack_violation_calls == 0u);
    kage_vita_loading_finish();

    /* Make the process-lifetime counter deliberately non-aligned, then prove
     * a new epoch starts its first loading poll at exactly local call 512. */
    CHECK(guest_host_import(&cpu, ISAAC_VITA_CRT_FREAD_NAME));
    isaac_vita_reset_archive_fread_epoch();
    kage_vita_loading_start();
    s_time = 3u * ORACLE_FRAME_INTERVAL_US;
    for (index = 0u; index < 511u; ++index)
        CHECK(guest_host_import(&cpu, ISAAC_VITA_CRT_FREAD_NAME));
    CHECK(kage_vita_loading_swap_count() == 1u && s_swap_calls == 4u);
    CHECK(guest_host_import(&cpu, ISAAC_VITA_CRT_FREAD_NAME));
    CHECK(kage_vita_loading_swap_count() == 2u && s_swap_calls == 5u);
    kage_vita_loading_finish();
    puts("Vita loading strong-over-weak fread counter oracle: PASS");
    return 0;
}
