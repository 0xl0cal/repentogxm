/* Host oracle for the cadence lifecycle at the production Vita boundary. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kage_vita_backend.h"
#include "kage_vita_backend_test_vitagl.h"
#include "kage_vita_fullspeed_scheduler.h"

static uint64_t s_now_us = 1000000u;
static int s_input_initialize_result = 1;
static unsigned s_loading_starts;
static unsigned s_loading_finishes;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "cadence lifecycle oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int sceClibPrintf(const char *format, ...)
{
    (void)format;
    return 0;
}

uint64_t sceKernelGetProcessTimeWide(void) { return s_now_us; }
uint64_t isaac_vita_get_process_time(void) { return s_now_us; }

int sceKernelDelayThread(unsigned int delay_us)
{
    s_now_us += delay_us;
    return 0;
}

void isaac_vita_log(const char *format, ...)
{
    (void)format;
}

void vglSetupRuntimeShaderCompiler(
    int optimization, int vertex, int fragment, int compiler)
{
    (void)optimization;
    (void)vertex;
    (void)fragment;
    (void)compiler;
}

void vglSetupDisplayRenderTarget(uint8_t scenes_per_frame)
{
    (void)scenes_per_frame;
}

GLboolean vglInitExtended(
    int legacy_pool, int width, int height,
    int ram_threshold, int multisample_mode)
{
    (void)legacy_pool;
    (void)width;
    (void)height;
    (void)ram_threshold;
    (void)multisample_mode;
    return GL_FALSE;
}

GLboolean vglInitWithCustomThreshold(
    int legacy_pool, int width, int height,
    int ram_threshold, int cdram_threshold,
    int phycont_threshold, int cdlg_threshold, int multisample_mode)
{
    (void)legacy_pool;
    (void)width;
    (void)height;
    (void)ram_threshold;
    (void)cdram_threshold;
    (void)phycont_threshold;
    (void)cdlg_threshold;
    (void)multisample_mode;
    return GL_FALSE;
}

const GLubyte *glGetString(GLenum name)
{
    static const GLubyte version[] = "cadence-lifecycle-oracle-vitaGL";
    return name == GL_VERSION ? version : NULL;
}

void glViewport(int x, int y, GLsizei width, GLsizei height)
{
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

void vglWaitVblankStart(GLboolean enabled) { (void)enabled; }
void vglSwapBuffers(GLboolean has_common_dialog)
{
    (void)has_common_dialog;
}

int kage_vita_input_initialize(void)
{
    return s_input_initialize_result;
}

void kage_vita_input_deactivate(void) {}
void isaac_vita_reset_archive_fread_epoch(void) {}
void kage_vita_loading_start(void) { ++s_loading_starts; }
void kage_vita_loading_finish(void) { ++s_loading_finishes; }

#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
static int snapshot_is_zero(const KageVitaFullspeedSnapshot *snapshot)
{
    KageVitaFullspeedSnapshot zero;

    memset(&zero, 0, sizeof zero);
    return memcmp(snapshot, &zero, sizeof zero) == 0;
}
#endif

int main(void)
{
    CHECK(!kage_vita_backend_initialize(959u, 540u));
    CHECK(!kage_vita_backend_ready());

#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
    {
        KageVitaFullspeedSnapshot before;
        KageVitaFullspeedSnapshot after;

        kage_vita_fullspeed_scheduler_snapshot(&before);
        CHECK(snapshot_is_zero(&before));
        CHECK(!kage_vita_fullspeed_scheduler_bypass_limiter());

        /* A rejected Initialize must not arm cadence. */
        kage_vita_fullspeed_scheduler_note_loop_head();
        CHECK(!kage_vita_fullspeed_scheduler_bypass_limiter());
        CHECK(kage_vita_fullspeed_scheduler_plan_render(
            1u, 1u, 0u, 2u, 0u));

        CHECK(kage_vita_backend_initialize(960u, 540u));
        CHECK(kage_vita_backend_ready());
        CHECK(s_loading_starts == 1u);
        kage_vita_fullspeed_scheduler_snapshot(&after);
        CHECK(snapshot_is_zero(&after));

        /* The first real loop makes the freshly reset scheduler authoritative. */
        kage_vita_fullspeed_scheduler_note_loop_head();
        CHECK(kage_vita_fullspeed_scheduler_bypass_limiter());
        kage_vita_fullspeed_scheduler_snapshot(&before);
        CHECK(before.wrapper_ticks == 1u);

        /* A duplicate observation disables cadence and fails open. */
        kage_vita_fullspeed_scheduler_note_service();
        kage_vita_fullspeed_scheduler_note_service();
        CHECK(!kage_vita_fullspeed_scheduler_bypass_limiter());
        CHECK(kage_vita_fullspeed_scheduler_plan_render(
            1u, 1u, 0u, 2u, 0u));
        kage_vita_fullspeed_scheduler_snapshot(&before);
        CHECK(before.duplicate_tick_violations == 1u);
        CHECK(before.runtime_disables == 1u);

        /* An idempotent Initialize in the same active session must not reset. */
        CHECK(kage_vita_backend_initialize(960u, 540u));
        kage_vita_fullspeed_scheduler_snapshot(&after);
        CHECK(memcmp(&before, &after, sizeof before) == 0);
        CHECK(!kage_vita_fullspeed_scheduler_bypass_limiter());

        kage_vita_backend_deactivate();
        CHECK(!kage_vita_backend_ready());
        CHECK(s_loading_finishes == 1u);
        kage_vita_fullspeed_scheduler_snapshot(&after);
        CHECK(snapshot_is_zero(&after));
        CHECK(!kage_vita_fullspeed_scheduler_bypass_limiter());
        CHECK(kage_vita_fullspeed_scheduler_plan_render(
            1u, 1u, 0u, 2u, 0u));

        /* A failed reactivation remains disarmed. */
        s_input_initialize_result = 0;
        CHECK(!kage_vita_backend_initialize(960u, 540u));
        CHECK(!kage_vita_backend_ready());
        CHECK(!kage_vita_fullspeed_scheduler_bypass_limiter());

        /* A real inactive -> active transition starts a clean epoch. */
        s_input_initialize_result = 1;
        CHECK(kage_vita_backend_initialize(960u, 540u));
        CHECK(s_loading_starts == 2u);
        kage_vita_fullspeed_scheduler_snapshot(&after);
        CHECK(snapshot_is_zero(&after));
        kage_vita_fullspeed_scheduler_note_loop_head();
        CHECK(kage_vita_fullspeed_scheduler_bypass_limiter());
        kage_vita_fullspeed_scheduler_snapshot(&after);
        CHECK(after.wrapper_ticks == 1u);
        CHECK(after.runtime_disables == 0u);
        kage_vita_backend_deactivate();
        CHECK(!kage_vita_fullspeed_scheduler_bypass_limiter());
    }

    puts("Vita cadence production-boundary lifecycle oracle: PASS "
         "(reset/deactivate/reinit/fail-open; active init idempotent)");
#else
    CHECK(kage_vita_backend_initialize(960u, 540u));
    CHECK(kage_vita_backend_ready());
    CHECK(kage_vita_backend_initialize(960u, 540u));
    kage_vita_backend_deactivate();
    CHECK(!kage_vita_backend_ready());
    s_input_initialize_result = 0;
    CHECK(!kage_vita_backend_initialize(960u, 540u));
    s_input_initialize_result = 1;
    CHECK(kage_vita_backend_initialize(960u, 540u));
    kage_vita_backend_deactivate();
    puts("Vita cadence scheduler-OFF boundary oracle: PASS "
         "(no scheduler object or runtime edge)");
#endif
    return 0;
}
