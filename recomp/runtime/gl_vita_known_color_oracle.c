#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl_bridge.h"
#include "gl_vita_backend.h"
#include "gl_vita_first_frame_oracle_vitagl.h"

#define FF_FRAMEBUFFER              0x8d40u
#define FF_READ_FRAMEBUFFER         0x8ca8u
#define FF_DRAW_FRAMEBUFFER         0x8ca9u
#define FF_FRAMEBUFFER_BINDING      0x8ca6u
#define FF_READ_FRAMEBUFFER_BINDING 0x8caau
#define FF_VIEWPORT                 0x0ba2u
#define FF_SCISSOR_TEST             0x0c11u
#define FF_COLOR_CLEAR_VALUE        0x0c22u
#define FF_COLOR_WRITEMASK          0x0c23u
#define FF_COLOR_BUFFER_BIT         0x4000u
#define FF_FRAMEBUFFER_COMPLETE     0x8cd5u

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "known-color oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static GLuint s_read_fbo;
static GLuint s_draw_fbo;
static GLint s_viewport[4];
static GLboolean s_color_mask[4];
static GLfloat s_clear_color[4];
static GLboolean s_scissor;
static GLenum s_error;
static unsigned s_bind_calls;
static unsigned s_disable_calls;
static unsigned s_enable_calls;
static unsigned s_clear_calls;
static unsigned s_green_clear_calls;
static char s_logs[16][384];
static unsigned s_log_count;
static unsigned s_log_truncated;
static unsigned s_max_log_length;

int kage_vita_backend_ready(void) { return 1; }

void guest_fault(CPU *__restrict cpu, uint32_t address, const char *message)
{
    cpu->fault_addr = address;
    cpu->fault = message;
    abort();
}

void isaac_gl_vita_oracle_rt_log(const char *format, ...)
{
    (void)format;
}

void isaac_gl_vita_oracle_rt_profile_report(const char *reason)
{
    (void)reason;
}

void isaac_gl_vita_oracle_rt_fault(uint32_t result, const char *message)
{
    (void)result;
    (void)message;
}

void isaac_gl_vita_first_frame_oracle_log(const char *format, ...)
{
    va_list arguments;
    int length;
    unsigned slot = s_log_count < 16u ? s_log_count : 15u;

    va_start(arguments, format);
    length = vsnprintf(s_logs[slot], sizeof(s_logs[slot]), format, arguments);
    va_end(arguments);
    s_logs[slot][sizeof(s_logs[slot]) - 1u] = '\0';
    if (length < 0 || (size_t)length >= sizeof(s_logs[slot]))
        ++s_log_truncated;
    if (length > 0 && (unsigned)length > s_max_log_length)
        s_max_log_length = (unsigned)length;
    if (s_log_count < 16u)
        ++s_log_count;
}

void oracle_ff_glNoop(void) {}
GLuint oracle_ff_glReturnUint(void) { return 1u; }
GLint oracle_ff_glReturnInt(void) { return 0; }
const GLubyte *oracle_ff_glReturnString(void)
{
    static const GLubyte value[] = "known-color-oracle";
    return value;
}

void oracle_ff_glBindFramebuffer(GLenum target, GLuint framebuffer)
{
    ++s_bind_calls;
    if (target == FF_FRAMEBUFFER) {
        s_read_fbo = framebuffer;
        s_draw_fbo = framebuffer;
    } else if (target == FF_READ_FRAMEBUFFER) {
        s_read_fbo = framebuffer;
    } else if (target == FF_DRAW_FRAMEBUFFER) {
        s_draw_fbo = framebuffer;
    }
}

void oracle_ff_glClear(GLbitfield mask)
{
    ++s_clear_calls;
    if (mask == FF_COLOR_BUFFER_BIT && s_draw_fbo == 0u &&
            s_viewport[0] == 0 && s_viewport[1] == 0 &&
            s_viewport[2] == 960 && s_viewport[3] == 544 &&
            !s_scissor && s_color_mask[0] && s_color_mask[1] &&
            s_color_mask[2] && s_color_mask[3] &&
            s_clear_color[0] == 0.0f && s_clear_color[1] == 1.0f &&
            s_clear_color[2] == 0.0f && s_clear_color[3] == 1.0f)
        ++s_green_clear_calls;
}

void oracle_ff_glClearColor(
    GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
    s_clear_color[0] = red;
    s_clear_color[1] = green;
    s_clear_color[2] = blue;
    s_clear_color[3] = alpha;
}

void oracle_ff_glColorMask(
    GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha)
{
    s_color_mask[0] = red;
    s_color_mask[1] = green;
    s_color_mask[2] = blue;
    s_color_mask[3] = alpha;
}

void oracle_ff_glDisable(GLenum capability)
{
    (void)capability;
    ++s_disable_calls;
}

void oracle_ff_glDrawElements(
    GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    (void)mode;
    (void)count;
    (void)type;
    (void)indices;
}

void oracle_ff_glEnable(GLenum capability)
{
    (void)capability;
    ++s_enable_calls;
}

void oracle_ff_glFinish(void) {}

void oracle_ff_glFramebufferTexture2D(
    GLenum target, GLenum attachment, GLenum texture_target,
    GLuint texture, GLint level)
{
    (void)target;
    (void)attachment;
    (void)texture_target;
    (void)texture;
    (void)level;
}

void oracle_ff_glGetBooleanv(GLenum name, GLboolean *values)
{
    if (name == FF_COLOR_WRITEMASK)
        memcpy(values, s_color_mask, sizeof(s_color_mask));
}

GLenum oracle_ff_glGetError(void)
{
    GLenum result = s_error;
    s_error = 0u;
    return result;
}

void oracle_ff_glGetFloatv(GLenum name, GLfloat *values)
{
    if (name == FF_COLOR_CLEAR_VALUE)
        memcpy(values, s_clear_color, sizeof(s_clear_color));
}

void oracle_ff_glGetIntegerv(GLenum name, GLint *values)
{
    if (name == FF_FRAMEBUFFER_BINDING)
        *values = (GLint)s_draw_fbo;
    else if (name == FF_READ_FRAMEBUFFER_BINDING)
        *values = (GLint)s_read_fbo;
    else if (name == FF_VIEWPORT)
        memcpy(values, s_viewport, sizeof(s_viewport));
}

GLboolean oracle_ff_glIsEnabled(GLenum capability)
{
    return capability == FF_SCISSOR_TEST ? s_scissor : 0u;
}

void oracle_ff_glReadPixels(
    GLint x, GLint y, GLsizei width, GLsizei height,
    GLenum format, GLenum type, void *pixels)
{
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)format;
    (void)type;
    if (pixels)
        *(uint32_t *)pixels = 0u;
}

void oracle_ff_glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
    s_viewport[0] = x;
    s_viewport[1] = y;
    s_viewport[2] = width;
    s_viewport[3] = height;
}

GLenum oracle_ff_glCheckNamedFramebufferStatus(
    GLuint framebuffer, GLenum target)
{
    (void)framebuffer;
    (void)target;
    return FF_FRAMEBUFFER_COMPLETE;
}

void oracle_ff_glBlitNamedFramebuffer(
    GLuint read_framebuffer, GLuint draw_framebuffer,
    GLint src_x0, GLint src_y0, GLint src_x1, GLint src_y1,
    GLint dst_x0, GLint dst_y0, GLint dst_x1, GLint dst_y1,
    GLbitfield mask, GLenum filter)
{
    (void)read_framebuffer;
    (void)draw_framebuffer;
    (void)src_x0;
    (void)src_y0;
    (void)src_x1;
    (void)src_y1;
    (void)dst_x0;
    (void)dst_y0;
    (void)dst_x1;
    (void)dst_y1;
    (void)mask;
    (void)filter;
}

static void reset_mock_state(void)
{
    s_read_fbo = 17u;
    s_draw_fbo = 0u;
    s_viewport[0] = 11;
    s_viewport[1] = 12;
    s_viewport[2] = 913;
    s_viewport[3] = 517;
    s_color_mask[0] = 1u;
    s_color_mask[1] = 0u;
    s_color_mask[2] = 1u;
    s_color_mask[3] = 0u;
    s_clear_color[0] = 0.1f;
    s_clear_color[1] = 0.2f;
    s_clear_color[2] = 0.3f;
    s_clear_color[3] = 0.4f;
    s_scissor = 0u;
    s_error = 0u;
    s_bind_calls = 0u;
    s_disable_calls = 0u;
    s_enable_calls = 0u;
    s_clear_calls = 0u;
    s_green_clear_calls = 0u;
    memset(s_logs, 0, sizeof(s_logs));
    s_log_count = 0u;
    s_log_truncated = 0u;
    s_max_log_length = 0u;
}

static int log_contains(const char *needle)
{
    unsigned index;

    for (index = 0u; index < s_log_count; ++index)
        if (strstr(s_logs[index], needle))
            return 1;
    return 0;
}

static IsaacVitaGlKnownColorProbe valid_event(void)
{
    IsaacVitaGlKnownColorProbe event;

    memset(&event, 0, sizeof(event));
    event.size = sizeof(event);
    event.present_index = ISAAC_VITAGL_KNOWN_COLOR_PRESENT_INDEX;
    event.sequence = 0x12345678u;
    event.front_index = 1u;
    event.back_index = 2u;
    event.address = 0x84000000u;
    event.dedicated = 1u;
    event.color = ISAAC_VITAGL_KNOWN_COLOR_FILL;
    event.format = ISAAC_VITAGL_KNOWN_COLOR_FORMAT;
    event.x = ISAAC_VITAGL_KNOWN_COLOR_X;
    event.y = ISAAC_VITAGL_KNOWN_COLOR_Y;
    event.width = ISAAC_VITAGL_KNOWN_COLOR_WIDTH;
    event.height = ISAAC_VITAGL_KNOWN_COLOR_HEIGHT;
    event.stride_bytes = 960u * 4u;
    event.gate_mask = ISAAC_VITAGL_KNOWN_COLOR_GATE_ALL;
    event.context_finish_called = 1u;
    event.fill_result = 0;
    event.transfer_finish_called = 1u;
    event.transfer_finish_result = 0;
    event.queue_result = 0;
    return event;
}

static int test_success_and_copy(void)
{
    IsaacVitaGlKnownColorProbe event;
    IsaacVitaFirstFrameSnapshot snapshot;
    GLint viewport[4];
    GLboolean mask[4];
    GLfloat clear[4];
    unsigned logs_before_hook;

    reset_mock_state();
    memcpy(viewport, s_viewport, sizeof(viewport));
    memcpy(mask, s_color_mask, sizeof(mask));
    memcpy(clear, s_clear_color, sizeof(clear));
    CHECK(gl_vita_backend_install());
    CHECK(isaac_vita_vitagl_known_color_present_index() == UINT32_MAX);
    gl_vita_backend_first_frame_before_present(3u);
    gl_vita_backend_first_frame_before_present(4u);
    CHECK(s_clear_calls == 0u);
    gl_vita_backend_first_frame_before_present(2u);
    CHECK(s_green_clear_calls == 1u && s_clear_calls == 1u);
    CHECK(s_bind_calls == 0u && s_disable_calls == 0u &&
          s_enable_calls == 0u);
    CHECK(s_read_fbo == 17u && s_draw_fbo == 0u);
    CHECK(memcmp(viewport, s_viewport, sizeof(viewport)) == 0);
    CHECK(memcmp(mask, s_color_mask, sizeof(mask)) == 0);
    CHECK(memcmp(clear, s_clear_color, sizeof(clear)) == 0);
    gl_vita_backend_first_frame_snapshot(&snapshot);
    CHECK(snapshot.control_clears == 1u);

    gl_vita_backend_first_frame_queue_begin(2u);
    CHECK(isaac_vita_vitagl_known_color_present_index() == 2u);
    event = valid_event();
    logs_before_hook = s_log_count;
    isaac_vita_vitagl_known_color_probe(&event);
    CHECK(s_log_count == logs_before_hook);
    event.queue_result = (int32_t)0xdeadbeefu;
    isaac_vita_vitagl_known_color_probe(&event);
    CHECK(s_log_count == logs_before_hook);
    memset(&event, 0xa5, sizeof(event));
    gl_vita_backend_first_frame_queue_end();
    CHECK(isaac_vita_vitagl_known_color_present_index() == UINT32_MAX);
    CHECK(s_log_count == logs_before_hook + 2u);
    CHECK(log_contains("phase=known-color-clear p=00000002"));
    CHECK(log_contains("attempted=1 saved=00000011/00000000"));
    CHECK(log_contains("phase=known-color-transfer captured=1"));
    CHECK(log_contains("seq=12345678"));
    CHECK(log_contains("gate=00003fff"));
    CHECK(log_contains("queue=00000000"));
    CHECK(!log_contains("queue=deadbeef"));
    CHECK(s_log_truncated == 0u && s_max_log_length < 384u);
    gl_vita_backend_uninstall();
    return 0;
}

static int test_fail_closed_clear_gates(void)
{
    IsaacVitaGlKnownColorProbe event;
    IsaacVitaFirstFrameSnapshot snapshot;

    reset_mock_state();
    s_scissor = 1u;
    CHECK(gl_vita_backend_install());
    gl_vita_backend_first_frame_before_present(2u);
    gl_vita_backend_first_frame_snapshot(&snapshot);
    CHECK(snapshot.control_clears == 0u && s_clear_calls == 0u);
    CHECK(s_bind_calls == 0u && s_disable_calls == 0u &&
          s_enable_calls == 0u && s_scissor == 1u);
    gl_vita_backend_first_frame_queue_begin(2u);
    event = valid_event();
    isaac_vita_vitagl_known_color_probe(&event);
    gl_vita_backend_first_frame_queue_end();
    CHECK(log_contains("attempted=0 saved=00000011/00000000"));
    CHECK(log_contains("scissor=1"));
    CHECK(log_contains("phase=known-color-transfer captured=1"));
    gl_vita_backend_uninstall();

    reset_mock_state();
    s_draw_fbo = 77u;
    CHECK(gl_vita_backend_install());
    gl_vita_backend_first_frame_before_present(2u);
    CHECK(s_clear_calls == 0u && s_bind_calls == 0u &&
          s_draw_fbo == 77u);
    gl_vita_backend_first_frame_queue_begin(2u);
    event = valid_event();
    event.size--;
    isaac_vita_vitagl_known_color_probe(&event);
    gl_vita_backend_first_frame_queue_end();
    CHECK(log_contains("attempted=0 saved=00000011/0000004d"));
    CHECK(log_contains("phase=known-color-transfer captured=0"));
    CHECK(s_log_truncated == 0u && s_max_log_length < 384u);
    gl_vita_backend_uninstall();
    return 0;
}

int main(void)
{
    if (test_success_and_copy() || test_fail_closed_clear_gates())
        return 1;
    puts("known-color runtime oracle: PASS");
    return 0;
}
