/* Executable host oracle for process-lifetime vitaGL deactivate/reuse. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kage_vita_backend.h"
#include "gl_vita_backend.h"
#include "kage_vita_backend_test_vitagl.h"

enum oracle_event {
    ORACLE_EVENT_FREAD_RESET = 1,
    ORACLE_EVENT_STALL_START,
    ORACLE_EVENT_LOADING_START,
    ORACLE_EVENT_STALL_STOP,
    ORACLE_EVENT_LOADING_FINISH,
    ORACLE_EVENT_INPUT_DEACTIVATE
};

static unsigned s_events[16];
static unsigned s_event_count;
static unsigned s_swap_calls;
static unsigned s_input_initialize_calls;
static unsigned s_render_target_setup_calls;
static uint8_t s_render_target_scenes;
static uint8_t s_init_render_target_scenes;
static int s_init_ram_threshold;
static int s_init_cdram_threshold;
static int s_init_phycont_threshold;
static int s_init_cdlg_threshold;
static char s_logs[4096];
static size_t s_log_length;
static int s_max_log_result;
static int s_stall_start_result = 1;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Vita backend teardown oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static void oracle_event(unsigned event)
{
    if (s_event_count < sizeof s_events / sizeof s_events[0])
        s_events[s_event_count++] = event;
}

int sceClibPrintf(const char *format, ...)
{
    va_list arguments;
    int result;
    size_t available = sizeof s_logs - s_log_length;

    va_start(arguments, format);
    result = vsnprintf(s_logs + s_log_length, available, format, arguments);
    va_end(arguments);
    if (result > s_max_log_result)
        s_max_log_result = result;
    if (result > 0 && available > 0u) {
        size_t written = (size_t)result;
        if (written >= available)
            written = available - 1u;
        s_log_length += written;
    }
    return result;
}

uint64_t sceKernelGetProcessTimeWide(void) { return 1000000u; }

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
    ++s_render_target_setup_calls;
    s_render_target_scenes = scenes_per_frame;
}

/* 0007 hook stand-in (stock profile only; the overlay profile never calls
 * it): stores 1..8 and returns the value in effect. */
uint8_t vglIsaacSetupFboRenderTargetScenes(uint8_t size)
{
    static uint8_t scenes = 1u;

    if (size >= 1u && size <= 8u)
        scenes = size;
    return scenes;
}

/* 0009 hook stand-in: 0 = observe, nonzero = apply; returns the mode. */
uint8_t vglIsaacSetupFboValidRegion(uint8_t apply)
{
    return apply ? 1u : 0u;
}

GLboolean vglInitWithCustomThreshold(
    int legacy_pool, int width, int height,
    int ram_threshold, int cdram_threshold,
    int phycont_threshold, int cdlg_threshold, int multisample_mode)
{
    (void)legacy_pool;
    (void)width;
    (void)height;
    (void)multisample_mode;
    s_init_render_target_scenes = s_render_target_scenes;
    s_init_ram_threshold = ram_threshold;
    s_init_cdram_threshold = cdram_threshold;
    s_init_phycont_threshold = phycont_threshold;
    s_init_cdlg_threshold = cdlg_threshold;
    return GL_FALSE;
}

int gl_vita_backend_get_display_surface_status(
    IsaacVitaGlDisplaySurfaceStatus *status)
{
#if defined(ISAAC_KAGE_DISPLAY_STATUS_FAILURE_CASE) && \
        ISAAC_KAGE_DISPLAY_STATUS_FAILURE_CASE == 1
    (void)status;
    return 0;
#else
    unsigned i;

    memset(status, 0, sizeof(*status));
    status->size = sizeof(*status);
    status->display_size = 0x00200000u;
    status->buffer_count = ISAAC_VITAGL_DISPLAY_SURFACE_COUNT;
    status->dedicated_count = ISAAC_VITAGL_DISPLAY_SURFACE_COUNT;
    status->failure_index = UINT32_MAX;
    for (i = 0; i < ISAAC_VITAGL_DISPLAY_SURFACE_COUNT; ++i) {
        status->alloc_result[i] = (int32_t)(0x7000u + i);
        status->dedicated[i] = 1u;
    }
#if defined(ISAAC_KAGE_DISPLAY_STATUS_FAILURE_CASE)
# if ISAAC_KAGE_DISPLAY_STATUS_FAILURE_CASE == 2
    status->size--;
# elif ISAAC_KAGE_DISPLAY_STATUS_FAILURE_CASE == 3
    status->buffer_count = 5u;
# elif ISAAC_KAGE_DISPLAY_STATUS_FAILURE_CASE == 4
    status->dedicated_count = 2u;
    status->dedicated[2] = 0u;
# elif ISAAC_KAGE_DISPLAY_STATUS_FAILURE_CASE == 5
    status->map_result[1] = (int32_t)0x805b0003u;
# else
#  error ISAAC_KAGE_DISPLAY_STATUS_FAILURE_CASE must be in 1..5
# endif
#endif
    return 1;
#endif
}

const GLubyte *glGetString(GLenum name)
{
    static const GLubyte version[] = "oracle-vitaGL";
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
    if (has_common_dialog != GL_FALSE)
        s_swap_calls = 1000u;
    ++s_swap_calls;
}

int kage_vita_input_initialize(void)
{
    ++s_input_initialize_calls;
    return 1;
}

void kage_vita_input_deactivate(void)
{
    oracle_event(ORACLE_EVENT_INPUT_DEACTIVATE);
}

void isaac_vita_reset_archive_fread_epoch(void)
{
    oracle_event(ORACLE_EVENT_FREAD_RESET);
}
void kage_vita_loading_start(void)
{
    oracle_event(ORACLE_EVENT_LOADING_START);
}
void kage_vita_loading_finish(void)
{
    oracle_event(ORACLE_EVENT_LOADING_FINISH);
}

int kage_vita_stall_probe_start(void)
{
    oracle_event(ORACLE_EVENT_STALL_START);
    return s_stall_start_result;
}
void kage_vita_stall_probe_stop(void)
{
    oracle_event(ORACLE_EVENT_STALL_STOP);
}
void kage_vita_stall_note_present_enter(unsigned present_count)
{
    (void)present_count;
}
void kage_vita_stall_note_present_return(unsigned present_count)
{
    (void)present_count;
}

int main(void)
{
#if defined(ISAAC_KAGE_DISPLAY_STATUS_FAILURE_CASE)
    CHECK(!kage_vita_backend_initialize(960u, 540u));
    CHECK(!kage_vita_backend_ready());
    CHECK(strcmp(kage_vita_backend_last_error(),
        "vitaGL did not create three dedicated display surfaces") == 0);
    CHECK(s_render_target_setup_calls == 1u);
    CHECK(s_init_render_target_scenes == 8u);
    CHECK(s_init_cdram_threshold == 0x00800000);
    CHECK(s_swap_calls == 0u);
    CHECK(s_event_count == 1u);
    CHECK(s_events[0] == ORACLE_EVENT_INPUT_DEACTIVATE);
    puts("Vita display-surface fail-closed initialize oracle: PASS");
    return 0;
#else
    unsigned input_calls_before;

#if defined(ISAAC_VITA_IO_PROFILE)
    kage_vita_backend_memory_snapshot_at_first_fbo(
        0, (const void *)(uintptr_t)0x81234000u);
    /* The endpoint is one-shot even if vitaGL reports the event twice. */
    kage_vita_backend_memory_snapshot_at_first_fbo(
        0, (const void *)(uintptr_t)0x81234000u);
#endif
    CHECK(kage_vita_backend_initialize(960u, 540u));
    CHECK(kage_vita_backend_ready());
    CHECK(s_render_target_setup_calls == 1u);
    CHECK(s_render_target_scenes == 8u);
    CHECK(s_init_render_target_scenes == 8u);
    CHECK(s_init_ram_threshold == 0x01000000);
    CHECK(s_init_cdram_threshold == 0x00800000);
    CHECK(s_init_phycont_threshold == 0);
    CHECK(s_init_cdlg_threshold == 0x008c6000);
    CHECK(s_swap_calls == 0u);
    CHECK(s_event_count == 3u);
    CHECK(s_events[0] == ORACLE_EVENT_FREAD_RESET);
    CHECK(s_events[1] == ORACLE_EVENT_STALL_START);
    CHECK(s_events[2] == ORACLE_EVENT_LOADING_START);
#if defined(ISAAC_VITA_IO_PROFILE)
    CHECK(strstr(s_logs,
        "p=first-1024-fbo-event k=0x00000000") != NULL);
    CHECK(strstr(s_logs,
        "kf(u/c/p)=4294967295/4294967295/4294967295") != NULL);
    CHECK(strstr(s_logs,
        "vf/t(r/v/p/b)=4294967295/4294967295,") != NULL);
    CHECK(strstr(s_logs,
        "4294967295/4294967295,4294967295/4294967295,") != NULL);
    CHECK(strstr(s_logs,
        "rq=0x00000000 rb=4294967295 ev=0x00000000 ") != NULL);
    CHECK(strstr(s_logs, "t=0x81234000 rt=1024x1024/8/0") != NULL);
    CHECK(strstr(s_logs,
        "p=vitagl-init-return k=0x00000000") != NULL);
    CHECK(strstr(s_logs,
#if defined(ISAAC_VITA_DISPLAY_RASTER_720)
        "rt=1024x1024/8/0 init=0/720/408/16777216/0") != NULL);
#else
        "rt=1024x1024/8/0 init=0/960/544/16777216/0") != NULL);
#endif
    if (s_max_log_result != (int)KAGE_VITA_MEMORY_SNAPSHOT_MAX_BODY) {
        fprintf(stderr, "memory snapshot max body: got %d expected %u\n",
                s_max_log_result,
                (unsigned)KAGE_VITA_MEMORY_SNAPSHOT_MAX_BODY);
    }
    CHECK(s_max_log_result == (int)KAGE_VITA_MEMORY_SNAPSHOT_MAX_BODY);
#endif

    /* Finishing the loading presentation does not deactivate the backend.
     * Repeated Initialize calls in that state are idempotent and must not
     * start a stale-counter loading/watchdog epoch. */
    kage_vita_loading_finish();
    CHECK(s_event_count == 4u);
    CHECK(s_events[3] == ORACLE_EVENT_LOADING_FINISH);
    input_calls_before = s_input_initialize_calls;
    CHECK(kage_vita_backend_initialize(960u, 540u));
    CHECK(kage_vita_backend_initialize(960u, 540u));
    CHECK(kage_vita_backend_ready());
    CHECK(s_input_initialize_calls == input_calls_before + 2u);
    CHECK(s_event_count == 4u);

    kage_vita_backend_deactivate();
    CHECK(!kage_vita_backend_ready());
    CHECK(s_event_count == 7u);
    CHECK(s_events[4] == ORACLE_EVENT_STALL_STOP);
    CHECK(s_events[5] == ORACLE_EVENT_LOADING_FINISH);
    CHECK(s_events[6] == ORACLE_EVENT_INPUT_DEACTIVATE);
    CHECK(s_swap_calls == 0u);

    input_calls_before = s_input_initialize_calls;
    s_stall_start_result = 0;
    CHECK(kage_vita_backend_initialize(960u, 540u));
    CHECK(kage_vita_backend_ready());
    CHECK(s_render_target_setup_calls == 1u);
    CHECK(s_render_target_scenes == 8u);
    CHECK(s_init_render_target_scenes == 8u);
    CHECK(s_input_initialize_calls == input_calls_before + 1u);
    CHECK(s_swap_calls == 0u);
    CHECK(s_event_count == 10u);
    CHECK(s_events[7] == ORACLE_EVENT_FREAD_RESET);
    CHECK(s_events[8] == ORACLE_EVENT_STALL_START);
    CHECK(s_events[9] == ORACLE_EVENT_LOADING_START);
    kage_vita_backend_deactivate();
    CHECK(!kage_vita_backend_ready());
    CHECK(s_event_count == 13u);
    CHECK(s_events[10] == ORACLE_EVENT_STALL_STOP);
    CHECK(s_events[11] == ORACLE_EVENT_LOADING_FINISH);
    CHECK(s_events[12] == ORACLE_EVENT_INPUT_DEACTIVATE);

    puts("Vita process-lifetime backend host oracle: PASS (deactivate -> no swap -> in-process reuse)");
    return 0;
#endif
}
