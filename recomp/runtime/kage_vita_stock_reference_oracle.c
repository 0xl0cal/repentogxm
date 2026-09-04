/* Host-executed oracle for the stock vitaGL initialization/display profile. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gl_vita_backend.h"
#include "kage_vita_backend.h"
#include "kage_vita_backend_test_vitagl.h"

static unsigned s_shader_setup_calls;
static int s_shader_optimization;
static int s_shader_vertex;
static int s_shader_fragment;
static int s_shader_compiler;
static unsigned s_display_target_setup_calls;
static unsigned s_extended_init_calls;
static unsigned s_custom_init_calls;
static int s_init_legacy_pool;
static int s_init_width;
static int s_init_height;
static int s_init_ram_threshold;
static int s_init_msaa;
static unsigned s_viewport_calls;
static int s_viewport_x;
static int s_viewport_y;
static GLsizei s_viewport_width;
static GLsizei s_viewport_height;
static unsigned s_surface_status_calls;
static unsigned s_swap_calls;
static unsigned s_wait_calls;
static GLboolean s_last_wait = GL_TRUE;
static unsigned s_loading_start_calls;
static unsigned s_loading_finish_calls;
static char s_logs[2048];
static size_t s_log_length;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "stock vitaGL oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int sceClibPrintf(const char *format, ...)
{
    va_list arguments;
    int result;
    size_t available = sizeof s_logs - s_log_length;

    va_start(arguments, format);
    result = vsnprintf(s_logs + s_log_length, available, format, arguments);
    va_end(arguments);
    if (result > 0 && available != 0u) {
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
    ++s_shader_setup_calls;
    s_shader_optimization = optimization;
    s_shader_vertex = vertex;
    s_shader_fragment = fragment;
    s_shader_compiler = compiler;
}

void vglSetupDisplayRenderTarget(uint8_t scenes_per_frame)
{
    (void)scenes_per_frame;
    ++s_display_target_setup_calls;
}

GLboolean vglInitExtended(
    int legacy_pool, int width, int height,
    int ram_threshold, int multisample_mode)
{
    ++s_extended_init_calls;
    s_init_legacy_pool = legacy_pool;
    s_init_width = width;
    s_init_height = height;
    s_init_ram_threshold = ram_threshold;
    s_init_msaa = multisample_mode;
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
    ++s_custom_init_calls;
    return GL_FALSE;
}

int gl_vita_backend_get_display_surface_status(
    IsaacVitaGlDisplaySurfaceStatus *status)
{
    (void)status;
    ++s_surface_status_calls;
    return 0;
}

const GLubyte *glGetString(GLenum name)
{
    static const GLubyte version[] = "stock-reference-oracle-vitaGL";
    return name == GL_VERSION ? version : NULL;
}

void glViewport(int x, int y, GLsizei width, GLsizei height)
{
    ++s_viewport_calls;
    s_viewport_x = x;
    s_viewport_y = y;
    s_viewport_width = width;
    s_viewport_height = height;
}

void vglWaitVblankStart(GLboolean enabled)
{
    ++s_wait_calls;
    s_last_wait = enabled;
}

void vglSwapBuffers(GLboolean has_common_dialog)
{
    (void)has_common_dialog;
    ++s_swap_calls;
}

int kage_vita_input_initialize(void) { return 1; }
void kage_vita_input_deactivate(void) {}
void isaac_vita_reset_archive_fread_epoch(void) {}
void kage_vita_loading_start(void) { ++s_loading_start_calls; }
void kage_vita_loading_finish(void) { ++s_loading_finish_calls; }

int main(void)
{
    unsigned viewport_calls_before;

    CHECK(!kage_vita_backend_initialize(959u, 540u));
    CHECK(s_shader_setup_calls == 0u);
    CHECK(s_extended_init_calls == 0u);

    CHECK(kage_vita_backend_initialize(960u, 540u));
    CHECK(s_shader_setup_calls == 1u);
    CHECK(s_shader_optimization == SHARK_OPT_UNSAFE);
    CHECK(s_shader_vertex == SHARK_ENABLE);
    CHECK(s_shader_fragment == SHARK_ENABLE);
    CHECK(s_shader_compiler == SHARK_ENABLE);
    CHECK(s_display_target_setup_calls == 0u);
    CHECK(s_extended_init_calls == 1u);
    CHECK(s_custom_init_calls == 0u);
    CHECK(s_init_legacy_pool == 0);
#if defined(ISAAC_VITA_DISPLAY_RASTER_720)
    CHECK(s_init_width == 720 && s_init_height == 408);
#else
    CHECK(s_init_width == 960 && s_init_height == 544);
#endif
    CHECK(s_init_ram_threshold == 0x01000000);
    CHECK(s_init_msaa == SCE_GXM_MULTISAMPLE_NONE);
    CHECK(s_surface_status_calls == 0u);
    CHECK(s_viewport_calls == 1u && s_viewport_x == 0);
#if defined(ISAAC_VITA_DISPLAY_RASTER_720)
    CHECK(s_viewport_y == 1);
    CHECK(s_viewport_width == 720 && s_viewport_height == 405);
#else
    CHECK(s_viewport_y == 2);
    CHECK(s_viewport_width == 960 && s_viewport_height == 540);
#endif
    CHECK(s_wait_calls == 1u && s_last_wait == GL_FALSE);
    CHECK(s_loading_start_calls == 1u);
    CHECK(strstr(s_logs,
        "profile=stock-vitagl-reference source=73dd57a") != NULL);
#if defined(ISAAC_VITA_DISPLAY_RASTER_720)
    CHECK(strstr(s_logs,
        "display=720x408 panel=960x544 logical=960x540") != NULL);
#else
    CHECK(strstr(s_logs, "physical=960x544 logical=960x540") != NULL);
#endif

    CHECK(kage_vita_backend_present());
    CHECK(s_swap_calls == 1u);
    CHECK(s_surface_status_calls == 0u);
    CHECK(s_loading_finish_calls == 1u);
    CHECK(strstr(s_logs, "first present complete") != NULL);

    kage_vita_backend_deactivate();
    CHECK(!kage_vita_backend_ready());
    viewport_calls_before = s_viewport_calls;
    CHECK(kage_vita_backend_initialize(960u, 540u));
#if defined(ISAAC_VITA_DISPLAY_RASTER_720)
    CHECK(s_viewport_calls == viewport_calls_before + 1u);
    CHECK(s_viewport_y == 1);
    CHECK(s_viewport_width == 720 && s_viewport_height == 405);
#else
    CHECK(s_viewport_calls == viewport_calls_before);
#endif

#if defined(ISAAC_VITA_DISPLAY_RASTER_720)
    puts("Stock vitaGL host oracle: PASS (720x408 init; 720x405 viewport; 960x540 logical)");
#else
    puts("Stock vitaGL host oracle: PASS (960x544 init; display raster OFF)");
#endif
    return 0;
}
