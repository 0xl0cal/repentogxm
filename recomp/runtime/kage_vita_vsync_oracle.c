/* Host-executed oracle for the Vita requested/actual VSync split and the
 * production backend's belt-and-suspenders GL_FALSE policy. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kage_vita_backend.h"
#include "gl_vita_backend.h"
#include "kage_vita_backend_test_vitagl.h"
#include "manual_kage_vita_vsync.h"

static unsigned s_wait_calls;
static unsigned s_render_target_setup_calls;
static uint8_t s_render_target_scenes;
static uint8_t s_init_render_target_scenes;
static int s_init_ram_threshold;
static int s_init_cdram_threshold;
static int s_init_phycont_threshold;
static int s_init_cdlg_threshold;
static unsigned s_true_wait_calls;
static unsigned s_false_wait_calls;
static unsigned s_fake_set_calls;
static int s_fake_set_result = 1;
static int s_fake_set_last = -1;
static char s_logs[2048];
static size_t s_log_length;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Vita VSync oracle failed at line %d: %s\n", \
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
    return 1;
}

const GLubyte *glGetString(GLenum name)
{
    static const GLubyte version[] = "vsync-oracle-vitaGL";
    return name == GL_VERSION ? version : NULL;
}

void glViewport(int x, int y, GLsizei width, GLsizei height)
{
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

void vglWaitVblankStart(GLboolean enabled)
{
    ++s_wait_calls;
    if (enabled == GL_TRUE)
        ++s_true_wait_calls;
    else if (enabled == GL_FALSE)
        ++s_false_wait_calls;
    else
        s_true_wait_calls += 1000u;
}

void vglSwapBuffers(GLboolean has_common_dialog)
{
    (void)has_common_dialog;
}

int kage_vita_input_initialize(void) { return 1; }
void kage_vita_input_deactivate(void) {}
void isaac_vita_reset_archive_fread_epoch(void) {}
void kage_vita_loading_start(void) {}
void kage_vita_loading_finish(void) {}

static int fake_set_backend(int enabled)
{
    ++s_fake_set_calls;
    s_fake_set_last = enabled;
    return s_fake_set_result;
}

static void reset_fake_setter(int result)
{
    s_fake_set_calls = 0u;
    s_fake_set_last = -1;
    s_fake_set_result = result;
}

static int check_policy_transaction(void)
{
    uint8_t stored = 0xa5u;
    uint8_t actual = 0u;
    int changed = -1;

    reset_fake_setter(1);
    CHECK(guest_kage_vita_vsync_reconcile(
        1u, &stored, &actual, 0, fake_set_backend, &changed));
    CHECK(stored == 1u && actual == 0u && changed == 0);
    CHECK(s_fake_set_calls == 0u);

    /* A repeated requested=1 remains persisted but does no backend work. */
    CHECK(guest_kage_vita_vsync_reconcile(
        1u, &stored, &actual, 0, fake_set_backend, &changed));
    CHECK(stored == 1u && actual == 0u && changed == 0);
    CHECK(s_fake_set_calls == 0u);

    CHECK(guest_kage_vita_vsync_reconcile(
        0u, &stored, &actual, 0, fake_set_backend, &changed));
    CHECK(stored == 0u && actual == 0u && changed == 0);
    CHECK(s_fake_set_calls == 0u);

    /* A stale published actual=1 must be reconciled through backend false. */
    actual = 1u;
    reset_fake_setter(1);
    CHECK(guest_kage_vita_vsync_reconcile(
        1u, &stored, &actual, 0, fake_set_backend, &changed));
    CHECK(stored == 1u && actual == 0u && changed == 1);
    CHECK(s_fake_set_calls == 1u && s_fake_set_last == 0);

    /* Backend-only drift is repaired even if the guest already says zero. */
    actual = 0u;
    reset_fake_setter(1);
    CHECK(guest_kage_vita_vsync_reconcile(
        0u, &stored, &actual, 1, fake_set_backend, &changed));
    CHECK(stored == 0u && actual == 0u && changed == 1);
    CHECK(s_fake_set_calls == 1u && s_fake_set_last == 0);

    /* Failure preserves the newly requested option and the old guest actual,
     * so a later call retries the reconciliation. */
    actual = 1u;
    reset_fake_setter(0);
    CHECK(!guest_kage_vita_vsync_reconcile(
        0u, &stored, &actual, 0, fake_set_backend, &changed));
    CHECK(stored == 0u && actual == 1u && changed == 0);
    CHECK(s_fake_set_calls == 1u && s_fake_set_last == 0);
    return 0;
}

int main(void)
{
    CHECK(check_policy_transaction() == 0);

    /* The real backend fails loudly before initialization without touching
     * vitaGL, then initializes and remains actual-off for either request. */
    CHECK(!kage_vita_backend_set_vsync(1));
    CHECK(s_wait_calls == 0u);
    CHECK(kage_vita_backend_initialize(960u, 540u));
    CHECK(s_render_target_setup_calls == 1u);
    CHECK(s_render_target_scenes == 8u);
    CHECK(s_init_render_target_scenes == 8u);
    CHECK(s_init_ram_threshold == 0x01000000);
    CHECK(s_init_cdram_threshold == 0x00800000);
    CHECK(s_init_phycont_threshold == 0);
    CHECK(s_init_cdlg_threshold == 0x008c6000);
    CHECK(s_wait_calls == 1u && s_false_wait_calls == 1u);
    CHECK(s_true_wait_calls == 0u);
    CHECK(!kage_vita_backend_vsync_enabled());

    CHECK(kage_vita_backend_set_vsync(1));
    CHECK(s_wait_calls == 2u && s_false_wait_calls == 2u);
    CHECK(s_true_wait_calls == 0u);
    CHECK(!kage_vita_backend_vsync_enabled());
    CHECK(kage_vita_backend_vsync_change_count() == 1u);

    CHECK(kage_vita_backend_set_vsync(0));
    CHECK(s_wait_calls == 3u && s_false_wait_calls == 3u);
    CHECK(s_true_wait_calls == 0u);
    CHECK(!kage_vita_backend_vsync_enabled());
    CHECK(kage_vita_backend_vsync_change_count() == 2u);
    CHECK(strstr(s_logs,
        "vsync request=1 actual=0 policy=software-60hz") != NULL);
    CHECK(strstr(s_logs,
        "vsync request=0 actual=0 policy=software-60hz") != NULL);

    puts("Vita VSync host oracle: PASS (requested preserved; actual false; no GL_TRUE)");
    return 0;
}
