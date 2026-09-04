#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl_bridge.h"
#include "gl_vita_backend.h"
#include "gl_vita_first_frame_oracle_vitagl.h"

#if UINTPTR_MAX != UINT32_MAX
# error first-frame behavior oracle requires a 32-bit host process
#endif

#define FF_FRAMEBUFFER_COMPLETE 0x8cd5u
#define FF_FRAMEBUFFER_INCOMPLETE 0x8cd7u
#define FF_FRAMEBUFFER_BINDING 0x8ca6u
#define FF_READ_FRAMEBUFFER 0x8ca8u
#define FF_DRAW_FRAMEBUFFER 0x8ca9u
#define FF_READ_FRAMEBUFFER_BINDING 0x8caau
#define FF_COLOR_ATTACHMENT0 0x8ce0u
#define FF_RGBA 0x1908u
#define FF_UNSIGNED_BYTE 0x1401u
#define FF_VIEWPORT 0x0ba2u
#define FF_SCISSOR_TEST 0x0c11u
#define FF_COLOR_CLEAR_VALUE 0x0c22u
#define FF_COLOR_WRITEMASK 0x0c23u
#define FF_COLOR_BUFFER_BIT 0x4000u
#define FF_BIND_TOKEN 0x7eed2b3cu
#define FF_CLEAR_TOKEN 0x7e194b2bu
#define FF_DRAW_TOKEN 0x7e302306u
#define FF_FRAMEBUFFER_TEXTURE_2D_TOKEN 0x7e122133u

_Static_assert(sizeof(ISAAC_VITA_FIRST_FRAME_BUILD_ID) - 1u == 96u,
               "oracle must exercise the maximum accepted build id");

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "first-frame oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static GLuint s_read_fbo;
static GLuint s_draw_fbo;
static GLint s_viewport[4] = { 11, 12, 913, 517 };
static GLboolean s_color_mask[4] = { 1u, 0u, 1u, 0u };
static GLfloat s_clear_color[4] = { 0.1f, 0.2f, 0.3f, 0.4f };
static GLboolean s_scissor = 1u;
static GLenum s_error;
static GLenum s_named_status = FF_FRAMEBUFFER_COMPLETE;
static unsigned s_magenta_default_clears;
static unsigned s_blit_calls;
static unsigned s_blit_scissor_enabled_calls;
static GLenum s_next_blit_error;
static GLenum s_next_read_error;
static GLenum s_next_restore_error;
static unsigned s_corrupt_next_read_canary;
static unsigned s_finish_calls;
static unsigned s_read_calls;
static GLuint s_read_fbos[16];
static GLint s_read_xy[16][2];
static unsigned s_framebuffer_texture_calls;
static GLuint s_framebuffer_texture_arg;
static GLuint s_blit_read;
static GLuint s_blit_draw;
static GLint s_blit_box[8];
static GLbitfield s_blit_mask;
static GLenum s_blit_filter;
static char s_logs[64][384];
static unsigned s_log_count;
static unsigned s_log_truncated;
static unsigned s_max_log_length;

void isaac_gl_vita_first_frame_oracle_log_worst_summary(void);

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
    unsigned slot = s_log_count < 64u ? s_log_count : 63u;

    va_start(arguments, format);
    length = vsnprintf(s_logs[slot], sizeof s_logs[slot], format, arguments);
    va_end(arguments);
    s_logs[slot][sizeof s_logs[slot] - 1u] = '\0';
    if (length < 0 || (size_t)length >= sizeof s_logs[slot])
        ++s_log_truncated;
    if (length > 0 && (unsigned)length > s_max_log_length)
        s_max_log_length = (unsigned)length;
    if (s_log_count < 64u)
        ++s_log_count;
}

void oracle_ff_glNoop(void) {}
GLuint oracle_ff_glReturnUint(void) { return 1u; }
GLint oracle_ff_glReturnInt(void) { return 0; }
const GLubyte *oracle_ff_glReturnString(void)
{
    static const GLubyte value[] = "oracle";
    return value;
}

void oracle_ff_glBindFramebuffer(GLenum target, GLuint framebuffer)
{
    if (target == 0x8d40u) {
        s_read_fbo = framebuffer;
        s_draw_fbo = framebuffer;
    } else if (target == FF_READ_FRAMEBUFFER) {
        s_read_fbo = framebuffer;
    } else if (target == FF_DRAW_FRAMEBUFFER) {
        s_draw_fbo = framebuffer;
        if (s_next_restore_error) {
            s_error = s_next_restore_error;
            s_next_restore_error = 0u;
        }
    }
}

void oracle_ff_glClear(GLbitfield mask)
{
    if (s_draw_fbo == 0u && mask == FF_COLOR_BUFFER_BIT &&
            s_viewport[0] == 0 && s_viewport[1] == 0 &&
            s_viewport[2] == 960 && s_viewport[3] == 544 &&
            !s_scissor && s_color_mask[0] && s_color_mask[1] &&
            s_color_mask[2] && s_color_mask[3] &&
            s_clear_color[0] == 1.0f && s_clear_color[1] == 0.0f &&
            s_clear_color[2] == 1.0f && s_clear_color[3] == 1.0f)
        ++s_magenta_default_clears;
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
    if (capability == FF_SCISSOR_TEST)
        s_scissor = 0u;
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
    if (capability == FF_SCISSOR_TEST)
        s_scissor = 1u;
}

void oracle_ff_glFinish(void)
{
    ++s_finish_calls;
}

void oracle_ff_glFramebufferTexture2D(
    GLenum target, GLenum attachment, GLenum texture_target,
    GLuint texture, GLint level)
{
    (void)target;
    (void)attachment;
    (void)texture_target;
    (void)level;
    ++s_framebuffer_texture_calls;
    s_framebuffer_texture_arg = texture;
}

void oracle_ff_glGetBooleanv(GLenum name, GLboolean *values)
{
    if (name == FF_COLOR_WRITEMASK)
        memcpy(values, s_color_mask, sizeof s_color_mask);
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
        memcpy(values, s_clear_color, sizeof s_clear_color);
}

void oracle_ff_glGetIntegerv(GLenum name, GLint *values)
{
    if (name == FF_FRAMEBUFFER_BINDING)
        *values = (GLint)s_draw_fbo;
    else if (name == FF_READ_FRAMEBUFFER_BINDING)
        *values = (GLint)s_read_fbo;
    else if (name == FF_VIEWPORT)
        memcpy(values, s_viewport, sizeof s_viewport);
}

GLboolean oracle_ff_glIsEnabled(GLenum capability)
{
    return capability == FF_SCISSOR_TEST ? s_scissor : 0u;
}

void oracle_ff_glReadPixels(
    GLint x, GLint y, GLsizei width, GLsizei height,
    GLenum format, GLenum type, void *pixels)
{
    uint32_t *words = (uint32_t *)pixels;
    unsigned slot = s_read_calls < 16u ? s_read_calls : 15u;

    if (width != 1 || height != 1 || format != FF_RGBA ||
            type != FF_UNSIGNED_BYTE || !pixels) {
        s_error = 0x0501u;
        return;
    }
    s_read_fbos[slot] = s_read_fbo;
    s_read_xy[slot][0] = x;
    s_read_xy[slot][1] = y;
    if (s_read_calls < 16u)
        ++s_read_calls;
    words[0] = s_read_fbo ? 0xff332211u : 0xffff00ffu;
    if (s_corrupt_next_read_canary) {
        words[1] = 0u;
        s_corrupt_next_read_canary = 0u;
    }
    if (s_next_read_error) {
        s_error = s_next_read_error;
        s_next_read_error = 0u;
    }
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
    return s_named_status;
}

void oracle_ff_glBlitNamedFramebuffer(
    GLuint read_framebuffer, GLuint draw_framebuffer,
    GLint src_x0, GLint src_y0, GLint src_x1, GLint src_y1,
    GLint dst_x0, GLint dst_y0, GLint dst_x1, GLint dst_y1,
    GLbitfield mask, GLenum filter)
{
    ++s_blit_calls;
    if (s_scissor)
        ++s_blit_scissor_enabled_calls;
    s_blit_read = read_framebuffer;
    s_blit_draw = draw_framebuffer;
    s_blit_box[0] = src_x0;
    s_blit_box[1] = src_y0;
    s_blit_box[2] = src_x1;
    s_blit_box[3] = src_y1;
    s_blit_box[4] = dst_x0;
    s_blit_box[5] = dst_y0;
    s_blit_box[6] = dst_x1;
    s_blit_box[7] = dst_y1;
    s_blit_mask = mask;
    s_blit_filter = filter;
    s_error = s_next_blit_error;
}

static int oracle_dispatch(
    CPU *cpu, uint32_t stack_top, uint32_t token,
    const uint32_t *arguments, uint32_t count)
{
    uint32_t index;

    cpu->esp = stack_top;
    for (index = count; index != 0u; --index)
        gpush(cpu, arguments[index - 1u]);
    gpush(cpu, 0xaabbccddu);
    return guest_gl_dispatch(cpu, token) && cpu->esp == stack_top;
}

static int log_contains(const char *needle)
{
    unsigned index;
    for (index = 0u; index < s_log_count; ++index) {
        if (strstr(s_logs[index], needle))
            return 1;
    }
    return 0;
}

static unsigned log_count_contains(const char *needle)
{
    unsigned count = 0u;
    unsigned index;
    for (index = 0u; index < s_log_count; ++index) {
        if (strstr(s_logs[index], needle))
            ++count;
    }
    return count;
}

static void oracle_queue_add(
    uint32_t present, uint32_t sequence, int32_t result)
{
    IsaacVitaGlDisplayQueueProbe event;

    memset(&event, 0, sizeof event);
    event.size = (uint32_t)sizeof event;
    event.stage = ISAAC_VITAGL_DISPLAY_QUEUE_STAGE_ADD_RESULT;
    event.sequence = sequence;
    event.front_index = 2u;
    event.back_index = present == UINT32_MAX ? 1u : present % 3u;
    event.address = 0x12345000u;
    event.detail.add.result = result;
    event.detail.add.old_sync = 0x81002000u;
    event.detail.add.new_sync = 0x81003000u;
    event.detail.add.begin_context = 0x82001000u;
    event.detail.add.begin_render_target = 0x82002000u;
    event.detail.add.begin_fragment_sync = 0x81003000u;
    event.detail.add.begin_color_surface = 0x83001000u;
    event.detail.add.begin_color_data = 0x12345000u;
    event.detail.add.begin_result = 0;
    event.detail.add.begin_count = 1u;
    event.detail.add.end_context = 0x82001000u;
    event.detail.add.end_result = 0;
    event.detail.add.end_count = 1u;
    event.detail.add.back_memblock_uid =
        (int32_t)(0x7000u + event.back_index);
    event.detail.add.back_get_base_result = 0;
    event.detail.add.back_map_result = 0;
    event.detail.add.back_dedicated = 1u;
    if (present != UINT32_MAX)
        gl_vita_backend_first_frame_queue_begin(present);
    isaac_vitagl_display_queue_probe(&event);
    if (present != UINT32_MAX)
        gl_vita_backend_first_frame_queue_end();
}

static void oracle_queue_callback(uint32_t sequence)
{
    IsaacVitaGlDisplayQueueProbe event;
    unsigned index;

    memset(&event, 0, sizeof event);
    event.size = (uint32_t)sizeof event;
    event.stage = ISAAC_VITAGL_DISPLAY_QUEUE_STAGE_DISPLAY_CALLBACK;
    event.sequence = sequence;
    event.front_index = 2u;
    event.back_index = 1u;
    event.address = 0x12345000u;
    event.detail.display.result = 0;
    event.detail.display.size = 24u;
    event.detail.display.base = 0x12345000u;
    event.detail.display.pitch = 960u;
    event.detail.display.pixel_format = 0u;
    event.detail.display.width = 960u;
    event.detail.display.height = 544u;
    event.detail.display.sync = 1u;
    event.detail.display.sample_count =
        ISAAC_VITAGL_DISPLAY_QUEUE_SAMPLE_COUNT;
    event.detail.display.sparse_hash = 0x11223344u;
    for (index = 0u; index < ISAAC_VITAGL_DISPLAY_QUEUE_SAMPLE_COUNT;
            ++index)
        event.detail.display.rgba[index] =
            0x11111111u * (index + 1u);
    event.detail.display.rgba[4] = 0xffff00ffu;
    /* Callback deliberately arrives after queue_end: correlation must use
     * its copied sequence, not the now-cleared active present tag. */
    isaac_vitagl_display_queue_probe(&event);
    memset(&event, 0xa5, sizeof event);
}

static void oracle_queue_success(uint32_t present, uint32_t sequence)
{
    oracle_queue_add(present, sequence, 0);
    oracle_queue_callback(sequence);
}

#if defined(ISAAC_VITA_DIRECT_DEFAULT)
int main(void)
{
    IsaacVitaFirstFrameSnapshot snapshot;
    GLint saved_viewport[4];
    GLboolean saved_mask[4];
    GLfloat saved_clear[4];

    CHECK(gl_vita_backend_install());
    gl_vita_backend_first_frame_set_manager_framebuffer(77u);
    oracle_ff_glBindFramebuffer(0x8d40u, 77u);
    oracle_ff_glViewport(11, 12, 913, 517);
    oracle_ff_glEnable(FF_SCISSOR_TEST);
    oracle_ff_glColorMask(1u, 0u, 1u, 0u);
    oracle_ff_glClearColor(0.1f, 0.2f, 0.3f, 0.4f);
    memcpy(saved_viewport, s_viewport, sizeof saved_viewport);
    memcpy(saved_mask, s_color_mask, sizeof saved_mask);
    memcpy(saved_clear, s_clear_color, sizeof saved_clear);

    gl_vita_backend_first_frame_before_present(0u);
    CHECK(s_magenta_default_clears == 0u && s_blit_calls == 0u);
    oracle_queue_success(0u, 17u);
    gl_vita_backend_first_frame_before_present(1u);
    CHECK(s_magenta_default_clears == 0u && s_blit_calls == 0u);
    oracle_queue_success(1u, 18u);
    gl_vita_backend_first_frame_before_present(2u);
    oracle_queue_success(2u, 19u);
    gl_vita_backend_first_frame_before_present(3u);
    oracle_queue_success(3u, 20u);
    gl_vita_backend_first_frame_before_present(4u);
    oracle_queue_success(4u, 21u);
    gl_vita_backend_first_frame_before_present(5u);
    gl_vita_backend_first_frame_before_present(8u);

    gl_vita_backend_first_frame_before_present(119u);
    oracle_queue_success(119u, 22u);
    gl_vita_backend_first_frame_before_present(120u);
    gl_vita_backend_first_frame_before_present(239u);
    oracle_queue_success(239u, 23u);
    gl_vita_backend_first_frame_before_present(240u);
    oracle_queue_success(240u, 24u);
    gl_vita_backend_first_frame_before_present(241u);
    gl_vita_backend_first_frame_before_present(359u);
    oracle_queue_success(359u, 25u);
    gl_vita_backend_first_frame_before_present(360u);
    oracle_queue_success(360u, 26u);
    gl_vita_backend_first_frame_before_present(361u);

    CHECK(s_magenta_default_clears == 0u && s_blit_calls == 0u);
    CHECK(memcmp(saved_viewport, s_viewport, sizeof saved_viewport) == 0);
    CHECK(memcmp(saved_mask, s_color_mask, sizeof saved_mask) == 0);
    CHECK(memcmp(saved_clear, s_clear_color, sizeof saved_clear) == 0);
    CHECK(s_read_fbo == 77u && s_draw_fbo == 77u && s_scissor == 1u);
    CHECK(s_finish_calls == 7u && s_read_calls == 7u);

    gl_vita_backend_first_frame_snapshot(&snapshot);
    CHECK(snapshot.manager_fbo == 77u);
    CHECK(snapshot.control_clears == 0u);
    CHECK(snapshot.blit_attempts == 0u && snapshot.blit_successes == 0u);
    CHECK(snapshot.blit_no_manager == 0u && snapshot.blit_incomplete == 0u);
    CHECK(log_contains("mode=true-direct-default probes=passive"));
    CHECK(log_count_contains("phase=true-direct-default-passive") == 2u);
    CHECK(!log_contains("phase=control-clear"));
    CHECK(!log_contains("phase=explicit-blit"));
    CHECK(!log_contains("phase=postprocess-bypass"));
    CHECK(log_contains("present=0 surface=default") &&
          log_contains("present=1 surface=default"));
    CHECK(log_contains("phase=queue-add p=00000002 seq=00000013") &&
          log_contains("phase=queue-scene p=00000002 seq=00000013") &&
          log_contains("begin=00000001/82001000/82002000/81003000/") &&
          log_contains("end=00000001/82001000/00000000 mismatch=00000000 "
                       "chain=1"));
    CHECK(log_contains("phase=queue-display p=00000004 seq=00000015") &&
          log_contains("fb=00000018/12345000/000003c0/00000000/"
                       "000003c0/00000220/00000001/00000000 chain=1"));
    CHECK(log_contains("phase=queue-probe-complete present=8 "
                       "seen=0x3f expected=0x3f dedicated=3/3 "
                       "dedmask=0x07 dedgate=1"));
    CHECK(log_count_contains("phase=readback") == 7u);
    CHECK(log_count_contains("phase=queue-add") == 3u);
    CHECK(log_count_contains("phase=queue-scene") == 3u);
    CHECK(log_count_contains("phase=queue-display") == 3u);
    CHECK(s_log_truncated == 0u);
    isaac_gl_vita_first_frame_oracle_log_worst_summary();
    CHECK(s_log_truncated == 0u && s_max_log_length <= 383u);
    CHECK(strstr(s_logs[s_log_count - 1u], "end=ff") != NULL);

    gl_vita_backend_uninstall();
    printf("first-frame true-direct passive oracle: PASS "
           "(max durable body=%u/383)\n", s_max_log_length);
    return 0;
}
#else
int main(void)
{
    uint32_t stack[64];
    uint32_t arguments[5];
    uint32_t stack_top = (uint32_t)(uintptr_t)&stack[64];
    CPU cpu;
    IsaacVitaFirstFrameSnapshot snapshot;
    GLint saved_viewport[4];
    GLboolean saved_mask[4];
    GLfloat saved_clear[4];
    unsigned clears_before;
    unsigned blits_before;
    unsigned reads_before;
    volatile uint32_t phase_boundary;

    memset(&cpu, 0, sizeof cpu);
    phase_boundary = 239u;
    CHECK(!isaac_vita_first_frame_postprocess_bypass_active(phase_boundary));
    phase_boundary = 240u;
    CHECK(isaac_vita_first_frame_postprocess_bypass_active(phase_boundary));
    phase_boundary = 359u;
    CHECK(isaac_vita_first_frame_postprocess_bypass_active(phase_boundary));
    CHECK(isaac_vita_first_frame_postprocess_bypass_complete(
        phase_boundary + 1u));
    phase_boundary = 360u;
    CHECK(!isaac_vita_first_frame_postprocess_bypass_active(phase_boundary));
    CHECK(isaac_vita_first_frame_postprocess_restored(phase_boundary + 1u));
    CHECK(gl_vita_backend_install());
    gl_vita_backend_first_frame_set_manager_framebuffer(77u);

    arguments[0] = 0x8d40u;
    arguments[1] = 77u;
    CHECK(oracle_dispatch(&cpu, stack_top, FF_BIND_TOKEN, arguments, 2u));
    arguments[0] = 0x8d40u;
    arguments[1] = FF_COLOR_ATTACHMENT0;
    arguments[2] = 0x0de1u;
    arguments[3] = 1234u;
    arguments[4] = 0u;
    CHECK(oracle_dispatch(&cpu, stack_top,
                          FF_FRAMEBUFFER_TEXTURE_2D_TOKEN, arguments, 5u));
    CHECK(s_framebuffer_texture_calls == 1u &&
          s_framebuffer_texture_arg == 1234u);
    arguments[0] = FF_COLOR_BUFFER_BIT;
    CHECK(oracle_dispatch(&cpu, stack_top, FF_CLEAR_TOKEN, arguments, 1u));
    arguments[0] = 4u;
    arguments[1] = 6u;
    arguments[2] = 0x1403u;
    arguments[3] = 0u;
    CHECK(oracle_dispatch(&cpu, stack_top, FF_DRAW_TOKEN, arguments, 4u));

    arguments[0] = 0x8d40u;
    arguments[1] = 0u;
    CHECK(oracle_dispatch(&cpu, stack_top, FF_BIND_TOKEN, arguments, 2u));
    arguments[0] = FF_COLOR_BUFFER_BIT;
    CHECK(oracle_dispatch(&cpu, stack_top, FF_CLEAR_TOKEN, arguments, 1u));
    arguments[0] = 4u;
    arguments[1] = 6u;
    arguments[2] = 0x1403u;
    arguments[3] = 0u;
    CHECK(oracle_dispatch(&cpu, stack_top, FF_DRAW_TOKEN, arguments, 4u));

    oracle_ff_glBindFramebuffer(0x8d40u, 77u);
    oracle_ff_glViewport(11, 12, 913, 517);
    oracle_ff_glEnable(FF_SCISSOR_TEST);
    oracle_ff_glColorMask(1u, 0u, 1u, 0u);
    oracle_ff_glClearColor(0.1f, 0.2f, 0.3f, 0.4f);
    memcpy(saved_viewport, s_viewport, sizeof saved_viewport);
    memcpy(saved_mask, s_color_mask, sizeof saved_mask);
    memcpy(saved_clear, s_clear_color, sizeof saved_clear);

    gl_vita_backend_first_frame_before_present(0u);
    CHECK(s_magenta_default_clears == 1u && s_blit_calls == 0u);
    CHECK(s_read_fbo == 77u && s_draw_fbo == 77u && s_scissor == 1u);
    CHECK(memcmp(saved_viewport, s_viewport, sizeof saved_viewport) == 0);
    CHECK(memcmp(saved_mask, s_color_mask, sizeof saved_mask) == 0);
    CHECK(memcmp(saved_clear, s_clear_color, sizeof saved_clear) == 0);
    /* An ADD outside the explicit guest-present tag is a loading/foreign
     * sentinel and must not manufacture a guest-present mapping. */
    oracle_queue_add(UINT32_MAX, 16u, 0);
    oracle_queue_callback(16u);
    oracle_queue_success(2u, 17u);
    gl_vita_backend_first_frame_before_present(3u);
    oracle_queue_add(3u, 18u, (int32_t)0x805b0001u);
    oracle_queue_callback(18u);
    gl_vita_backend_first_frame_before_present(4u);
    oracle_queue_success(4u, 19u);
    gl_vita_backend_first_frame_before_present(5u);
    gl_vita_backend_first_frame_before_present(8u);

    gl_vita_backend_first_frame_before_present(119u);
    CHECK(s_magenta_default_clears == 6u && s_blit_calls == 0u);
    CHECK(s_finish_calls == 1u && s_read_calls == 2u);
    CHECK(s_read_fbos[0] == 77u && s_read_fbos[1] == 0u);
    CHECK(s_read_xy[0][0] == 480 && s_read_xy[0][1] == 270);
    CHECK(s_read_xy[1][0] == 480 && s_read_xy[1][1] == 272);
    CHECK(s_read_fbo == 77u && s_draw_fbo == 77u);
    oracle_queue_success(119u, 20u);
    gl_vita_backend_first_frame_before_present(120u);
    CHECK(s_magenta_default_clears == 6u && s_blit_calls == 1u);
    CHECK(s_blit_scissor_enabled_calls == 0u && s_scissor == 1u);
    CHECK(s_blit_read == 77u && s_blit_draw == 0u);
    CHECK(s_blit_box[0] == 0 && s_blit_box[1] == 0 &&
          s_blit_box[2] == 960 && s_blit_box[3] == 540 &&
          s_blit_box[4] == 0 && s_blit_box[5] == 2 &&
          s_blit_box[6] == 960 && s_blit_box[7] == 542);
    CHECK(s_blit_mask == FF_COLOR_BUFFER_BIT && s_blit_filter == 0x2600u);

    gl_vita_backend_first_frame_set_manager_framebuffer(0u);
    blits_before = s_blit_calls;
    gl_vita_backend_first_frame_before_present(121u);
    CHECK(s_blit_calls == blits_before);
    gl_vita_backend_first_frame_set_manager_framebuffer(88u);
    s_named_status = FF_FRAMEBUFFER_INCOMPLETE;
    gl_vita_backend_first_frame_before_present(122u);
    CHECK(s_blit_calls == blits_before);

    gl_vita_backend_first_frame_set_manager_framebuffer(77u);
    s_named_status = FF_FRAMEBUFFER_COMPLETE;
    s_next_blit_error = 0x0502u;
    gl_vita_backend_first_frame_before_present(123u);
    CHECK(s_blit_calls == blits_before + 1u);
    CHECK(s_blit_scissor_enabled_calls == 0u && s_scissor == 1u);
    blits_before = s_blit_calls;

    /* A missing manager is not the default framebuffer: boundary 239 must
     * skip that sample and still collect the explicitly named default. */
    gl_vita_backend_first_frame_set_manager_framebuffer(0u);
    reads_before = s_read_calls;
    gl_vita_backend_first_frame_before_present(239u);
    CHECK(s_read_calls == reads_before + 1u);
    CHECK(s_read_fbos[reads_before] == 0u);
    oracle_queue_add(239u, 32u, (int32_t)0x805b0001u);
    oracle_queue_success(239u, 33u);

    gl_vita_backend_first_frame_set_manager_framebuffer(77u);
    arguments[0] = 0x8d40u;
    arguments[1] = 77u;
    CHECK(oracle_dispatch(&cpu, stack_top, FF_BIND_TOKEN, arguments, 2u));
    arguments[0] = 0x8d40u;
    arguments[1] = FF_COLOR_ATTACHMENT0;
    arguments[2] = 0x0de1u;
    arguments[3] = 5678u;
    arguments[4] = 0u;
    CHECK(oracle_dispatch(&cpu, stack_top,
                          FF_FRAMEBUFFER_TEXTURE_2D_TOKEN, arguments, 5u));
    arguments[0] = 0x8d40u;
    arguments[1] = 0u;
    CHECK(oracle_dispatch(&cpu, stack_top, FF_BIND_TOKEN, arguments, 2u));
    oracle_ff_glBindFramebuffer(0x8d40u, 77u);
    oracle_ff_glBindFramebuffer(FF_READ_FRAMEBUFFER, 66u);

    clears_before = s_magenta_default_clears;
    s_next_read_error = 0x0502u;
    s_next_restore_error = 0x0506u;
    s_corrupt_next_read_canary = 1u;
    gl_vita_backend_first_frame_before_present(240u);
    CHECK(s_magenta_default_clears == clears_before &&
          s_blit_calls == blits_before);
    CHECK(s_read_fbo == 66u && s_draw_fbo == 77u);
    oracle_queue_success(240u, 34u);
    gl_vita_backend_first_frame_before_present(241u);

    gl_vita_backend_first_frame_before_present(359u);
    oracle_queue_success(359u, 35u);
    gl_vita_backend_first_frame_before_present(360u);
    oracle_queue_success(360u, 36u);
    gl_vita_backend_first_frame_before_present(361u);

    gl_vita_backend_first_frame_snapshot(&snapshot);
    CHECK(snapshot.current_guest_fbo == 0u && snapshot.manager_fbo == 77u);
    CHECK(snapshot.manager_color_attach_arg == 5678u &&
          snapshot.manager_color_attach_calls == 1u);
    CHECK(snapshot.bind_zero == 2u && snapshot.bind_nonzero == 2u);
    CHECK(snapshot.clear_default == 1u && snapshot.clear_offscreen == 1u);
    CHECK(snapshot.draw_default == 1u && snapshot.draw_offscreen == 1u);
    CHECK(snapshot.last_draw_fbo == 0u && snapshot.control_clears == 6u);
    CHECK(snapshot.blit_attempts == 2u && snapshot.blit_successes == 1u);
    CHECK(snapshot.blit_no_manager == 2u && snapshot.blit_incomplete == 1u);
    CHECK(s_finish_calls == 5u && s_read_calls == 9u);
    CHECK(s_read_fbo == 66u && s_draw_fbo == 77u);
    CHECK(log_contains(
        "bid40=oracle:first-frame:012345678901234567890 "
        "phase=control-clear"));
    CHECK(log_contains("phase=control-clear-complete present=120"));
    CHECK(log_contains("phase=explicit-blit present=120 result=blit-ok"));
    CHECK(log_contains("result=skip-zero-manager"));
    CHECK(log_contains("result=skip-incomplete"));
    CHECK(log_contains("result=blit-error"));
    CHECK(log_contains("present=239 surface=manager fbo=0"));
    CHECK(log_contains("present=239 surface=manager fbo=0 attach_arg=0 ") &&
          log_contains("available=0 status=0x00000000 attempted=0"));
    CHECK(log_contains("present=240 surface=manager"));
    CHECK(log_contains("read=0x00000502 restore=0x00000506 canary=0"));
    CHECK(log_contains("present=119 surface=manager") &&
          log_contains("read=0x00000000 restore=0x00000000 canary=1"));
    CHECK(log_contains("phase=queue-add p=00000003 seq=00000012 "
                       "rc=805b0001"));
    CHECK(log_contains("mem=00007000/00000000/00000000 ded=1"));
    CHECK(log_contains("phase=queue-display p=00000002 seq=00000011") &&
          log_contains("phase=queue-display p=00000004 seq=00000013"));
    CHECK(log_contains("begin=00000001/82001000/82002000/81003000/") &&
          log_contains("end=00000001/82001000/00000000 mismatch=00000000 "
                       "chain=1"));
    CHECK(log_contains("fb=00000018/12345000/000003c0/00000000/"
                       "000003c0/00000220/00000001/00000000 chain=1"));
    CHECK(log_contains("hash=11223344 rgba=11111111,22222222"));
    CHECK(log_contains("44444444,ffff00ff,66666666"));
    CHECK(log_contains("present=119 surface=default") &&
          log_contains("nonblack=1 rgba=ffff00ff end=ff"));
    CHECK(log_contains("phase=queue-probe-complete present=8 "
                       "seen=0x3f expected=0x3f dedicated=3/3 "
                       "dedmask=0x07 dedgate=1"));
    CHECK(log_count_contains("phase=readback") == 10u);
    CHECK(log_count_contains("rgba=") == 13u);
    CHECK(log_count_contains("phase=queue-add") == 3u);
    CHECK(log_count_contains("phase=queue-scene") == 3u);
    CHECK(log_count_contains("phase=queue-display") == 3u);
    CHECK(s_log_truncated == 0u);
    CHECK(log_count_contains("phase=config") == 1u);
    isaac_gl_vita_first_frame_oracle_log_worst_summary();
    CHECK(s_log_truncated == 0u);
    CHECK(s_max_log_length <= 383u);
    CHECK(strstr(s_logs[s_log_count - 1u], "end=ff") != NULL);

    /* Reset retires, rather than clearing underneath, a callback generation.
     * A fully published old event must not leak into the next session. */
    gl_vita_backend_uninstall();
    CHECK(gl_vita_backend_install());
    gl_vita_backend_first_frame_set_manager_framebuffer(77u);
    gl_vita_backend_first_frame_before_present(0u);
    oracle_queue_success(2u, 49u);
    gl_vita_backend_uninstall();
    CHECK(gl_vita_backend_install());
    gl_vita_backend_first_frame_set_manager_framebuffer(77u);
    gl_vita_backend_first_frame_before_present(0u);
    reads_before = log_count_contains("phase=queue-add");
    gl_vita_backend_first_frame_before_present(3u);
    CHECK(log_count_contains("phase=queue-add") == reads_before);
    gl_vita_backend_uninstall();

    printf("first-frame phase/state/restore/guard oracle: PASS "
           "(max durable body=%u/383)\n", s_max_log_length);
    return 0;
}
#endif
