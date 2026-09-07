/* Executable x86 lifecycle/redzone oracle and static-only softfp link oracle. */
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl_bridge.h"
#include "gl_vita_backend.h"
#include "gl_vita_backend_test_vitagl.h"

#if UINTPTR_MAX != UINT32_MAX
# error gl_vita_backend_oracle requires a 32-bit address space
#endif

#define ORACLE_GL_RENDERBUFFER        0x00008d41u
#define ORACLE_GL_RENDERBUFFER_WIDTH  0x00008d42u
#define ORACLE_GL_RENDERBUFFER_HEIGHT 0x00008d43u
#define ORACLE_GL_DEPTH24             0x000081a6u
#define ORACLE_GL_FRAMEBUFFER         0x00008d40u
#define ORACLE_GL_READ_FRAMEBUFFER    0x00008ca8u
#define ORACLE_GL_DRAW_FRAMEBUFFER    0x00008ca9u
#define ORACLE_GL_VIEWPORT            0x00000ba2u
#define ORACLE_GL_TEXTURE_2D          0x00000de1u
#define ORACLE_GL_TEXTURE_CUBE_MAP    0x00008513u
#define ORACLE_GL_TEXTURE0            0x000084c0u
#define ORACLE_GL_TEXTURE_BINDING_2D  0x00008069u
#define ORACLE_GL_TEXTURE_MAG_FILTER  0x00002800u
#define ORACLE_GL_TEXTURE_MIN_FILTER  0x00002801u
#define ORACLE_GL_TEXTURE_WRAP_S      0x00002802u
#define ORACLE_GL_TEXTURE_WRAP_T      0x00002803u
#define ORACLE_GL_TEXTURE_SWIZZLE_R   0x00008e42u
#define ORACLE_GL_LINEAR              0x00002601u
#define ORACLE_GL_ALPHA               0x00001906u
#define ORACLE_GL_RED                 0x00001903u
#define ORACLE_GL_RGBA                0x00001908u
#define ORACLE_GL_ABGR_EXT            0x00008000u
#define ORACLE_GL_UNSIGNED_BYTE       0x00001401u
#define ORACLE_GL_SRC_ALPHA           0x00000302u
#define ORACLE_GL_ONE_MINUS_SRC_ALPHA 0x00000303u
#define ORACLE_GL_ONE                 1u
#define ORACLE_GL_LESS                0x00000201u
#define ORACLE_GL_LEQUAL              0x00000203u
#define ORACLE_GL_FLOAT               0x00001406u

#define ORACLE_TOKEN_BIND_FRAMEBUFFER 0x7eed2b3cu
/* Either raster A/B virtualizes the guest viewport (query from the logical
 * shadow, native replay on draw-binding changes); only the display raster
 * scales the default framebuffer's values. */
#if defined(ISAAC_VITA_DISPLAY_RASTER_720) || defined(ISAAC_VITA_FBO_RASTER_SCALE)
# define ORACLE_LOGICAL_VIEWPORT 1
#endif
#define ORACLE_TOKEN_BIND_RBO  0x7e9bfd5cu
#define ORACLE_TOKEN_CLEAR_DEPTH 0x7e6b0830u
#define ORACLE_TOKEN_DELETE_FRAMEBUFFERS 0x7e892c7du
#define ORACLE_TOKEN_DELETE_RBO 0x7efa9491u
#define ORACLE_TOKEN_GEN_RBO    0x7e9316cdu
#define ORACLE_TOKEN_GET_INTEGER 0x7ee6ba14u
#define ORACLE_TOKEN_QUERY_RBO  0x7e7972cau
#define ORACLE_TOKEN_STORE_RBO  0x7eca621du
#define ORACLE_TOKEN_VIEWPORT   0x7e9fd65eu
#define ORACLE_TOKEN_ACTIVE_TEXTURE 0x7e1f5017u
#define ORACLE_TOKEN_BLEND_SEPARATE 0x7e30124au
#define ORACLE_TOKEN_ENABLE_ATTRIB 0x7e4df411u
#define ORACLE_TOKEN_DELETE_TEXTURES 0x7e5f3dd4u
#define ORACLE_TOKEN_GEN_TEXTURES 0x7e63264au
#define ORACLE_TOKEN_DISABLE_ATTRIB 0x7ea27a94u
#define ORACLE_TOKEN_ATTRIB_POINTER 0x7ec4fcb9u
#define ORACLE_TOKEN_BIND_TEXTURE 0x7ecd2b7eu
#define ORACLE_TOKEN_FRAMEBUFFER_TEXTURE 0x7e122133u
#define ORACLE_TOKEN_TEX_IMAGE 0x7ee84ab1u
#define ORACLE_TOKEN_TEX_PARAMETER 0x7ec17871u
#define ORACLE_TOKEN_TEX_SUB_IMAGE 0x7eb9db1eu
#define ORACLE_TOKEN_DEPTH_FUNC 0x7edf529cu
#define ORACLE_TOKEN_CREATE_PROGRAM 0x7e2c4d40u
#define ORACLE_TOKEN_DELETE_PROGRAM 0x7e79d194u
#define ORACLE_TOKEN_DRAW_ELEMENTS 0x7e302306u
#define ORACLE_TOKEN_GET_PROGRAM 0x7eced544u
#define ORACLE_TOKEN_GET_UNIFORM 0x7e488552u
#define ORACLE_TOKEN_LINK_PROGRAM 0x7e106822u
#define ORACLE_TOKEN_UNIFORM1I 0x7efc8bdeu
#define ORACLE_TOKEN_UNIFORM4FV 0x7ed0e6f8u
#define ORACLE_TOKEN_UNIFORM_MATRIX4FV 0x7e194c0fu
#define ORACLE_TOKEN_USE_PROGRAM 0x7ef5cab2u

#define ORACLE_GL_LINK_STATUS 0x00008b82u
#define ORACLE_GL_CURRENT_PROGRAM 0x00008b8du
#define ORACLE_GL_TRIANGLES 0x00000004u
#define ORACLE_GL_UNSIGNED_SHORT 0x00001403u

#define ORACLE_VIEWPORT_CAPACITY 32u

enum oracle_gl_event {
    ORACLE_GL_EVENT_BIND_FRAMEBUFFER = 1,
    ORACLE_GL_EVENT_VIEWPORT = 2
};

typedef struct oracle_viewport_call {
    GLint x;
    GLint y;
    GLsizei width;
    GLsizei height;
} oracle_viewport_call;

static jmp_buf s_fault_jump;
static int s_fault_armed;
static uint32_t s_next_renderbuffer = 900u;
static unsigned s_native_gen_calls;
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
static unsigned s_native_gen_texture_calls;
static GLuint s_native_gen_texture_value;
static int s_native_gen_texture_write;
static uint64_t s_texture_profile_clock;
static uint64_t s_native_delete_texture_clock_cost;

uint64_t sceKernelGetProcessTimeWide(void)
{
    s_texture_profile_clock += 7u;
    return s_texture_profile_clock;
}
#endif
#if defined(ISAAC_VITA_GL_TIME_PROFILE) &&     !defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
/* Monotonic stub so the GL time brackets link and never report bad_clock. */
static uint64_t s_gl_time_clock;

uint64_t sceKernelGetProcessTimeWide(void)
{
    s_gl_time_clock += 3u;
    return s_gl_time_clock;
}
#endif
static unsigned s_native_delete_calls;
static unsigned s_native_clear_depth_calls;
static unsigned s_native_clear_calls;
static GLbitfield s_native_clear_mask;
static unsigned s_native_clear_color_calls;
static GLfloat s_native_clear_color[4];
static unsigned s_native_enable_calls;
static GLdouble s_native_clear_depth;
static unsigned s_native_bind_framebuffer_calls;
static GLenum s_native_bind_framebuffer_target;
static GLuint s_native_bind_framebuffer_name;
static unsigned s_native_active_texture_calls;
static GLenum s_native_active_texture;
static unsigned s_native_bind_texture_calls;
static GLenum s_native_bind_texture_target;
static GLuint s_native_bind_texture_name;
static GLuint s_native_bound_texture_2d;
static unsigned s_native_framebuffer_texture_calls;
static GLuint s_native_framebuffer_texture_name;
static unsigned s_native_tex_image_calls;
static GLenum s_native_tex_image_target;
static GLint s_native_tex_image_level;
static GLint s_native_tex_image_internal_format;
static GLsizei s_native_tex_image_width;
static GLsizei s_native_tex_image_height;
static GLint s_native_tex_image_border;
static GLenum s_native_tex_image_format;
static GLenum s_native_tex_image_type;
static const void *s_native_tex_image_pixels;
static unsigned s_native_tex_sub_image_calls;
static GLsizei s_native_tex_sub_image_width;
static GLsizei s_native_tex_sub_image_height;
static GLenum s_native_tex_sub_image_format;
static const void *s_native_tex_sub_image_pixels;
#if defined(ISAAC_VITA_FXRAY_ALPHA_MASK)
# define ORACLE_FXRAY_WIDTH 256u
# define ORACLE_FXRAY_HEIGHT 512u
# define ORACLE_FXRAY_PIXELS (ORACLE_FXRAY_WIDTH * ORACLE_FXRAY_HEIGHT)
# define ORACLE_FXRAY_LOG_CAPACITY 8u
/* Pinned vitaGL 73dd57a source/textures.c maps GL_ALPHA to this GXM
 * format and takes its one-byte fast-store path.  VitaSDK names the
 * single-channel swizzle in ABGR order: R111 therefore samples as RGBA
 * (1, 1, 1, R), not merely as an abstract desktop-GL promise. */
# define ORACLE_SCE_GXM_TEXTURE_FORMAT_U8_R111 0x00007000u
static uint8_t s_fxray_rgba[ORACLE_FXRAY_PIXELS * 4u];
static uint8_t s_fxray_expected_alpha[ORACLE_FXRAY_PIXELS];
static unsigned s_fxray_alpha_uploads;
static unsigned s_fxray_alpha_mismatches;
static unsigned s_fxray_sample_mismatches;
static unsigned s_fxray_u8_r111_uploads;
static unsigned s_fxray_restore_rows;
static unsigned s_fxray_restore_mismatches;
static unsigned s_fxray_restore_next_y;
static int s_fxray_verify_upload;
static int s_fxray_verify_restore;
static char s_fxray_logs[ORACLE_FXRAY_LOG_CAPACITY][192];
static unsigned s_fxray_log_calls;

_Static_assert(ORACLE_SCE_GXM_TEXTURE_FORMAT_U8_R111 == 0x00007000u,
               "pinned GXM U8_R111 swizzle changed");
#endif
static unsigned s_native_blend_calls;
static GLenum s_native_blend[4];
static unsigned s_native_delete_texture_calls;
static unsigned s_native_depth_calls;
static GLenum s_native_depth;
static unsigned s_native_enable_attrib_calls;
static unsigned s_native_disable_attrib_calls;
static GLuint s_native_attrib_index;
static unsigned s_native_attrib_pointer_calls;
static GLint s_native_attrib_size;
static GLenum s_native_attrib_type;
static GLboolean s_native_attrib_normalized;
static GLsizei s_native_attrib_stride;
static const void *s_native_attrib_pointer;
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
static uint16_t s_native_attrib_mask, s_native_attrib_observed;
static uint16_t s_native_canonical_attrib_observed;
static unsigned s_native_attrib_invalid_mutation;
#endif
#if defined(ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE)
/* Ordered fake-vitaGL call trace for the direct-state comparison (attribute
 * toggles and pointers, draws and program binds, with their arguments). */
static char s_direct_trace[1u << 15];
static size_t s_direct_trace_len;
static int s_direct_trace_overflow;
static void oracle_direct_trace(const char *format, ...)
{
    va_list args;
    int n;

    va_start(args, format);
    n = vsnprintf(s_direct_trace + s_direct_trace_len,
                  sizeof s_direct_trace - s_direct_trace_len, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= sizeof s_direct_trace - s_direct_trace_len) {
        s_direct_trace_overflow = 1;
        return;
    }
    s_direct_trace_len += (size_t)n;
}
# define ORACLE_DIRECT_TRACE(...) oracle_direct_trace(__VA_ARGS__)
/* Foreign table members for the decline checks: must never be reached. */
static unsigned oracle_backend_fake_calls;
static void oracle_backend_fake_toggle(guest_gl_uint index)
{
    (void)index;
    ++oracle_backend_fake_calls;
}
static guest_gl_int oracle_backend_fake_location(guest_gl_uint program,
                                                 guest_gl_addr name)
{
    (void)program;
    (void)name;
    ++oracle_backend_fake_calls;
    return -1;
}
#else
# define ORACLE_DIRECT_TRACE(...) ((void)0)
#endif
static unsigned s_native_get_integer_calls;
static GLuint s_native_next_program = 7u;
static GLint s_native_link_status;
static GLint s_native_uniform_location = 0x1234;
static unsigned s_native_create_program_calls;
static unsigned s_native_delete_program_calls;
static unsigned s_native_draw_elements_calls;
static GLenum s_native_draw_mode;
static GLsizei s_native_draw_count;
static GLenum s_native_draw_type;
static const void *s_native_draw_indices;
static unsigned s_native_canonical_quad_calls;
static GLsizei s_native_canonical_quad_count;
static GLboolean s_native_canonical_quad_result;
static unsigned s_native_get_program_calls;
static unsigned s_native_get_shader_calls;
static unsigned s_native_shader_source_calls;
static unsigned s_native_coloroffset_mark_calls;
static unsigned s_native_get_uniform_calls;
static unsigned s_native_link_program_calls;
static unsigned s_native_use_program_calls;
static unsigned s_native_uniform1i_calls;
static unsigned s_native_uniform4fv_calls;
static unsigned s_native_uniform_matrix4fv_calls;
static GLuint s_native_last_program;
static GLuint s_native_current_program;
static int s_native_reject_use_program;
static GLint s_native_last_uniform_location;
static GLint s_native_last_uniform1i;
static GLsizei s_native_last_uniform_count;
static GLboolean s_native_last_uniform_transpose;
static GLfloat s_native_last_uniform_payload[16];
static oracle_viewport_call s_native_viewports[ORACLE_VIEWPORT_CAPACITY];
static unsigned s_native_viewport_calls;
static unsigned s_gl_events[ORACLE_VIEWPORT_CAPACITY * 2u];
static unsigned s_gl_event_count;
static char s_rt_log[384];
static int s_rt_log_result;
static unsigned s_rt_log_calls;
static unsigned s_rt_profile_calls;
static unsigned s_rt_fault_calls;
static const char *s_rt_profile_reason;
static const char *s_rt_fault_message;
static uint32_t s_rt_fault_result;
#if defined(ISAAC_VITA_IO_PROFILE)
static unsigned s_memory_snapshot_calls;
static int32_t s_memory_snapshot_event_result;
static const void *s_memory_snapshot_target;
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "RBO oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int kage_vita_backend_ready(void)
{
    return 1;
}

#if defined(ISAAC_VITA_IO_PROFILE)
void kage_vita_backend_memory_snapshot_at_first_fbo(
    int32_t event_result, const void *target)
{
    ++s_memory_snapshot_calls;
    s_memory_snapshot_event_result = event_result;
    s_memory_snapshot_target = target;
}

void kage_vita_io_profile_texture_begin(
    uint32_t kind, int32_t width, int32_t height)
{
    (void)kind;
    (void)width;
    (void)height;
}

void kage_vita_io_profile_texture_end(void) {}
#endif

void guest_fault(CPU *__restrict c, uint32_t address, const char *message)
{
    c->fault_addr = address;
    c->fault = message;
    if (s_fault_armed)
        longjmp(s_fault_jump, 1);
    abort();
}

void isaac_gl_vita_oracle_rt_log(const char *format, ...)
{
    va_list arguments;

    ++s_rt_log_calls;
    va_start(arguments, format);
    s_rt_log_result = vsnprintf(
        s_rt_log, sizeof s_rt_log, format, arguments);
    va_end(arguments);
    s_rt_log[sizeof s_rt_log - 1u] = '\0';
}

#if defined(ISAAC_VITA_FXRAY_ALPHA_MASK)
void isaac_vita_log(const char *format, ...)
{
    va_list arguments;
    unsigned slot = s_fxray_log_calls;

    ++s_fxray_log_calls;
    if (slot >= ORACLE_FXRAY_LOG_CAPACITY)
        return;
    va_start(arguments, format);
    (void)vsnprintf(
        s_fxray_logs[slot], sizeof s_fxray_logs[slot], format, arguments);
    va_end(arguments);
    s_fxray_logs[slot][sizeof s_fxray_logs[slot] - 1u] = '\0';
}

static unsigned oracle_fxray_log_matches(const char *needle)
{
    unsigned count = 0u;
    unsigned limit = s_fxray_log_calls < ORACLE_FXRAY_LOG_CAPACITY
        ? s_fxray_log_calls : ORACLE_FXRAY_LOG_CAPACITY;
    unsigned index;

    for (index = 0u; index < limit; ++index)
        if (strstr(s_fxray_logs[index], needle))
            ++count;
    return count;
}

static void oracle_sample_u8_r111(uint8_t stored, uint8_t sampled[4])
{
    /* The suffix is ABGR: A=R, B=1, G=1, R=1. */
    sampled[0] = 255u;
    sampled[1] = 255u;
    sampled[2] = 255u;
    sampled[3] = stored;
}
#endif

void isaac_gl_vita_oracle_rt_profile_report(const char *reason)
{
    ++s_rt_profile_calls;
    s_rt_profile_reason = reason;
}

#if defined(ISAAC_VITA_FBO_RASTER_SCALE) && !defined(ISAAC_VITA_FXRAY_ALPHA_MASK)
/* The raster policy logs a scaled-target readback once; nothing here reads
 * a scaled target back, so the sink only has to exist. */
static unsigned s_fbo_raster_log_calls;

void isaac_vita_log(const char *format, ...)
{
    (void)format;
    ++s_fbo_raster_log_calls;
}
#endif

void isaac_gl_vita_oracle_rt_fault(uint32_t result, const char *message)
{
    ++s_rt_fault_calls;
    s_rt_fault_result = result;
    s_rt_fault_message = message;
}

static void oracle_rt_reset(void)
{
    memset(s_rt_log, 0, sizeof s_rt_log);
    s_rt_log_result = -1;
    s_rt_log_calls = 0u;
    s_rt_profile_calls = 0u;
    s_rt_fault_calls = 0u;
    s_rt_profile_reason = NULL;
    s_rt_fault_message = NULL;
    s_rt_fault_result = 0u;
}

void oracle_glNoop(void) {}
GLuint oracle_glReturnUint(void) { return 1u; }
GLint oracle_glReturnInt(void) { return 0; }
const GLubyte *oracle_glReturnString(void)
{
    static const GLubyte value[] = "oracle";
    return value;
}

GLuint oracle_glCreateProgram(void)
{
    ++s_native_create_program_calls;
    return s_native_next_program;
}

void oracle_glDeleteProgram(GLuint program)
{
    ++s_native_delete_program_calls;
    s_native_last_program = program;
}

void oracle_glGetProgramiv(GLuint program, GLenum name, GLint *value)
{
    ++s_native_get_program_calls;
    s_native_last_program = program;
    if ((uintptr_t)value == 2u)
        return;
    if (value)
        *value = name == ORACLE_GL_LINK_STATUS ? s_native_link_status : 0;
}

void oracle_glGetShaderiv(GLuint shader, GLenum name, GLint *value)
{
    ++s_native_get_shader_calls;
    s_native_last_program = shader;
    (void)name;
    if (value)
        *value = 0;
}

void oracle_glShaderSource(
    GLuint shader, GLsizei count, const GLchar *const *strings,
    const GLint *lengths)
{
    ++s_native_shader_source_calls;
    s_native_last_program = shader;
    (void)count;
    (void)strings;
    (void)lengths;
}

void vglIsaacMarkColorOffsetShader(
    uint32_t shader, uint32_t source_fnv1a,
    uint32_t source_first512_fnv1a, uint32_t source_size)
{
    ++s_native_coloroffset_mark_calls;
    s_native_last_program = shader;
    (void)source_fnv1a;
    (void)source_first512_fnv1a;
    (void)source_size;
}

GLint oracle_glGetUniformLocation(GLuint program, const GLchar *name)
{
    ++s_native_get_uniform_calls;
    s_native_last_program = program;
    (void)name;
    return s_native_uniform_location;
}

void oracle_glLinkProgram(GLuint program)
{
    ++s_native_link_program_calls;
    s_native_last_program = program;
}

void oracle_glUniform1i(GLint location, GLint value)
{
    ++s_native_uniform1i_calls;
    s_native_last_uniform_location = location;
    s_native_last_uniform1i = value;
}

void oracle_glUniform4fv(
    GLint location, GLsizei count, const GLfloat *value)
{
    ++s_native_uniform4fv_calls;
    s_native_last_uniform_location = location;
    s_native_last_uniform_count = count;
    if (location == 0 || location == -1)
        return;
    if ((uintptr_t)value == 1u)
        guest_gl_backend_fault(
            ORACLE_TOKEN_UNIFORM4FV,
            "oracle native glUniform4fv received hostile pointer");
    if ((uintptr_t)value == 2u)
        return;
    if (value && count > 0)
        memcpy(s_native_last_uniform_payload, value,
               4u * sizeof s_native_last_uniform_payload[0]);
}

void oracle_glUniformMatrix4fv(
    GLint location, GLsizei count, GLboolean transpose,
    const GLfloat *value)
{
    ++s_native_uniform_matrix4fv_calls;
    s_native_last_uniform_location = location;
    s_native_last_uniform_count = count;
    s_native_last_uniform_transpose = transpose;
    if (location == 0 || location == -1)
        return;
    if ((uintptr_t)value == 1u)
        guest_gl_backend_fault(
            ORACLE_TOKEN_UNIFORM_MATRIX4FV,
            "oracle native glUniformMatrix4fv received hostile pointer");
    if (value && count > 0)
        memcpy(s_native_last_uniform_payload, value,
               sizeof s_native_last_uniform_payload);
}

void oracle_glUseProgram(GLuint program)
{
    ++s_native_use_program_calls;
    s_native_last_program = program;
    ORACLE_DIRECT_TRACE("u%u;", (unsigned)program);
    if (!s_native_reject_use_program)
        s_native_current_program = program;
}

void oracle_glClearDepth(GLdouble depth)
{
    ++s_native_clear_depth_calls;
    s_native_clear_depth = depth;
}

void oracle_glClear(GLbitfield mask)
{
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
    s_native_attrib_observed = s_native_attrib_mask;
#endif
    ++s_native_clear_calls;
    s_native_clear_mask = mask;
}

void oracle_glClearColor(
    GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
    ++s_native_clear_color_calls;
    s_native_clear_color[0] = red;
    s_native_clear_color[1] = green;
    s_native_clear_color[2] = blue;
    s_native_clear_color[3] = alpha;
}

void oracle_glEnable(GLenum capability)
{
    (void)capability;
    ++s_native_enable_calls;
}

void oracle_glBindRenderbuffer(GLenum target, GLuint renderbuffer)
{
    (void)target;
    (void)renderbuffer;
}

void oracle_glBindFramebuffer(GLenum target, GLuint framebuffer)
{
    ++s_native_bind_framebuffer_calls;
    s_native_bind_framebuffer_target = target;
    s_native_bind_framebuffer_name = framebuffer;
    if (s_gl_event_count < sizeof s_gl_events / sizeof s_gl_events[0])
        s_gl_events[s_gl_event_count++] = ORACLE_GL_EVENT_BIND_FRAMEBUFFER;
}

void oracle_glActiveTexture(GLenum texture)
{
    ++s_native_active_texture_calls;
    s_native_active_texture = texture;
}

void oracle_glBindTexture(GLenum target, GLuint texture)
{
    ++s_native_bind_texture_calls;
    s_native_bind_texture_target = target;
    s_native_bind_texture_name = texture;
    if (target == ORACLE_GL_TEXTURE_2D)
        s_native_bound_texture_2d = texture;
}

void oracle_glFramebufferTexture2D(
    GLenum target, GLenum attachment, GLenum texture_target,
    GLuint texture, GLint level)
{
    (void)target;
    (void)attachment;
    (void)texture_target;
    (void)level;
    ++s_native_framebuffer_texture_calls;
    s_native_framebuffer_texture_name = texture;
}

void oracle_glTexImage2D(
    GLenum target, GLint level, GLint internal_format,
    GLsizei width, GLsizei height, GLint border,
    GLenum format, GLenum type, const void *pixels)
{
    ++s_native_tex_image_calls;
    s_native_tex_image_target = target;
    s_native_tex_image_level = level;
    s_native_tex_image_internal_format = internal_format;
    s_native_tex_image_width = width;
    s_native_tex_image_height = height;
    s_native_tex_image_border = border;
    s_native_tex_image_format = format;
    s_native_tex_image_type = type;
    s_native_tex_image_pixels = pixels;
#if defined(ISAAC_VITA_FXRAY_ALPHA_MASK)
    if (s_fxray_verify_upload && target == ORACLE_GL_TEXTURE_2D &&
            level == 0 && internal_format == (GLint)ORACLE_GL_ALPHA &&
            width == (GLsizei)ORACLE_FXRAY_WIDTH &&
            height == (GLsizei)ORACLE_FXRAY_HEIGHT && border == 0 &&
            format == ORACLE_GL_ALPHA &&
            type == ORACLE_GL_UNSIGNED_BYTE && pixels) {
        const uint8_t *alpha = (const uint8_t *)pixels;
        uint32_t index;

        ++s_fxray_alpha_uploads;
        ++s_fxray_u8_r111_uploads;
        for (index = 0u; index < ORACLE_FXRAY_PIXELS; ++index) {
            uint8_t sampled[4];

            if (alpha[index] != s_fxray_expected_alpha[index])
                ++s_fxray_alpha_mismatches;
            oracle_sample_u8_r111(alpha[index], sampled);
            if (sampled[0] != 255u || sampled[1] != 255u ||
                    sampled[2] != 255u ||
                    sampled[3] != s_fxray_expected_alpha[index])
                ++s_fxray_sample_mismatches;
        }
    }
    if (s_fxray_verify_restore && target == ORACLE_GL_TEXTURE_2D &&
            level == 0 && internal_format == (GLint)ORACLE_GL_RGBA &&
            width == (GLsizei)ORACLE_FXRAY_WIDTH &&
            height == (GLsizei)ORACLE_FXRAY_HEIGHT && border == 0 &&
            format == ORACLE_GL_RGBA &&
            type == ORACLE_GL_UNSIGNED_BYTE && !pixels) {
        s_fxray_restore_next_y = 0u;
    }
#endif
}

void oracle_glTexSubImage2D(
    GLenum target, GLint level, GLint x_offset, GLint y_offset,
    GLsizei width, GLsizei height, GLenum format, GLenum type,
    const void *pixels)
{
    (void)target;
    (void)level;
    ++s_native_tex_sub_image_calls;
    s_native_tex_sub_image_width = width;
    s_native_tex_sub_image_height = height;
    s_native_tex_sub_image_format = format;
    s_native_tex_sub_image_pixels = pixels;
#if defined(ISAAC_VITA_FXRAY_ALPHA_MASK)
    if (s_fxray_verify_restore && target == ORACLE_GL_TEXTURE_2D &&
            level == 0 && x_offset == 0 &&
            y_offset == (GLint)s_fxray_restore_next_y &&
            width == (GLsizei)ORACLE_FXRAY_WIDTH && height == 1 &&
            format == ORACLE_GL_RGBA && type == ORACLE_GL_UNSIGNED_BYTE &&
            pixels && s_fxray_restore_next_y < ORACLE_FXRAY_HEIGHT) {
        const uint8_t *rgba = (const uint8_t *)pixels;
        uint32_t x;

        for (x = 0u; x < ORACLE_FXRAY_WIDTH; ++x) {
            uint32_t offset = x * 4u;
            uint8_t expected = s_fxray_expected_alpha[
                s_fxray_restore_next_y * ORACLE_FXRAY_WIDTH + x];

            if (rgba[offset] != 255u || rgba[offset + 1u] != 255u ||
                    rgba[offset + 2u] != 255u ||
                    rgba[offset + 3u] != expected)
                ++s_fxray_restore_mismatches;
        }
        ++s_fxray_restore_rows;
        ++s_fxray_restore_next_y;
    }
#else
    (void)x_offset;
    (void)y_offset;
    (void)type;
#endif
}

void oracle_glBlendFuncSeparate(
    GLenum source_rgb, GLenum destination_rgb,
    GLenum source_alpha, GLenum destination_alpha)
{
    ++s_native_blend_calls;
    s_native_blend[0] = source_rgb;
    s_native_blend[1] = destination_rgb;
    s_native_blend[2] = source_alpha;
    s_native_blend[3] = destination_alpha;
}

void oracle_glDeleteTextures(GLsizei count, const GLuint *textures)
{
    GLsizei index;

    ++s_native_delete_texture_calls;
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    /* Make the native interval observably different from post bookkeeping. */
    s_texture_profile_clock += s_native_delete_texture_clock_cost;
#endif
    if (!textures || count <= 0)
        return;
    for (index = 0; index < count; ++index)
        if (textures[index] == s_native_bound_texture_2d)
            s_native_bound_texture_2d = 0u;
}

void oracle_glDepthFunc(GLenum function)
{
    ++s_native_depth_calls;
    s_native_depth = function;
}

void oracle_glEnableVertexAttribArray(GLuint index)
{
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
    s_native_attrib_observed = s_native_attrib_mask;
    if (index < 16u)
        s_native_attrib_mask |= (uint16_t)(1u << index);
    else if (s_native_attrib_invalid_mutation)
        s_native_attrib_mask ^= 1u; /* Explicit hostile model, no undefined shift. */
#endif
    ++s_native_enable_attrib_calls;
    s_native_attrib_index = index;
    ORACLE_DIRECT_TRACE("E%u;", (unsigned)index);
}

void oracle_glDisableVertexAttribArray(GLuint index)
{
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
    s_native_attrib_observed = s_native_attrib_mask;
    if (index < 16u)
        s_native_attrib_mask &= (uint16_t)~(1u << index);
    else if (s_native_attrib_invalid_mutation)
        s_native_attrib_mask ^= 1u;
#endif
    ++s_native_disable_attrib_calls;
    s_native_attrib_index = index;
    ORACLE_DIRECT_TRACE("D%u;", (unsigned)index);
}

void oracle_glVertexAttribPointer(
    GLuint index, GLint size, GLenum type, GLboolean normalized,
    GLsizei stride, const void *pointer)
{
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
    s_native_attrib_observed = s_native_attrib_mask;
#endif
    ++s_native_attrib_pointer_calls;
    s_native_attrib_index = index;
    ORACLE_DIRECT_TRACE("P%u,%d,%x,%u,%d,%lx;", (unsigned)index, (int)size,
                        (unsigned)type, (unsigned)normalized, (int)stride,
                        (unsigned long)(uintptr_t)pointer);
    s_native_attrib_size = size;
    s_native_attrib_type = type;
    s_native_attrib_normalized = normalized;
    s_native_attrib_stride = stride;
    s_native_attrib_pointer = pointer;
}

void oracle_glGetIntegerv(GLenum name, GLint *value)
{
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
    s_native_attrib_observed = s_native_attrib_mask;
#endif
    ++s_native_get_integer_calls;
    if (!value)
        return;
    if (name == ORACLE_GL_CURRENT_PROGRAM) {
        value[0] = (GLint)s_native_current_program;
    } else if (name == ORACLE_GL_VIEWPORT) {
        value[0] = 11;
        value[1] = 22;
        value[2] = 333;
        value[3] = 444;
    } else if (name == ORACLE_GL_TEXTURE_BINDING_2D) {
        value[0] = (GLint)s_native_bound_texture_2d;
    } else {
        value[0] = 0x12345678;
    }
}

void oracle_glViewport(
    GLint x, GLint y, GLsizei width, GLsizei height)
{
    if (s_native_viewport_calls < ORACLE_VIEWPORT_CAPACITY) {
        oracle_viewport_call *call =
            &s_native_viewports[s_native_viewport_calls];
        call->x = x;
        call->y = y;
        call->width = width;
        call->height = height;
    }
    ++s_native_viewport_calls;
    if (s_gl_event_count < sizeof s_gl_events / sizeof s_gl_events[0])
        s_gl_events[s_gl_event_count++] = ORACLE_GL_EVENT_VIEWPORT;
}

void oracle_glDeleteRenderbuffers(
    GLsizei count, const GLuint *renderbuffers)
{
    (void)count;
    (void)renderbuffers;
    ++s_native_delete_calls;
}

void oracle_glDeleteFramebuffers(
    GLsizei count, const GLuint *framebuffers)
{
    (void)count;
    (void)framebuffers;
}

void oracle_glDrawElements(
    GLenum mode, GLsizei count, GLenum type, const void *indices)
{
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
    s_native_attrib_observed = s_native_attrib_mask;
#endif
    ++s_native_draw_elements_calls;
    s_native_draw_mode = mode;
    ORACLE_DIRECT_TRACE("d%x,%d,%x,%lx;", (unsigned)mode, (int)count,
                        (unsigned)type, (unsigned long)(uintptr_t)indices);
    s_native_draw_count = count;
    s_native_draw_type = type;
    s_native_draw_indices = indices;
}

GLboolean vglIsaacDrawCanonicalQuads(GLsizei count)
{
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
    s_native_attrib_observed = s_native_attrib_mask;
    s_native_canonical_attrib_observed = s_native_attrib_mask;
#endif
    ++s_native_canonical_quad_calls;
    s_native_canonical_quad_count = count;
    return s_native_canonical_quad_result;
}

void oracle_glGenRenderbuffers(GLsizei count, GLuint *renderbuffers)
{
    GLsizei index;

    ++s_native_gen_calls;
    for (index = 0; index < count; ++index)
        renderbuffers[index] = s_next_renderbuffer++;
}

void oracle_glGenTextures(GLsizei count, GLuint *textures)
{
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    ++s_native_gen_texture_calls;
    if (s_native_gen_texture_write && count > 0 && textures)
        textures[0] = s_native_gen_texture_value;
#else
    (void)count;
    (void)textures;
#endif
}

void oracle_glRenderbufferStorage(
    GLenum target, GLenum format, GLsizei width, GLsizei height)
{
    (void)target;
    (void)format;
    (void)width;
    (void)height;
}

static int oracle_dispatch_with_return(
    CPU *cpu, uint32_t stack_top, uint32_t token,
    const uint32_t *arguments, uint32_t argument_count,
    uint32_t return_word)
{
    uint32_t index;

    cpu->esp = stack_top;
    for (index = argument_count; index != 0u; --index)
        gpush(cpu, arguments[index - 1u]);
    gpush(cpu, return_word);
    if (!guest_gl_dispatch(cpu, token))
        return 0;
    return cpu->esp == stack_top;
}

static int oracle_dispatch(
    CPU *cpu, uint32_t stack_top, uint32_t token,
    const uint32_t *arguments, uint32_t argument_count)
{
    return oracle_dispatch_with_return(
        cpu, stack_top, token, arguments, argument_count, 0xaabbccddu);
}

static int oracle_expect_fault(
    CPU *cpu, uint32_t stack_top, uint32_t token,
    const uint32_t *arguments, uint32_t argument_count,
    uint32_t expected_address, const char *expected_message)
{
    cpu->fault = NULL;
    cpu->fault_addr = 0u;
    s_fault_armed = 1;
    if (setjmp(s_fault_jump) == 0) {
        (void)oracle_dispatch(
            cpu, stack_top, token, arguments, argument_count);
        s_fault_armed = 0;
        return 0;
    }
    s_fault_armed = 0;
    return cpu->fault_addr == expected_address && cpu->fault &&
           strcmp(cpu->fault, expected_message) == 0;
}

static int oracle_query(
    CPU *cpu, uint32_t stack_top, uint32_t name,
    uint32_t redzone[3], uint32_t expected)
{
    uint32_t arguments[3];

    redzone[0] = 0x13579bdfu;
    redzone[1] = 0xccccccccu;
    redzone[2] = 0x2468ace0u;
    arguments[0] = ORACLE_GL_RENDERBUFFER;
    arguments[1] = name;
    arguments[2] = (uint32_t)(uintptr_t)&redzone[1];
    if (!oracle_dispatch(
            cpu, stack_top, ORACLE_TOKEN_QUERY_RBO, arguments, 3u))
        return 0;
    return redzone[0] == 0x13579bdfu && redzone[1] == expected &&
           redzone[2] == 0x2468ace0u;
}

static int oracle_set_viewport(
    CPU *cpu, uint32_t stack_top,
    int32_t x, int32_t y, int32_t width, int32_t height)
{
    uint32_t arguments[4];

    arguments[0] = (uint32_t)x;
    arguments[1] = (uint32_t)y;
    arguments[2] = (uint32_t)width;
    arguments[3] = (uint32_t)height;
    return oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_VIEWPORT, arguments, 4u);
}

static int oracle_bind_framebuffer(
    CPU *cpu, uint32_t stack_top, uint32_t target, uint32_t framebuffer)
{
    uint32_t arguments[2];

    arguments[0] = target;
    arguments[1] = framebuffer;
    return oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BIND_FRAMEBUFFER, arguments, 2u);
}

static int oracle_delete_framebuffer(
    CPU *cpu, uint32_t stack_top, uint32_t *framebuffer)
{
    uint32_t arguments[2];

    arguments[0] = 1u;
    arguments[1] = (uint32_t)(uintptr_t)framebuffer;
    return oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_DELETE_FRAMEBUFFERS, arguments, 2u);
}

static int oracle_get_viewport(
    CPU *cpu, uint32_t stack_top, int32_t redzone[6])
{
    uint32_t arguments[2];

    redzone[0] = (int32_t)0x89abcdefu;
    redzone[1] = redzone[2] = redzone[3] = redzone[4] = 0x55555555;
    redzone[5] = 0x76543210;
    arguments[0] = ORACLE_GL_VIEWPORT;
    arguments[1] = (uint32_t)(uintptr_t)&redzone[1];
    if (!oracle_dispatch(
            cpu, stack_top, ORACLE_TOKEN_GET_INTEGER, arguments, 2u))
        return 0;
    return (uint32_t)redzone[0] == 0x89abcdefu &&
        redzone[5] == 0x76543210;
}

static int oracle_last_viewport_is(
    int32_t x, int32_t y, int32_t width, int32_t height)
{
    const oracle_viewport_call *call;

    if (s_native_viewport_calls == 0u ||
            s_native_viewport_calls > ORACLE_VIEWPORT_CAPACITY)
        return 0;
    call = &s_native_viewports[s_native_viewport_calls - 1u];
    return call->x == x && call->y == y &&
        call->width == width && call->height == height;
}

static void oracle_reset_gl_events(void)
{
    memset(s_gl_events, 0, sizeof s_gl_events);
    s_gl_event_count = 0u;
}

static int oracle_bind_texture(
    CPU *cpu, uint32_t stack_top, uint32_t texture)
{
    uint32_t arguments[2] = { ORACLE_GL_TEXTURE_2D, texture };

    return oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BIND_TEXTURE, arguments, 2u);
}

static int oracle_tex_image(
    CPU *cpu, uint32_t stack_top, int32_t level,
    int32_t internal_format, int32_t width, int32_t height,
    int32_t border, uint32_t format, uint32_t type, const void *pixels)
{
    uint32_t arguments[9];

    arguments[0] = ORACLE_GL_TEXTURE_2D;
    arguments[1] = (uint32_t)level;
    arguments[2] = (uint32_t)internal_format;
    arguments[3] = (uint32_t)width;
    arguments[4] = (uint32_t)height;
    arguments[5] = (uint32_t)border;
    arguments[6] = format;
    arguments[7] = type;
    arguments[8] = (uint32_t)(uintptr_t)pixels;
    return oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_TEX_IMAGE, arguments, 9u);
}

#if defined(ISAAC_VITA_FXRAY_ALPHA_MASK) || \
    defined(ISAAC_VITA_FBO_CLEAR_ELISION) || \
    defined(ISAAC_VITA_FBO_RASTER_SCALE)
static int oracle_tex_sub_image(
    CPU *cpu, uint32_t stack_top, int32_t width, int32_t height,
    const void *pixels)
{
    uint32_t arguments[9];

    arguments[0] = ORACLE_GL_TEXTURE_2D;
    arguments[1] = 0u;
    arguments[2] = 0u;
    arguments[3] = 0u;
    arguments[4] = (uint32_t)width;
    arguments[5] = (uint32_t)height;
    arguments[6] = ORACLE_GL_RGBA;
    arguments[7] = ORACLE_GL_UNSIGNED_BYTE;
    arguments[8] = (uint32_t)(uintptr_t)pixels;
    return oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_TEX_SUB_IMAGE, arguments, 9u);
}
#endif

#if defined(ISAAC_VITA_FXRAY_ALPHA_MASK)
static int oracle_tex_parameter(
    CPU *cpu, uint32_t stack_top, uint32_t name, uint32_t value)
{
    uint32_t arguments[3];

    arguments[0] = ORACLE_GL_TEXTURE_2D;
    arguments[1] = name;
    arguments[2] = value;
    return oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_TEX_PARAMETER, arguments, 3u);
}
#endif

#if defined(ISAAC_VITA_FXRAY_ALPHA_MASK)
static void oracle_fxray_prepare_rgba(void)
{
    uint32_t index;

    for (index = 0u; index < ORACLE_FXRAY_PIXELS; ++index) {
        uint32_t offset = index * 4u;
        uint8_t alpha = (uint8_t)((index * 37u + index / 257u) & 255u);

        s_fxray_rgba[offset] = 255u;
        s_fxray_rgba[offset + 1u] = 255u;
        s_fxray_rgba[offset + 2u] = 255u;
        s_fxray_rgba[offset + 3u] = alpha;
        s_fxray_expected_alpha[index] = alpha;
    }
}

static void oracle_fxray_reset_verifier(void)
{
    s_fxray_alpha_uploads = 0u;
    s_fxray_alpha_mismatches = 0u;
    s_fxray_sample_mismatches = 0u;
    s_fxray_u8_r111_uploads = 0u;
    s_fxray_restore_rows = 0u;
    s_fxray_restore_mismatches = 0u;
    s_fxray_restore_next_y = 0u;
    s_fxray_verify_upload = 1;
    s_fxray_verify_restore = 1;
    memset(s_fxray_logs, 0, sizeof s_fxray_logs);
    s_fxray_log_calls = 0u;
}

static int oracle_test_fxray_alpha_mask(CPU *cpu, uint32_t stack_top)
{
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    IsaacVitaTextureChurnProfile profile;
#endif
    uint8_t subpixel[4] = { 7u, 8u, 9u, 10u };
    uint32_t arguments[5];
    uint32_t deleted;
    unsigned bind_before;
    unsigned framebuffer_before;
    unsigned image_before;
    unsigned sub_before;

    gl_vita_backend_uninstall();
    CHECK(gl_vita_backend_install());
    oracle_fxray_prepare_rgba();
    oracle_fxray_reset_verifier();

    /* Shape rejection happens before touching source bytes. */
    CHECK(oracle_bind_texture(cpu, stack_top, 100u));
    image_before = s_native_tex_image_calls;
    CHECK(oracle_tex_image(
        cpu, stack_top, 0, ORACLE_GL_RGBA, 255,
        (int32_t)ORACLE_FXRAY_HEIGHT, 0,
        ORACLE_GL_RGBA, ORACLE_GL_UNSIGNED_BYTE,
        (const void *)(uintptr_t)1u));
    CHECK(s_native_tex_image_calls == image_before + 1u);
    CHECK(s_native_tex_image_internal_format == (GLint)ORACLE_GL_RGBA);
    CHECK((uintptr_t)s_native_tex_image_pixels == 1u);

    /* The exact white-RGB 256x512 shape is uploaded once as GL_ALPHA, with
     * every source alpha byte retained. */
    CHECK(oracle_bind_texture(cpu, stack_top, 101u));
    image_before = s_native_tex_image_calls;
    CHECK(oracle_tex_image(
        cpu, stack_top, 0, ORACLE_GL_RGBA,
        (int32_t)ORACLE_FXRAY_WIDTH, (int32_t)ORACLE_FXRAY_HEIGHT,
        0, ORACLE_GL_RGBA, ORACLE_GL_UNSIGNED_BYTE, s_fxray_rgba));
    CHECK(s_native_tex_image_calls == image_before + 1u);
    CHECK(s_native_tex_image_target == ORACLE_GL_TEXTURE_2D);
    CHECK(s_native_tex_image_level == 0);
    CHECK(s_native_tex_image_internal_format == (GLint)ORACLE_GL_ALPHA);
    CHECK(s_native_tex_image_width == (GLsizei)ORACLE_FXRAY_WIDTH);
    CHECK(s_native_tex_image_height == (GLsizei)ORACLE_FXRAY_HEIGHT);
    CHECK(s_native_tex_image_border == 0);
    CHECK(s_native_tex_image_format == ORACLE_GL_ALPHA);
    CHECK(s_native_tex_image_type == ORACLE_GL_UNSIGNED_BYTE);
    CHECK(s_native_tex_image_pixels != s_fxray_rgba);
    CHECK(s_fxray_alpha_uploads == 1u && s_fxray_alpha_mismatches == 0u);
    CHECK(s_fxray_u8_r111_uploads == 1u &&
          s_fxray_sample_mismatches == 0u);
    CHECK(oracle_fxray_log_matches("phase=upload") == 1u);
    CHECK(oracle_fxray_log_matches("hit=1 slot=0 name=101") == 1u);

#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    /* The diagnostic build enables FXRay compaction too.  Prove that the
     * rejected 255x512 request remains a linear native upload while the
     * compacted 256x512 request is charged as one-byte converted storage. */
    gl_vita_backend_texture_churn_profile_take_window(&profile);
    CHECK(profile.gen_calls == 0u && profile.gen_names == 0u);
    CHECK(profile.gen_ambiguous == 0u && profile.gen_scan_slots == 0u);
    CHECK(profile.image_first == 0u && profile.image_redefine == 0u);
    CHECK(profile.image_unknown == 2u && profile.image_other_level == 0u);
    CHECK(profile.path_linear == 1u);
    CHECK(profile.path_converted == 1u);
    CHECK(profile.path_other == 0u);
    CHECK(profile.known_pixels == 261632u);
    CHECK(profile.known_alloc_bytes == 655360u);
    CHECK(profile.image_observed_us == 14u);
    CHECK(profile.image_max_observed_us == 7u);
    CHECK(profile.logical_live == 0u && profile.window_peak_live == 0u);
    CHECK(profile.recycled_names == 0u && profile.bad == 0u);
    CHECK(profile.clock_calls == 4u && profile.clock_pair_max_us == 7u);
#endif

    /* Frozen KAGE's real post-bind MIN/MAG/WRAP state remains on the compact
     * texture.  None of these sample-invariant pnames may reconstruct RGBA. */
    image_before = s_native_tex_image_calls;
    sub_before = s_native_tex_sub_image_calls;
    CHECK(oracle_tex_parameter(
        cpu, stack_top, ORACLE_GL_TEXTURE_MIN_FILTER, ORACLE_GL_LINEAR));
    CHECK(oracle_tex_parameter(
        cpu, stack_top, ORACLE_GL_TEXTURE_MAG_FILTER, ORACLE_GL_LINEAR));
    CHECK(oracle_tex_parameter(
        cpu, stack_top, ORACLE_GL_TEXTURE_WRAP_S, 0x0000812fu));
    CHECK(oracle_tex_parameter(
        cpu, stack_top, ORACLE_GL_TEXTURE_WRAP_T, 0x0000812fu));
    CHECK(s_native_tex_image_calls == image_before);
    CHECK(s_native_tex_sub_image_calls == sub_before);
    CHECK(oracle_fxray_log_matches("phase=upload") == 1u);
    CHECK(oracle_fxray_log_matches("phase=fallback") == 0u);

    /* Attachment after upload restores all RGBA rows before native observes
     * the FBO edge, and restores the previously bound object afterwards. */
    memset(s_fxray_rgba, 0x31, sizeof s_fxray_rgba);
    CHECK(oracle_bind_texture(cpu, stack_top, 202u));
    bind_before = s_native_bind_texture_calls;
    framebuffer_before = s_native_framebuffer_texture_calls;
    image_before = s_native_tex_image_calls;
    sub_before = s_native_tex_sub_image_calls;
    s_fxray_restore_rows = 0u;
    s_fxray_restore_mismatches = 0u;
    arguments[0] = ORACLE_GL_FRAMEBUFFER;
    arguments[1] = 0x00008ce0u;
    arguments[2] = ORACLE_GL_TEXTURE_2D;
    arguments[3] = 101u;
    arguments[4] = 0u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_FRAMEBUFFER_TEXTURE, arguments, 5u));
    CHECK(s_native_tex_image_calls == image_before + 1u);
    CHECK(s_native_tex_sub_image_calls == sub_before + ORACLE_FXRAY_HEIGHT);
    CHECK(s_fxray_restore_rows == ORACLE_FXRAY_HEIGHT);
    CHECK(s_fxray_restore_mismatches == 0u);
    CHECK(s_native_bind_texture_calls == bind_before + 2u);
    CHECK(s_native_bound_texture_2d == 202u);
    CHECK(s_native_framebuffer_texture_calls == framebuffer_before + 1u);
    CHECK(s_native_framebuffer_texture_name == 101u);
    CHECK(oracle_fxray_log_matches("phase=fallback") == 1u);
    CHECK(oracle_fxray_log_matches(
        "reason=framebuffer-attachment name=101 fail_closed=1") == 1u);

    /* Names ever used as render targets stay excluded even after another
     * exact-looking definition. */
    oracle_fxray_prepare_rgba();
    CHECK(oracle_bind_texture(cpu, stack_top, 101u));
    image_before = s_native_tex_image_calls;
    CHECK(oracle_tex_image(
        cpu, stack_top, 0, ORACLE_GL_RGBA,
        (int32_t)ORACLE_FXRAY_WIDTH, (int32_t)ORACLE_FXRAY_HEIGHT,
        0, ORACLE_GL_RGBA, ORACLE_GL_UNSIGNED_BYTE, s_fxray_rgba));
    CHECK(s_native_tex_image_calls == image_before + 1u);
    CHECK(s_native_tex_image_internal_format == (GLint)ORACLE_GL_RGBA);
    CHECK(s_native_tex_image_format == ORACLE_GL_RGBA);
    CHECK(s_native_tex_image_pixels == s_fxray_rgba);
    CHECK(s_fxray_alpha_uploads == 1u);

    /* A last-pixel RGB mismatch is a hostile full-scan reject and remains an
     * byte-identical native upload. */
    CHECK(oracle_bind_texture(cpu, stack_top, 102u));
    s_fxray_rgba[sizeof s_fxray_rgba - 2u] = 254u;
    CHECK(oracle_tex_image(
        cpu, stack_top, 0, ORACLE_GL_RGBA,
        (int32_t)ORACLE_FXRAY_WIDTH, (int32_t)ORACLE_FXRAY_HEIGHT,
        0, ORACLE_GL_RGBA, ORACLE_GL_UNSIGNED_BYTE, s_fxray_rgba));
    CHECK(s_native_tex_image_internal_format == (GLint)ORACLE_GL_RGBA);
    CHECK(s_native_tex_image_pixels == s_fxray_rgba);
    CHECK(s_fxray_alpha_uploads == 1u);

    /* Any sub-image mutation first reconstructs exact RGBA from the private
     * shadow, then forwards the original call and pointer unchanged. */
    oracle_fxray_prepare_rgba();
    CHECK(oracle_bind_texture(cpu, stack_top, 103u));
    CHECK(oracle_tex_image(
        cpu, stack_top, 0, ORACLE_GL_RGBA,
        (int32_t)ORACLE_FXRAY_WIDTH, (int32_t)ORACLE_FXRAY_HEIGHT,
        0, ORACLE_GL_RGBA, ORACLE_GL_UNSIGNED_BYTE, s_fxray_rgba));
    CHECK(s_fxray_alpha_uploads == 2u && s_fxray_alpha_mismatches == 0u);
    CHECK(s_fxray_u8_r111_uploads == 2u &&
          s_fxray_sample_mismatches == 0u);
    CHECK(oracle_fxray_log_matches("phase=upload") == 2u);
    CHECK(oracle_fxray_log_matches("hit=2 slot=0 name=103") == 1u);
    memset(s_fxray_rgba, 0x52, sizeof s_fxray_rgba);
    image_before = s_native_tex_image_calls;
    sub_before = s_native_tex_sub_image_calls;
    s_fxray_restore_rows = 0u;
    s_fxray_restore_mismatches = 0u;
    CHECK(oracle_tex_sub_image(cpu, stack_top, 1, 1, subpixel));
    CHECK(s_native_tex_image_calls == image_before + 1u);
    CHECK(s_native_tex_sub_image_calls ==
          sub_before + ORACLE_FXRAY_HEIGHT + 1u);
    CHECK(s_fxray_restore_rows == ORACLE_FXRAY_HEIGHT);
    CHECK(s_fxray_restore_mismatches == 0u);
    CHECK(s_native_tex_sub_image_width == 1);
    CHECK(s_native_tex_sub_image_height == 1);
    CHECK(s_native_tex_sub_image_format == ORACLE_GL_RGBA);
    CHECK(s_native_tex_sub_image_pixels == subpixel);
    CHECK(s_native_bound_texture_2d == 103u);

    /* Deletion drops only the named shadow.  The surviving second record is
     * still reconstructed if it is subsequently mutated. */
    oracle_fxray_prepare_rgba();
    CHECK(oracle_bind_texture(cpu, stack_top, 104u));
    CHECK(oracle_tex_image(
        cpu, stack_top, 0, ORACLE_GL_RGBA,
        (int32_t)ORACLE_FXRAY_WIDTH, (int32_t)ORACLE_FXRAY_HEIGHT,
        0, ORACLE_GL_RGBA, ORACLE_GL_UNSIGNED_BYTE, s_fxray_rgba));
    CHECK(oracle_bind_texture(cpu, stack_top, 105u));
    CHECK(oracle_tex_image(
        cpu, stack_top, 0, ORACLE_GL_RGBA,
        (int32_t)ORACLE_FXRAY_WIDTH, (int32_t)ORACLE_FXRAY_HEIGHT,
        0, ORACLE_GL_RGBA, ORACLE_GL_UNSIGNED_BYTE, s_fxray_rgba));
    deleted = 104u;
    arguments[0] = 1u;
    arguments[1] = (uint32_t)(uintptr_t)&deleted;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_DELETE_TEXTURES, arguments, 2u));
    CHECK(oracle_bind_texture(cpu, stack_top, 104u));
    sub_before = s_native_tex_sub_image_calls;
    CHECK(oracle_tex_sub_image(cpu, stack_top, 1, 1, subpixel));
    CHECK(s_native_tex_sub_image_calls == sub_before + 1u);
    CHECK(oracle_bind_texture(cpu, stack_top, 105u));
    sub_before = s_native_tex_sub_image_calls;
    s_fxray_restore_rows = 0u;
    s_fxray_restore_mismatches = 0u;
    CHECK(oracle_tex_sub_image(cpu, stack_top, 1, 1, subpixel));
    CHECK(s_native_tex_sub_image_calls ==
          sub_before + ORACLE_FXRAY_HEIGHT + 1u);
    CHECK(s_fxray_restore_rows == ORACLE_FXRAY_HEIGHT);
    CHECK(s_fxray_restore_mismatches == 0u);

    /* The only texture-state API in the 73-symbol surface is TexParameteri.
     * Frozen KAGE filter/wrap pnames are representation-invariant; any other
     * pname (including a future swizzle exposure) restores RGBA first. */
    oracle_fxray_prepare_rgba();
    CHECK(oracle_bind_texture(cpu, stack_top, 106u));
    CHECK(oracle_tex_image(
        cpu, stack_top, 0, ORACLE_GL_RGBA,
        (int32_t)ORACLE_FXRAY_WIDTH, (int32_t)ORACLE_FXRAY_HEIGHT,
        0, ORACLE_GL_RGBA, ORACLE_GL_UNSIGNED_BYTE, s_fxray_rgba));
    image_before = s_native_tex_image_calls;
    sub_before = s_native_tex_sub_image_calls;
    s_fxray_restore_rows = 0u;
    s_fxray_restore_mismatches = 0u;
    arguments[0] = ORACLE_GL_TEXTURE_2D;
    arguments[1] = ORACLE_GL_TEXTURE_SWIZZLE_R;
    arguments[2] = 0x00001903u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_TEX_PARAMETER, arguments, 3u));
    CHECK(s_native_tex_image_calls == image_before + 1u);
    CHECK(s_native_tex_sub_image_calls == sub_before + ORACLE_FXRAY_HEIGHT);
    CHECK(s_fxray_restore_rows == ORACLE_FXRAY_HEIGHT);
    CHECK(s_fxray_restore_mismatches == 0u);

    /* A normal filter pName leaves the optimized object live, while teardown
     * still cannot strand alpha storage after its CPU shadow is gone. */
    oracle_fxray_prepare_rgba();
    CHECK(oracle_bind_texture(cpu, stack_top, 107u));
    CHECK(oracle_tex_image(
        cpu, stack_top, 0, ORACLE_GL_RGBA,
        (int32_t)ORACLE_FXRAY_WIDTH, (int32_t)ORACLE_FXRAY_HEIGHT,
        0, ORACLE_GL_RGBA, ORACLE_GL_UNSIGNED_BYTE, s_fxray_rgba));
    image_before = s_native_tex_image_calls;
    sub_before = s_native_tex_sub_image_calls;
    arguments[0] = ORACLE_GL_TEXTURE_2D;
    arguments[1] = ORACLE_GL_TEXTURE_MIN_FILTER;
    arguments[2] = ORACLE_GL_LINEAR;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_TEX_PARAMETER, arguments, 3u));
    CHECK(s_native_tex_image_calls == image_before);
    CHECK(s_native_tex_sub_image_calls == sub_before);
    sub_before = s_native_tex_sub_image_calls;
    s_fxray_restore_rows = 0u;
    s_fxray_restore_mismatches = 0u;
    gl_vita_backend_uninstall();
    CHECK(s_native_tex_sub_image_calls == sub_before + ORACLE_FXRAY_HEIGHT);
    CHECK(s_fxray_restore_rows == ORACLE_FXRAY_HEIGHT);
    CHECK(s_fxray_restore_mismatches == 0u);
    CHECK(gl_vita_backend_install());
    CHECK(oracle_fxray_log_matches("phase=upload") == 2u);
    CHECK(oracle_fxray_log_matches("phase=fallback") == 1u);
    CHECK(s_fxray_log_calls == 3u);
    return 0;
}
#else
static int oracle_test_fxray_off_passthrough(CPU *cpu, uint32_t stack_top)
{
    unsigned get_before;
    unsigned image_before;

    CHECK(oracle_bind_texture(cpu, stack_top, 707u));
    get_before = s_native_get_integer_calls;
    image_before = s_native_tex_image_calls;
    /* Pointer 1 is intentionally hostile.  OFF must never inspect it before
     * the fake native GL owner receives the exact original request. */
    CHECK(oracle_tex_image(
        cpu, stack_top, 0, ORACLE_GL_RGBA, 256, 512, 0,
        ORACLE_GL_RGBA, ORACLE_GL_UNSIGNED_BYTE,
        (const void *)(uintptr_t)1u));
    CHECK(s_native_get_integer_calls == get_before);
    CHECK(s_native_tex_image_calls == image_before + 1u);
    CHECK(s_native_tex_image_internal_format == (GLint)ORACLE_GL_RGBA);
    CHECK(s_native_tex_image_format == ORACLE_GL_RGBA);
    CHECK((uintptr_t)s_native_tex_image_pixels == 1u);
    return 0;
}
#endif

#if defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO)
/* ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO (coalesce-memo mode): the
 * generation word the replay memo is keyed on is bumped by exactly the
 * mutators that invalidate the shim location cache, by every backend
 * install/uninstall (never rewound), and by nothing else on the draw path;
 * VERIFY routes a memo/wrapper disagreement into ph120.a loc(...,m). */
/* Registry tokens of gl_surface_generated.inc not used elsewhere here. */
#define ORACLE_TOKEN_GET_ATTRIB 0x7e307ce3u
#define ORACLE_TOKEN_ATTACH_SHADER 0x7ec5ca07u
#define ORACLE_TOKEN_COMPILE_SHADER 0x7edbdc55u
#define ORACLE_TOKEN_CREATE_SHADER 0x7e8e2dffu
#define ORACLE_TOKEN_DELETE_SHADER 0x7e226ffeu
#define ORACLE_TOKEN_SHADER_SOURCE 0x7ea2cbd7u

static int oracle_test_location_memo_generation(CPU *cpu, uint32_t stack_top)
{
    static const char attrib_name[] = "aPosition";
    static const char source_text[] = "void main() {}";
    const char *sources[1] = { source_text };
    uint32_t arguments[4];
    uint32_t generation;
    uint32_t program, shader;

    gl_vita_backend_uninstall();
    CHECK(gl_vita_backend_install());
    generation = g_isaac_vita_gl_location_generation;
    CHECK(generation != 0u);

    /* Pure queries and the draw-path program bind leave the word alone. */
    arguments[0] = 7u;
    arguments[1] = (uint32_t)(uintptr_t)attrib_name;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_GET_ATTRIB, arguments, 2u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_GET_UNIFORM, arguments, 2u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_USE_PROGRAM, arguments, 1u));
    CHECK(g_isaac_vita_gl_location_generation == generation);

    /* Every program/shader-state mutator bumps it exactly once, before
     * the native call (the wrapper order is pinned by the source). */
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_CREATE_PROGRAM, arguments, 0u));
    program = cpu->eax;
    CHECK(g_isaac_vita_gl_location_generation == ++generation);
    arguments[0] = 0x8b31u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_CREATE_SHADER, arguments, 1u));
    shader = cpu->eax;
    CHECK(g_isaac_vita_gl_location_generation == generation);
    arguments[0] = shader;
    arguments[1] = 1u;
    arguments[2] = (uint32_t)(uintptr_t)sources;
    arguments[3] = 0u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_SHADER_SOURCE, arguments, 4u));
    CHECK(g_isaac_vita_gl_location_generation == ++generation);
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_COMPILE_SHADER, arguments, 1u));
    CHECK(g_isaac_vita_gl_location_generation == ++generation);
    arguments[0] = program;
    arguments[1] = shader;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_ATTACH_SHADER, arguments, 2u));
    CHECK(g_isaac_vita_gl_location_generation == ++generation);
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_LINK_PROGRAM, arguments, 1u));
    CHECK(g_isaac_vita_gl_location_generation == ++generation);
    arguments[0] = shader;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_DELETE_SHADER, arguments, 1u));
    CHECK(g_isaac_vita_gl_location_generation == ++generation);
    arguments[0] = program;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_DELETE_PROGRAM, arguments, 1u));
    CHECK(g_isaac_vita_gl_location_generation == ++generation);
    /* Name recycling: delete + create is two bumps. */
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_CREATE_PROGRAM, arguments, 0u));
    CHECK(g_isaac_vita_gl_location_generation == ++generation);
    arguments[0] = cpu->eax;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_DELETE_PROGRAM, arguments, 1u));
    CHECK(g_isaac_vita_gl_location_generation == ++generation);

    /* Install/uninstall reset the shim cache; the memo word only moves
     * forward, so an entry filled before either is stale after. */
    gl_vita_backend_uninstall();
    CHECK(g_isaac_vita_gl_location_generation > generation);
    generation = g_isaac_vita_gl_location_generation;
    CHECK(gl_vita_backend_install());
    CHECK(g_isaac_vita_gl_location_generation > generation);

#if defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_VERIFY) && \
    defined(ISAAC_VITA_PHASE_PROFILE)
    {
        uint32_t before =
            g_isaac_vita_gl_phase_profile_counters.location_cache_mismatch;
        isaac_vita_gl_location_memo_note_mismatch();
        CHECK(g_isaac_vita_gl_phase_profile_counters.location_cache_mismatch ==
              before + 1u);
    }
#endif
    return 0;
}
#endif

#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
static void oracle_redundancy_reset_native(void)
{
    s_native_next_program = 7u;
    s_native_link_status = 0;
    s_native_uniform_location = 0x1234;
    s_native_create_program_calls = 0u;
    s_native_delete_program_calls = 0u;
    s_native_get_program_calls = 0u;
    s_native_get_uniform_calls = 0u;
    s_native_link_program_calls = 0u;
    s_native_use_program_calls = 0u;
    s_native_uniform1i_calls = 0u;
    s_native_uniform4fv_calls = 0u;
    s_native_uniform_matrix4fv_calls = 0u;
    s_native_last_program = 0u;
    s_native_current_program = 0u;
    s_native_reject_use_program = 0;
    s_native_last_uniform_location = 0;
    s_native_last_uniform1i = 0;
    s_native_last_uniform_count = 0;
    s_native_last_uniform_transpose = 0u;
    memset(s_native_last_uniform_payload, 0,
           sizeof s_native_last_uniform_payload);
#if defined(ISAAC_VITA_PHASE_PROFILE)
    memset(&g_isaac_vita_gl_phase_profile_counters, 0,
           sizeof g_isaac_vita_gl_phase_profile_counters);
#endif
}

static int oracle_test_gl_redundancy(CPU *cpu, uint32_t stack_top)
{
    static const char uniform_name[] = "u_oracle";
    uint32_t arguments[4];
    GLint link_status = -1;
    GLfloat vector_a[8] = {
        1.0f, -0.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f
    };
    GLfloat vector_b[4];
    GLfloat matrix[16];
    unsigned before;
    unsigned index;

    gl_vita_backend_uninstall();
    CHECK(gl_vita_backend_install());
    oracle_redundancy_reset_native();

    /* Unknown programs remain exact native passthroughs on every call. */
    arguments[0] = 99u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_USE_PROGRAM, arguments, 1u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_USE_PROGRAM, arguments, 1u));
    CHECK(s_native_use_program_calls == 2u);

    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_CREATE_PROGRAM, NULL, 0u));
    CHECK(cpu->eax == 7u && s_native_create_program_calls == 1u);
    arguments[0] = 7u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_LINK_PROGRAM, arguments, 1u));
    CHECK(s_native_link_program_calls == 1u);

    /* Merely calling LinkProgram is insufficient: both false and unobserved
     * link status keep repeated binds on the native path. */
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_USE_PROGRAM, arguments, 1u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_USE_PROGRAM, arguments, 1u));
    before = s_native_use_program_calls;

    /* A returned native query does not prove its guest output was written.
     * The tracker learns only from its own second query into a local word. */
    arguments[0] = 7u;
    arguments[1] = ORACLE_GL_LINK_STATUS;
    arguments[2] = 2u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_GET_PROGRAM, arguments, 3u));
    CHECK(s_native_get_program_calls == 2u);

    arguments[0] = 7u;
    arguments[1] = ORACLE_GL_LINK_STATUS;
    arguments[2] = (uint32_t)(uintptr_t)&link_status;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_GET_PROGRAM, arguments, 3u));
    CHECK(link_status == 0 && s_native_get_program_calls == 4u);
    arguments[0] = 7u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_USE_PROGRAM, arguments, 1u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_USE_PROGRAM, arguments, 1u));
    CHECK(s_native_use_program_calls == before + 2u);

    s_native_link_status = 1;
    arguments[0] = 7u;
    arguments[1] = ORACLE_GL_LINK_STATUS;
    arguments[2] = (uint32_t)(uintptr_t)&link_status;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_GET_PROGRAM, arguments, 3u));
    CHECK(link_status == 1 && s_native_get_program_calls == 6u);

    /* Even a linked tracked object is not cached until the driver's current
     * program query proves that this particular bind succeeded. */
    s_native_current_program = 0u;
    s_native_reject_use_program = 1;
    before = s_native_use_program_calls;
    arguments[0] = 7u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_USE_PROGRAM, arguments, 1u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_USE_PROGRAM, arguments, 1u));
    CHECK(s_native_use_program_calls == before + 2u);

    s_native_reject_use_program = 0;
    before = s_native_use_program_calls;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_USE_PROGRAM, arguments, 1u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_USE_PROGRAM, arguments, 1u));
    CHECK(s_native_use_program_calls == before + 1u);

    /* Program zero is cached only after one forwarded bind; switching back to
     * a proven linked program also forwards exactly once. */
    before = s_native_use_program_calls;
    arguments[0] = 0u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_USE_PROGRAM, arguments, 1u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_USE_PROGRAM, arguments, 1u));
    CHECK(s_native_use_program_calls == before + 1u);
    before = s_native_use_program_calls;
    arguments[0] = 7u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_USE_PROGRAM, arguments, 1u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_USE_PROGRAM, arguments, 1u));
    CHECK(s_native_use_program_calls == before + 1u);

    arguments[0] = 7u;
    arguments[1] = (uint32_t)(uintptr_t)uniform_name;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_GET_UNIFORM, arguments, 2u));
    CHECK(cpu->eax == 0x1234u && s_native_get_uniform_calls == 1u);

    /* Scalar values use their exact bytes. */
    before = s_native_uniform1i_calls;
    arguments[0] = 0x1234u;
    arguments[1] = 42u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM1I, arguments, 2u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM1I, arguments, 2u));
    CHECK(s_native_uniform1i_calls == before + 1u);
    arguments[1] = 43u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM1I, arguments, 2u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM1I, arguments, 2u));
    CHECK(s_native_uniform1i_calls == before + 2u &&
          s_native_last_uniform1i == 43);

    /* -1 remains a native no-op request rather than becoming a hidden cache
     * hit, even with a pointer which must never be inspected. */
    before = s_native_uniform4fv_calls;
    arguments[0] = UINT32_MAX;
    arguments[1] = 1u;
    arguments[2] = 1u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM4FV, arguments, 3u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM4FV, arguments, 3u));
    CHECK(s_native_uniform4fv_calls == before + 2u);

    /* Native is allowed to return without reading a pointer.  That still must
     * not cause either a pre-call comparison or post-call snapshot read. */
    before = s_native_uniform4fv_calls;
    arguments[0] = 0x1234u;
    arguments[1] = 1u;
    arguments[2] = 2u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM4FV, arguments, 3u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM4FV, arguments, 3u));
    CHECK(s_native_uniform4fv_calls == before + 2u);

    /* Pointer-form uniforms are always exact native calls, including repeats,
     * byte changes and address changes.  The bridge never dereferences them. */
    memcpy(vector_b, vector_a, sizeof vector_b);
    before = s_native_uniform4fv_calls;
    arguments[0] = 0x1234u;
    arguments[1] = 1u;
    arguments[2] = (uint32_t)(uintptr_t)vector_a;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM4FV, arguments, 3u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM4FV, arguments, 3u));
    CHECK(s_native_uniform4fv_calls == before + 2u);
    vector_a[3] = -4.0f;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM4FV, arguments, 3u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM4FV, arguments, 3u));
    CHECK(s_native_uniform4fv_calls == before + 4u);
    memcpy(vector_b, vector_a, sizeof vector_b);
    arguments[2] = (uint32_t)(uintptr_t)vector_b;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM4FV, arguments, 3u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM4FV, arguments, 3u));
    CHECK(s_native_uniform4fv_calls == before + 6u);

    /* A pointer write and count>1 both invalidate the scalar shadow because
     * array locations may overlap it. */
    before = s_native_uniform1i_calls;
    arguments[0] = 0x1234u;
    arguments[1] = 43u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM1I, arguments, 2u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM1I, arguments, 2u));
    CHECK(s_native_uniform1i_calls == before + 1u);
    before = s_native_uniform4fv_calls;
    arguments[0] = 0x1234u;
    arguments[1] = 1u;
    arguments[2] = (uint32_t)(uintptr_t)vector_b;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM4FV, arguments, 3u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM4FV, arguments, 3u));
    CHECK(s_native_uniform4fv_calls == before + 2u);
    arguments[1] = 2u;
    arguments[2] = (uint32_t)(uintptr_t)vector_a;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM4FV, arguments, 3u));
    CHECK(s_native_uniform4fv_calls == before + 3u &&
          s_native_last_uniform_count == 2);
    before = s_native_uniform1i_calls;
    arguments[0] = 0x1234u;
    arguments[1] = 43u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM1I, arguments, 2u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM1I, arguments, 2u));
    CHECK(s_native_uniform1i_calls == before + 1u);

    /* Matrix pointers and either transpose value remain native-only.  The
     * matrix uniform sits at a new location.  Under the location-cache
     * contract (gl_vita_backend.c, ISAAC_VITA_GL_LOCATION_CACHE) a
     * glGetUniformLocation answer changes only after a program mutator:
     * pinned vitaGL rebuilds the uniform lists in glLinkProgram.  So relink
     * program 7 before the fake starts answering 0x2345, as the guest would
     * have to; a bare fake answer change behind an unchanged program is not
     * something vitaGL can do, and the cache would (correctly) keep serving
     * the memoized 0x1234.  Querying a different name would also miss the
     * memo but would not exercise the invalidation the contract relies on.
     * The relink un-proves the program for the redundancy tracker; re-prove
     * it as the guest does (status query, one forwarded bind) so the later
     * "Relink invalidates ..." paragraph still starts from a proven program
     * and the suppression totals below are unchanged. */
    for (index = 0u; index < 16u; ++index)
        matrix[index] = (GLfloat)index + 0.25f;
    arguments[0] = 7u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_LINK_PROGRAM, arguments, 1u));
    arguments[0] = 7u;
    arguments[1] = ORACLE_GL_LINK_STATUS;
    arguments[2] = (uint32_t)(uintptr_t)&link_status;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_GET_PROGRAM, arguments, 3u));
    CHECK(link_status == 1);
    before = s_native_use_program_calls;
    arguments[0] = 7u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_USE_PROGRAM, arguments, 1u));
    CHECK(s_native_use_program_calls == before + 1u);
    s_native_uniform_location = 0x2345;
    before = s_native_get_uniform_calls;
    arguments[0] = 7u;
    arguments[1] = (uint32_t)(uintptr_t)uniform_name;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_GET_UNIFORM, arguments, 2u));
    /* Every variant reaches vitaGL here: the relink bumped the memo
     * generation (ISAAC_VITA_GL_LOCATION_CACHE) or there is no memo. */
    CHECK(cpu->eax == 0x2345u &&
          s_native_get_uniform_calls == before + 1u);
    before = s_native_uniform_matrix4fv_calls;
    arguments[0] = 0x2345u;
    arguments[1] = 1u;
    arguments[2] = 0u;
    arguments[3] = (uint32_t)(uintptr_t)matrix;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM_MATRIX4FV, arguments, 4u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM_MATRIX4FV, arguments, 4u));
    arguments[2] = 1u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM_MATRIX4FV, arguments, 4u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM_MATRIX4FV, arguments, 4u));
    CHECK(s_native_uniform_matrix4fv_calls == before + 4u &&
          s_native_last_uniform_transpose == 1u);

    /* A changed hostile pointer must reach native GL without an eager bridge
     * read.  The controlled native fault leaves dispatch reusable. */
    before = s_native_uniform4fv_calls;
    arguments[0] = 0x1234u;
    arguments[1] = 1u;
    arguments[2] = 1u;
    CHECK(oracle_expect_fault(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM4FV, arguments, 3u,
        ORACLE_TOKEN_UNIFORM4FV,
        "oracle native glUniform4fv received hostile pointer"));
    CHECK(s_native_uniform4fv_calls == before + 1u);
    arguments[0] = 0x7777u;
    CHECK(oracle_expect_fault(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM4FV, arguments, 3u,
        ORACLE_TOKEN_UNIFORM4FV,
        "oracle native glUniform4fv received hostile pointer"));
    CHECK(s_native_uniform4fv_calls == before + 2u);

    /* Relink invalidates current-program, locations and payloads until a new
     * successful status query and location query are observed. */
    arguments[0] = 7u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_LINK_PROGRAM, arguments, 1u));
    before = s_native_use_program_calls;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_USE_PROGRAM, arguments, 1u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_USE_PROGRAM, arguments, 1u));
    CHECK(s_native_use_program_calls == before + 2u);
    before = s_native_uniform4fv_calls;
    arguments[0] = 0x1234u;
    arguments[1] = 1u;
    arguments[2] = (uint32_t)(uintptr_t)vector_b;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM4FV, arguments, 3u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM4FV, arguments, 3u));
    CHECK(s_native_uniform4fv_calls == before + 2u);

    arguments[0] = 7u;
    arguments[1] = ORACLE_GL_LINK_STATUS;
    arguments[2] = (uint32_t)(uintptr_t)&link_status;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_GET_PROGRAM, arguments, 3u));
    arguments[0] = 7u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_USE_PROGRAM, arguments, 1u));
    s_native_uniform_location = 0x1234;
    arguments[0] = 7u;
    arguments[1] = (uint32_t)(uintptr_t)uniform_name;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_GET_UNIFORM, arguments, 2u));
    before = s_native_uniform4fv_calls;
    arguments[0] = 0x1234u;
    arguments[1] = 1u;
    arguments[2] = (uint32_t)(uintptr_t)vector_b;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM4FV, arguments, 3u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM4FV, arguments, 3u));
    CHECK(s_native_uniform4fv_calls == before + 2u);

    /* Delete and install/uninstall both make stale identities fail open. */
    arguments[0] = 7u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_DELETE_PROGRAM, arguments, 1u));
    CHECK(s_native_delete_program_calls == 1u);
    before = s_native_use_program_calls;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_USE_PROGRAM, arguments, 1u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_USE_PROGRAM, arguments, 1u));
    CHECK(s_native_use_program_calls == before + 2u);

    gl_vita_backend_uninstall();
    CHECK(gl_vita_backend_install());
    before = s_native_use_program_calls;
    arguments[0] = 0u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_USE_PROGRAM, arguments, 1u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_USE_PROGRAM, arguments, 1u));
    CHECK(s_native_use_program_calls == before + 1u);

#if defined(ISAAC_VITA_PHASE_PROFILE)
    CHECK(g_isaac_vita_gl_phase_profile_counters.use_program ==
          s_native_use_program_calls);
    CHECK(g_isaac_vita_gl_phase_profile_counters.uniform ==
          s_native_uniform1i_calls + s_native_uniform4fv_calls +
          s_native_uniform_matrix4fv_calls);
    CHECK(g_isaac_vita_gl_phase_profile_counters.use_program_suppressed == 4u);
    CHECK(g_isaac_vita_gl_phase_profile_counters.uniform_suppressed == 4u);
#endif
    return 0;
}
#endif

#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
static void oracle_typed_state_reset_native(void)
{
    s_native_active_texture_calls = 0u;
    s_native_active_texture = 0u;
    s_native_bind_texture_calls = 0u;
    s_native_bind_texture_target = 0u;
    s_native_bind_texture_name = 0u;
    s_native_blend_calls = 0u;
    memset(s_native_blend, 0, sizeof s_native_blend);
    s_native_delete_texture_calls = 0u;
    s_native_depth_calls = 0u;
    s_native_depth = 0u;
    s_native_enable_attrib_calls = 0u;
    s_native_disable_attrib_calls = 0u;
    s_native_attrib_index = 0u;
    s_native_attrib_pointer_calls = 0u;
    s_native_attrib_size = 0;
    s_native_attrib_type = 0u;
    s_native_attrib_normalized = 0u;
    s_native_attrib_stride = 0;
    s_native_attrib_pointer = NULL;
    s_native_viewport_calls = 0u;
#if defined(ISAAC_VITA_PHASE_PROFILE)
    memset(&g_isaac_vita_gl_phase_profile_counters, 0,
           sizeof g_isaac_vita_gl_phase_profile_counters);
#endif
}

static int oracle_test_gl_typed_state(CPU *cpu, uint32_t stack_top)
{
    uint32_t arguments[6];
    GLuint deleted_texture = 8u;

    gl_vita_backend_uninstall();
    CHECK(gl_vita_backend_install());
    oracle_typed_state_reset_native();

    /* Active texture: exact valid duplicates stop; invalid enums retain the
     * native path and make the following texture-unit identity unknown. */
    arguments[0] = ORACLE_GL_TEXTURE0;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_ACTIVE_TEXTURE, arguments, 1u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_ACTIVE_TEXTURE, arguments, 1u));
    arguments[0] = ORACLE_GL_TEXTURE0 + 1u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_ACTIVE_TEXTURE, arguments, 1u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_ACTIVE_TEXTURE, arguments, 1u));
    arguments[0] = ORACLE_GL_TEXTURE0 + 16u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_ACTIVE_TEXTURE, arguments, 1u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_ACTIVE_TEXTURE, arguments, 1u));
    CHECK(s_native_active_texture_calls == 4u);

    /* Unknown active-unit calls and unmodelled targets pass through. */
    arguments[0] = ORACLE_GL_TEXTURE_2D;
    arguments[1] = 7u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BIND_TEXTURE, arguments, 2u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BIND_TEXTURE, arguments, 2u));
    arguments[0] = ORACLE_GL_TEXTURE0;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_ACTIVE_TEXTURE, arguments, 1u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_ACTIVE_TEXTURE, arguments, 1u));

    arguments[0] = ORACLE_GL_TEXTURE_2D;
    arguments[1] = 7u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BIND_TEXTURE, arguments, 2u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BIND_TEXTURE, arguments, 2u));
    arguments[1] = 8u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BIND_TEXTURE, arguments, 2u));
    arguments[0] = ORACLE_GL_TEXTURE0 + 1u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_ACTIVE_TEXTURE, arguments, 1u));
    arguments[0] = ORACLE_GL_TEXTURE_2D;
    arguments[1] = 8u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BIND_TEXTURE, arguments, 2u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BIND_TEXTURE, arguments, 2u));
    arguments[0] = ORACLE_GL_TEXTURE0;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_ACTIVE_TEXTURE, arguments, 1u));
    arguments[0] = ORACLE_GL_TEXTURE_2D;
    arguments[1] = 8u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BIND_TEXTURE, arguments, 2u));

    arguments[0] = ORACLE_GL_TEXTURE_CUBE_MAP;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BIND_TEXTURE, arguments, 2u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BIND_TEXTURE, arguments, 2u));
    arguments[0] = ORACLE_GL_TEXTURE_2D;
    arguments[1] = 16384u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BIND_TEXTURE, arguments, 2u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BIND_TEXTURE, arguments, 2u));
    arguments[1] = 8u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BIND_TEXTURE, arguments, 2u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BIND_TEXTURE, arguments, 2u));

    /* Deletion invalidates bindings without an extra bridge-side array read. */
    arguments[0] = 1u;
    arguments[1] = (uint32_t)(uintptr_t)&deleted_texture;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_DELETE_TEXTURES, arguments, 2u));
    arguments[0] = ORACLE_GL_TEXTURE_2D;
    arguments[1] = 8u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BIND_TEXTURE, arguments, 2u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BIND_TEXTURE, arguments, 2u));
    CHECK(s_native_bind_texture_calls == 11u);
    CHECK(s_native_delete_texture_calls == 1u);

    /* Blend/depth invalid tuples always pass through and poison their exact
     * shadows, so a later formerly-known valid tuple forwards again. */
    arguments[0] = ORACLE_GL_SRC_ALPHA;
    arguments[1] = ORACLE_GL_ONE_MINUS_SRC_ALPHA;
    arguments[2] = ORACLE_GL_ONE;
    arguments[3] = ORACLE_GL_ONE_MINUS_SRC_ALPHA;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BLEND_SEPARATE, arguments, 4u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BLEND_SEPARATE, arguments, 4u));
    arguments[0] = ORACLE_GL_ONE;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BLEND_SEPARATE, arguments, 4u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BLEND_SEPARATE, arguments, 4u));
    arguments[0] = 0xdeadu;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BLEND_SEPARATE, arguments, 4u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BLEND_SEPARATE, arguments, 4u));
    arguments[0] = ORACLE_GL_ONE;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BLEND_SEPARATE, arguments, 4u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BLEND_SEPARATE, arguments, 4u));
    CHECK(s_native_blend_calls == 5u);

    arguments[0] = ORACLE_GL_LESS;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_DEPTH_FUNC, arguments, 1u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_DEPTH_FUNC, arguments, 1u));
    arguments[0] = ORACLE_GL_LEQUAL;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_DEPTH_FUNC, arguments, 1u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_DEPTH_FUNC, arguments, 1u));
    arguments[0] = 0xdeadu;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_DEPTH_FUNC, arguments, 1u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_DEPTH_FUNC, arguments, 1u));
    arguments[0] = ORACLE_GL_LEQUAL;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_DEPTH_FUNC, arguments, 1u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_DEPTH_FUNC, arguments, 1u));
    CHECK(s_native_depth_calls == 5u);

    /* FBO transitions invalidate the logical viewport shadow.  Negative
     * dimensions stay exact native calls and likewise poison it. */
    CHECK(oracle_set_viewport(cpu, stack_top, 10, 20, 30, 40));
    CHECK(oracle_set_viewport(cpu, stack_top, 10, 20, 30, 40));
    CHECK(oracle_bind_framebuffer(
        cpu, stack_top, ORACLE_GL_READ_FRAMEBUFFER, 44u));
    CHECK(oracle_set_viewport(cpu, stack_top, 10, 20, 30, 40));
    CHECK(oracle_set_viewport(cpu, stack_top, 10, 20, 30, 40));
    CHECK(oracle_set_viewport(cpu, stack_top, 10, 20, -1, 40));
    CHECK(oracle_set_viewport(cpu, stack_top, 10, 20, -1, 40));
    CHECK(oracle_set_viewport(cpu, stack_top, 10, 20, 30, 40));
    CHECK(oracle_set_viewport(cpu, stack_top, 10, 20, 30, 40));
    CHECK(s_native_viewport_calls == 5u);

    /* Enable/disable and pointer tuples are per valid attribute.  Pointer 1
     * is deliberately hostile: equality is integer-only and never reads it. */
    arguments[0] = 2u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_ENABLE_ATTRIB, arguments, 1u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_ENABLE_ATTRIB, arguments, 1u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_DISABLE_ATTRIB, arguments, 1u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_DISABLE_ATTRIB, arguments, 1u));
    arguments[0] = 16u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_ENABLE_ATTRIB, arguments, 1u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_ENABLE_ATTRIB, arguments, 1u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_DISABLE_ATTRIB, arguments, 1u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_DISABLE_ATTRIB, arguments, 1u));
    arguments[0] = 2u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_DISABLE_ATTRIB, arguments, 1u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_DISABLE_ATTRIB, arguments, 1u));
    CHECK(s_native_enable_attrib_calls == 3u);
    CHECK(s_native_disable_attrib_calls == 4u);

#define ORACLE_ATTRIB_CALL() \
    CHECK(oracle_dispatch( \
        cpu, stack_top, ORACLE_TOKEN_ATTRIB_POINTER, arguments, 6u))
    arguments[0] = 3u;
    arguments[1] = 2u;
    arguments[2] = ORACLE_GL_FLOAT;
    arguments[3] = 0u;
    arguments[4] = 16u;
    arguments[5] = 1u;
    ORACLE_ATTRIB_CALL();
    ORACLE_ATTRIB_CALL();
    arguments[3] = 1u;
    ORACLE_ATTRIB_CALL();
    ORACLE_ATTRIB_CALL();
    arguments[1] = 0u;
    ORACLE_ATTRIB_CALL();
    ORACLE_ATTRIB_CALL();
    arguments[1] = 2u;
    ORACLE_ATTRIB_CALL();
    ORACLE_ATTRIB_CALL();
    arguments[2] = 0xdeadu;
    ORACLE_ATTRIB_CALL();
    ORACLE_ATTRIB_CALL();
    arguments[2] = ORACLE_GL_FLOAT;
    arguments[4] = UINT32_MAX;
    ORACLE_ATTRIB_CALL();
    ORACLE_ATTRIB_CALL();
    arguments[4] = 16u;
    arguments[0] = 16u;
    ORACLE_ATTRIB_CALL();
    ORACLE_ATTRIB_CALL();
#undef ORACLE_ATTRIB_CALL
    CHECK(s_native_attrib_pointer_calls == 11u);
    CHECK((uintptr_t)s_native_attrib_pointer == 1u);

    /* Full lifecycle reset: formerly exact texture state must forward once. */
    gl_vita_backend_uninstall();
    CHECK(gl_vita_backend_install());
    arguments[0] = ORACLE_GL_TEXTURE0;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_ACTIVE_TEXTURE, arguments, 1u));
    arguments[0] = ORACLE_GL_TEXTURE_2D;
    arguments[1] = 8u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BIND_TEXTURE, arguments, 2u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BIND_TEXTURE, arguments, 2u));
    CHECK(s_native_active_texture_calls == 8u);
    CHECK(s_native_bind_texture_calls == 12u);

#if defined(ISAAC_VITA_PHASE_PROFILE)
    CHECK(g_isaac_vita_gl_phase_profile_counters
              .typed_state_hit_active_texture == 3u);
    CHECK(g_isaac_vita_gl_phase_profile_counters
              .typed_state_hit_bind_texture == 6u);
    CHECK(g_isaac_vita_gl_phase_profile_counters
              .typed_state_hit_blend == 3u);
    CHECK(g_isaac_vita_gl_phase_profile_counters
              .typed_state_hit_depth == 3u);
    CHECK(g_isaac_vita_gl_phase_profile_counters
              .typed_state_hit_viewport == 3u);
    CHECK(g_isaac_vita_gl_phase_profile_counters
              .typed_state_hit_attrib_toggle == 3u);
    CHECK(g_isaac_vita_gl_phase_profile_counters
              .typed_state_hit_attrib_pointer == 3u);
    CHECK(g_isaac_vita_gl_phase_profile_counters.typed_state_miss == 27u);
    CHECK(g_isaac_vita_gl_phase_profile_counters
              .typed_state_reject_invalid == 22u);
    CHECK(g_isaac_vita_gl_phase_profile_counters
              .typed_state_reject_unknown == 4u);
    CHECK(g_isaac_vita_gl_phase_profile_counters.bind_texture == 12u);
    CHECK(g_isaac_vita_gl_phase_profile_counters.state == 15u);
    CHECK(g_isaac_vita_gl_phase_profile_counters.attrib_toggle == 7u);
    CHECK(g_isaac_vita_gl_phase_profile_counters.attrib_pointer == 11u);
#endif
    return 0;
}
#endif

#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
static int oracle_test_texture_churn(CPU *cpu, uint32_t stack_top)
{
    IsaacVitaTextureChurnProfile profile;
    uint32_t arguments[9];
    GLuint deleted[2];
    GLuint texture = 0u;


    gl_vita_backend_uninstall();
    CHECK(gl_vita_backend_install());
    s_native_gen_texture_calls = 0u;
    s_texture_profile_clock = 0u;
    s_native_delete_texture_clock_cost = 11u;

    /* A legal zero-name call still crosses native GL and both clocked seams.
     * Keep it in a clean window so the exact 4-clocks-per-call invariant is
     * checked before later deliberately ambiguous lifecycle events set bad. */
    arguments[0] = 0u;
    arguments[1] = 0u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_DELETE_TEXTURES, arguments, 2u));
    gl_vita_backend_texture_churn_profile_take_window(&profile);
    CHECK(profile.delete_calls == 1u && profile.delete_names == 0u);
    CHECK(profile.delete_native_observed_us == 18u &&
          profile.delete_native_max_observed_us == 18u);
    CHECK(profile.delete_post_observed_us == 7u &&
          profile.delete_post_max_observed_us == 7u);
    CHECK(profile.clock_calls == 4u && profile.bad == 0u);

    /* Fresh RED texture: the n=1 output name is also the pinned linear
     * allocator's exact scan count. */
    s_native_gen_texture_write = 1;
    s_native_gen_texture_value = 1u;
    arguments[0] = 1u;
    arguments[1] = (uint32_t)(uintptr_t)&texture;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_GEN_TEXTURES, arguments, 2u));
    CHECK(texture == 1u);
    arguments[0] = ORACLE_GL_TEXTURE0;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_ACTIVE_TEXTURE, arguments, 1u));
    arguments[0] = ORACLE_GL_TEXTURE_2D;
    arguments[1] = texture;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BIND_TEXTURE, arguments, 2u));
    arguments[0] = ORACLE_GL_TEXTURE_2D;
    arguments[1] = 0u;
    arguments[2] = ORACLE_GL_RED;
    arguments[3] = 10u;
    arguments[4] = 2u;
    arguments[5] = 0u;
    arguments[6] = ORACLE_GL_RED;
    arguments[7] = ORACLE_GL_UNSIGNED_BYTE;
    arguments[8] = 0u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_TEX_IMAGE, arguments, 9u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_TEX_IMAGE, arguments, 9u));

    /* A second fresh name and the only frozen converted upload: 256x1
     * ABGR_EXT palette data into RGBA storage. */
    texture = 0u;
    s_native_gen_texture_value = 4u;
    arguments[0] = 1u;
    arguments[1] = (uint32_t)(uintptr_t)&texture;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_GEN_TEXTURES, arguments, 2u));
    arguments[0] = ORACLE_GL_TEXTURE_2D;
    arguments[1] = texture;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BIND_TEXTURE, arguments, 2u));
    arguments[0] = ORACLE_GL_TEXTURE_2D;
    arguments[1] = 0u;
    arguments[2] = ORACLE_GL_RGBA;
    arguments[3] = 256u;
    arguments[4] = 1u;
    arguments[5] = 0u;
    arguments[6] = ORACLE_GL_ABGR_EXT;
    arguments[7] = ORACLE_GL_UNSIGNED_BYTE;
    arguments[8] = 0u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_TEX_IMAGE, arguments, 9u));

    /* Unknown names remain observable without being adopted into the
     * lifecycle; nonzero mip levels are separate from level-zero redefine. */
    arguments[0] = ORACLE_GL_TEXTURE_2D;
    arguments[1] = 9u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BIND_TEXTURE, arguments, 2u));
    arguments[0] = ORACLE_GL_TEXTURE_2D;
    arguments[1] = 0u;
    arguments[2] = ORACLE_GL_RGBA;
    arguments[3] = 1u;
    arguments[4] = 1u;
    arguments[5] = 0u;
    arguments[6] = ORACLE_GL_RGBA;
    arguments[7] = ORACLE_GL_UNSIGNED_BYTE;
    arguments[8] = 0u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_TEX_IMAGE, arguments, 9u));
    arguments[0] = ORACLE_GL_TEXTURE_2D;
    arguments[1] = 4u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BIND_TEXTURE, arguments, 2u));
    arguments[0] = ORACLE_GL_TEXTURE_2D;
    arguments[1] = 1u;
    arguments[2] = ORACLE_GL_RGBA;
    arguments[3] = 1u;
    arguments[4] = 1u;
    arguments[5] = 0u;
    arguments[6] = ORACLE_GL_RGBA;
    arguments[7] = ORACLE_GL_UNSIGNED_BYTE;
    arguments[8] = 0u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_TEX_IMAGE, arguments, 9u));

    /* glDeleteTextures permits batches even though the two frozen destructor
     * sites currently issue n=1.  Exercise exact multi-name lifecycle and
     * binding cleanup so that assumption is not required for safety. */
    deleted[0] = 1u;
    deleted[1] = 4u;
    arguments[0] = 2u;
    arguments[1] = (uint32_t)(uintptr_t)deleted;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_DELETE_TEXTURES, arguments, 2u));

    /* Slot exhaustion leaves output untouched.  This fixture therefore
     * yields an equal before/after value and is conservatively ambiguous. */
    texture = 1u;
    arguments[0] = 1u;
    arguments[1] = (uint32_t)(uintptr_t)&texture;
    s_native_gen_texture_write = 0;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_GEN_TEXTURES, arguments, 2u));

    /* A changed output proves recycling and can be adopted exactly. */
    s_native_gen_texture_write = 1;
    s_native_gen_texture_value = 1u;
    texture = 0u;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_GEN_TEXTURES, arguments, 2u));

    /* Delete it again while retaining the old value in guest storage.  A
     * real successful write of the same recycled ID is observationally
     * identical to the exhaustion fixture above, so it must also be amb. */
    arguments[0] = 1u;
    arguments[1] = (uint32_t)(uintptr_t)&texture;
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_DELETE_TEXTURES, arguments, 2u));
    CHECK(oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_GEN_TEXTURES, arguments, 2u));

    gl_vita_backend_texture_churn_profile_take_window(&profile);
    CHECK(s_native_gen_texture_calls == 5u);
    CHECK(profile.gen_calls == 5u);
    CHECK(profile.gen_names == 3u);
    CHECK(profile.gen_ambiguous == 2u);
    CHECK(profile.gen_observed_us == 35u);
    CHECK(profile.gen_scan_slots == 6u);
    CHECK(profile.delete_calls == 2u);
    CHECK(profile.delete_names == 3u);
    CHECK(profile.delete_native_observed_us == 36u);
    CHECK(profile.delete_native_max_observed_us == 18u);
    CHECK(profile.delete_post_observed_us == 14u);
    CHECK(profile.delete_post_max_observed_us == 7u);
    CHECK(profile.image_first == 2u);
    CHECK(profile.image_redefine == 1u);
    CHECK(profile.image_unknown == 1u);
    CHECK(profile.image_other_level == 1u);
    CHECK(profile.path_linear == 3u);
    CHECK(profile.path_converted == 1u);
    CHECK(profile.path_other == 1u);
    CHECK(profile.known_pixels == 297u);
    CHECK(profile.known_alloc_bytes == 1120u);
    CHECK(profile.image_observed_us == 35u);
    CHECK(profile.image_max_observed_us == 7u);
    CHECK(profile.logical_live == 0u);
    CHECK(profile.window_peak_live == 2u);
    CHECK(profile.recycled_names == 1u);
    CHECK(profile.bad == 2u);
    CHECK(profile.clock_calls == 2u * (
        profile.gen_calls + profile.image_first + profile.image_redefine +
        profile.image_unknown + profile.image_other_level) +
        4u * profile.delete_calls);
    CHECK(profile.clock_calls == 28u);
    CHECK(profile.clock_pair_max_us == 7u);

    memset(&profile, 0xff, sizeof profile);
    gl_vita_backend_texture_churn_profile_take_window(&profile);
    CHECK(profile.gen_calls == 0u && profile.image_first == 0u);
    CHECK(profile.delete_calls == 0u && profile.delete_names == 0u);
    CHECK(profile.delete_native_observed_us == 0u &&
          profile.delete_native_max_observed_us == 0u);
    CHECK(profile.delete_post_observed_us == 0u &&
          profile.delete_post_max_observed_us == 0u);
    CHECK(profile.logical_live == 0u && profile.window_peak_live == 0u);
    CHECK(profile.bad == 1u);
    gl_vita_backend_uninstall();
    memset(&profile, 0xff, sizeof profile);
    gl_vita_backend_texture_churn_profile_take_window(&profile);
    CHECK(profile.logical_live == 0u && profile.window_peak_live == 0u);
    CHECK(profile.bad == 0u);
    s_native_delete_texture_clock_cost = 0u;
    CHECK(gl_vita_backend_install());
    return 0;
}
#endif

#if defined(ISAAC_VITA_FBO_CLEAR_ELISION) || defined(ISAAC_VITA_FBO_RASTER_SCALE)
#define ORACLE_TOKEN_CLEAR 0x7e194b2bu
#define ORACLE_TOKEN_CLEAR_COLOR 0x7e9e5cb0u
#define ORACLE_TOKEN_ENABLE 0x7ed43cfdu
#define ORACLE_TOKEN_GEN_FRAMEBUFFERS 0x7e98408fu
#define ORACLE_TOKEN_READ_PIXELS 0x7e038504u
#define ORACLE_TOKEN_FRAMEBUFFER_RENDERBUFFER 0x7e8a2af9u
#define ORACLE_GL_COLOR_ATTACHMENT0 0x00008ce0u
#define ORACLE_GL_COLOR_BUFFER_BIT 0x00004000u
#define ORACLE_GL_DEPTH_BUFFER_BIT 0x00000100u
#define ORACLE_GL_STENCIL_BUFFER_BIT 0x00000400u
#define ORACLE_GL_SCISSOR_TEST 0x00000c11u

static unsigned s_fbo_guest_clear_calls;

static int oracle_fbo_clear(CPU *cpu, uint32_t stack_top, uint32_t mask)
{
    uint32_t arguments[1];

    arguments[0] = mask;
    ++s_fbo_guest_clear_calls;
    return oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_CLEAR, arguments, 1u);
}

static int oracle_fbo_clear_color(
    CPU *cpu, uint32_t stack_top,
    float red, float green, float blue, float alpha)
{
    uint32_t arguments[4];

    memcpy(&arguments[0], &red, sizeof red);
    memcpy(&arguments[1], &green, sizeof green);
    memcpy(&arguments[2], &blue, sizeof blue);
    memcpy(&arguments[3], &alpha, sizeof alpha);
    return oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_CLEAR_COLOR, arguments, 4u);
}

#if !defined(ISAAC_VITA_FBO_CLEAR_ELISION_DEPTH_DROP)
/* Only the plain elision test folds owed depths; the depth-drop build must
 * stay -Wunused-function clean under -Werror. */
static int oracle_fbo_clear_depth(
    CPU *cpu, uint32_t stack_top, double depth)
{
    uint32_t arguments[2];
    uint64_t bits;

    memcpy(&bits, &depth, sizeof bits);
    arguments[0] = (uint32_t)bits;
    arguments[1] = (uint32_t)(bits >> 32);
    return oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_CLEAR_DEPTH, arguments, 2u);
}
#endif

static int oracle_fbo_attach(
    CPU *cpu, uint32_t stack_top, uint32_t target, uint32_t texture)
{
    uint32_t arguments[5];

    arguments[0] = target;
    arguments[1] = ORACLE_GL_COLOR_ATTACHMENT0;
    arguments[2] = ORACLE_GL_TEXTURE_2D;
    arguments[3] = texture;
    arguments[4] = 0u;
    return oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_FRAMEBUFFER_TEXTURE, arguments, 5u);
}

#if defined(ISAAC_VITA_FBO_CLEAR_ELISION_DEPTH_DROP)
/* Every argument spelled out: detaches, other levels and texture targets. */
static int oracle_fbo_attach_full(
    CPU *cpu, uint32_t stack_top, uint32_t target, uint32_t attachment,
    uint32_t texture_target, uint32_t texture, uint32_t level)
{
    uint32_t arguments[5];

    arguments[0] = target;
    arguments[1] = attachment;
    arguments[2] = texture_target;
    arguments[3] = texture;
    arguments[4] = level;
    return oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_FRAMEBUFFER_TEXTURE, arguments, 5u);
}
#endif

static int oracle_fbo_draw(CPU *cpu, uint32_t stack_top)
{
    uint32_t arguments[4];

    arguments[0] = ORACLE_GL_TRIANGLES;
    arguments[1] = 6u;
    arguments[2] = ORACLE_GL_UNSIGNED_SHORT;
    arguments[3] = 0x1000u;
    return oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_DRAW_ELEMENTS, arguments, 4u);
}

static int oracle_fbo_enable(CPU *cpu, uint32_t stack_top, uint32_t cap)
{
    uint32_t arguments[1];

    arguments[0] = cap;
    return oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_ENABLE, arguments, 1u);
}

/* The native facade never writes the output array, so the caller stores the
 * "generated" name first, exactly as a real driver would have. */
static int oracle_fbo_gen(CPU *cpu, uint32_t stack_top, uint32_t *name)
{
    uint32_t arguments[2];

    arguments[0] = 1u;
    arguments[1] = (uint32_t)(uintptr_t)name;
    return oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_GEN_FRAMEBUFFERS, arguments, 2u);
}

static int oracle_fbo_delete_texture(
    CPU *cpu, uint32_t stack_top, uint32_t *name)
{
    uint32_t arguments[2];

    arguments[0] = 1u;
    arguments[1] = (uint32_t)(uintptr_t)name;
    return oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_DELETE_TEXTURES, arguments, 2u);
}

#if defined(ISAAC_VITA_FBO_RASTER_SCALE)
/* Only the raster-scale case selects texture units; a clear-elision-only
 * build must stay -Wunused-function clean under -Werror. */
static int oracle_fbo_active_texture(
    CPU *cpu, uint32_t stack_top, uint32_t unit)
{
    uint32_t arguments[1];

    arguments[0] = ORACLE_GL_TEXTURE0 + unit;
    return oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_ACTIVE_TEXTURE, arguments, 1u);
}
#endif
#endif

#if defined(ISAAC_VITA_FBO_CLEAR_ELISION)
static int oracle_fbo_read_pixels(CPU *cpu, uint32_t stack_top)
{
    static uint8_t pixels[4];
    uint32_t arguments[7];

    arguments[0] = 0u;
    arguments[1] = 0u;
    arguments[2] = 1u;
    arguments[3] = 1u;
    arguments[4] = ORACLE_GL_RGBA;
    arguments[5] = ORACLE_GL_UNSIGNED_BYTE;
    arguments[6] = (uint32_t)(uintptr_t)pixels;
    return oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_READ_PIXELS, arguments, 7u);
}

#if !defined(ISAAC_VITA_FBO_CLEAR_ELISION_DEPTH_DROP)
/* Colour renderbuffers only matter to the colour-note path of the plain
 * elision test. */
static int oracle_fbo_attach_renderbuffer(
    CPU *cpu, uint32_t stack_top, uint32_t attachment, uint32_t renderbuffer)
{
    uint32_t arguments[4];

    arguments[0] = ORACLE_GL_FRAMEBUFFER;
    arguments[1] = attachment;
    arguments[2] = ORACLE_GL_RENDERBUFFER;
    arguments[3] = renderbuffer;
    return oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_FRAMEBUFFER_RENDERBUFFER, arguments, 4u);
}
#endif
#endif

#if defined(ISAAC_VITA_FBO_CLEAR_ELISION) && \
    !defined(ISAAC_VITA_FBO_CLEAR_ELISION_DEPTH_DROP)
/* Exact clear elision on offscreen targets: colour no-ops are absorbed,
 * depth/stencil clears are owed to the first draw of the same GXM scene.
 * (The depth-drop sub-mode changes the colour path; its own test follows.) */
static int oracle_test_fbo_clear_elision(CPU *cpu, uint32_t stack_top)
{
    static uint8_t pixels[16 * 16 * 4];
    uint32_t name;
    unsigned native_before;
    unsigned guest_before;
    unsigned depth_calls;
    unsigned bind_calls;
    unsigned draw_calls;
    unsigned attach_calls;

    gl_vita_backend_uninstall();
    CHECK(gl_vita_backend_install());
    native_before = s_native_clear_calls;
    guest_before = s_fbo_guest_clear_calls;

    /* Fresh offscreen target: the first clear is native. */
    CHECK(oracle_bind_framebuffer(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 7u));
    CHECK(oracle_fbo_attach(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 5u));
    CHECK(oracle_fbo_clear_color(cpu, stack_top, 0.1f, 0.2f, 0.3f, 1.0f));
    CHECK(oracle_fbo_clear(cpu, stack_top,
        ORACLE_GL_COLOR_BUFFER_BIT | ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 1u);
    CHECK(s_native_clear_mask ==
        (ORACLE_GL_COLOR_BUFFER_BIT | ORACLE_GL_DEPTH_BUFFER_BIT));
    /* The texture holds that colour: every request is absorbed; the depth
     * and stencil parts become owed. */
    CHECK(oracle_fbo_clear(cpu, stack_top,
        ORACLE_GL_COLOR_BUFFER_BIT | ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_COLOR_BUFFER_BIT));
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(oracle_fbo_clear(cpu, stack_top, 0u));
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_STENCIL_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 1u);
    /* The first draw replays the owed buffers as one native clear (the
     * stock quad of the same scene) with no clear-depth traffic, because
     * the owed depth is the current one. */
    depth_calls = s_native_clear_depth_calls;
    draw_calls = s_native_draw_elements_calls;
    CHECK(oracle_fbo_draw(cpu, stack_top));
    CHECK(s_native_clear_calls == native_before + 2u);
    CHECK(s_native_clear_mask ==
        (ORACLE_GL_DEPTH_BUFFER_BIT | ORACLE_GL_STENCIL_BUFFER_BIT));
    CHECK(s_native_clear_depth_calls == depth_calls);
    CHECK(s_native_draw_elements_calls == draw_calls + 1u);
    /* The draw forgot the colour; depth is owed again and replayed once. */
    CHECK(oracle_fbo_clear(cpu, stack_top,
        ORACLE_GL_COLOR_BUFFER_BIT | ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 3u);
    CHECK(oracle_fbo_clear(cpu, stack_top,
        ORACLE_GL_COLOR_BUFFER_BIT | ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 3u);
    CHECK(oracle_fbo_draw(cpu, stack_top));
    CHECK(s_native_clear_calls == native_before + 4u);
    CHECK(s_native_clear_mask == ORACLE_GL_DEPTH_BUFFER_BIT);
    CHECK(oracle_fbo_draw(cpu, stack_top));
    CHECK(s_native_clear_calls == native_before + 4u);
    /* A different clear colour is a real change; the same colour again is
     * not.  The comparison is on exact float bits. */
    CHECK(oracle_fbo_clear_color(cpu, stack_top, 0.1f, 0.2f, 0.3f, 0.5f));
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_COLOR_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 5u);
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_COLOR_BUFFER_BIT));
    CHECK(oracle_fbo_clear_color(cpu, stack_top, 0.1f, 0.2f, 0.3f, 0.5f));
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_COLOR_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 5u);
    /* An owed depth folds into the next native clear of the target with the
     * depth it was requested under; the guest's clear depth is restored. */
    CHECK(oracle_fbo_clear_depth(cpu, stack_top, 0.5));
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 5u);
    CHECK(oracle_fbo_clear_depth(cpu, stack_top, 0.25));
    depth_calls = s_native_clear_depth_calls;
    CHECK(oracle_fbo_clear_color(cpu, stack_top, 0.9f, 0.9f, 0.9f, 0.9f));
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_COLOR_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 6u);
    CHECK(s_native_clear_mask ==
        (ORACLE_GL_COLOR_BUFFER_BIT | ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(s_native_clear_depth_calls == depth_calls + 2u);
    CHECK(s_native_clear_depth == 0.25);
    /* A depth the guest requests again supersedes the owed one. */
    CHECK(oracle_fbo_clear_depth(cpu, stack_top, 0.5));
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(oracle_fbo_clear_depth(cpu, stack_top, 0.75));
    depth_calls = s_native_clear_depth_calls;
    CHECK(oracle_fbo_clear_color(cpu, stack_top, 0.8f, 0.8f, 0.8f, 0.8f));
    CHECK(oracle_fbo_clear(cpu, stack_top,
        ORACLE_GL_COLOR_BUFFER_BIT | ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 7u);
    CHECK(s_native_clear_depth_calls == depth_calls);
    CHECK(s_native_clear_depth == 0.75);
    /* Clamped depths compare on the clamped bits (-1000 and -5 are both
     * 0.0), so the replay needs no retargeting. */
    CHECK(oracle_fbo_clear_depth(cpu, stack_top, -1000.0));
    CHECK(oracle_fbo_clear(cpu, stack_top,
        ORACLE_GL_COLOR_BUFFER_BIT | ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 7u);
    CHECK(oracle_fbo_clear_depth(cpu, stack_top, -5.0));
    depth_calls = s_native_clear_depth_calls;
    CHECK(oracle_fbo_draw(cpu, stack_top));
    CHECK(s_native_clear_calls == native_before + 8u);
    CHECK(s_native_clear_mask == ORACLE_GL_DEPTH_BUFFER_BIT);
    CHECK(s_native_clear_depth_calls == depth_calls);
    /* A clear on another framebuffer ends the owing scene in stock vitaGL:
     * the owed clear is dropped, never replayed. */
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(oracle_bind_framebuffer(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 8u));
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_COLOR_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 9u);
    CHECK(oracle_fbo_attach(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 6u));
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_COLOR_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 10u);
    CHECK(oracle_bind_framebuffer(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 7u));
    CHECK(oracle_fbo_draw(cpu, stack_top));
    CHECK(s_native_clear_calls == native_before + 10u);
    /* So does the present. */
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_DEPTH_BUFFER_BIT));
    gl_vita_backend_fbo_present();
    CHECK(oracle_fbo_draw(cpu, stack_top));
    CHECK(s_native_clear_calls == native_before + 10u);
    /* Colour knowledge follows the texture across the manager framebuffer's
     * attachment swaps (texture 6 was cleared to 0.8 through framebuffer 8). */
    CHECK(oracle_fbo_clear_color(cpu, stack_top, 0.1f, 0.2f, 0.3f, 1.0f));
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_COLOR_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 11u);
    CHECK(oracle_fbo_attach(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 6u));
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_COLOR_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 12u);
    CHECK(oracle_fbo_attach(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 5u));
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_COLOR_BUFFER_BIT));
    CHECK(oracle_fbo_attach(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 6u));
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_COLOR_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 12u);
    /* Drawing through another framebuffer forgets the shared texture. */
    CHECK(oracle_bind_framebuffer(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 8u));
    CHECK(oracle_fbo_draw(cpu, stack_top));
    CHECK(s_native_clear_calls == native_before + 12u);
    CHECK(oracle_bind_framebuffer(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 7u));
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_COLOR_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 13u);
    /* An attachment change settles the owed clear first (vitaGL would open a
     * new scene at the next clear or draw). */
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_DEPTH_BUFFER_BIT));
    attach_calls = s_native_framebuffer_texture_calls;
    CHECK(oracle_fbo_attach(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 5u));
    CHECK(s_native_clear_calls == native_before + 14u);
    CHECK(s_native_clear_mask == ORACLE_GL_DEPTH_BUFFER_BIT);
    CHECK(s_native_framebuffer_texture_calls == attach_calls + 1u);
    /* glReadPixels, texture uploads and texture deletion settle it too, and
     * uploads forget every colour. */
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(oracle_fbo_read_pixels(cpu, stack_top));
    CHECK(s_native_clear_calls == native_before + 15u);
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(oracle_bind_texture(cpu, stack_top, 42u));
    CHECK(oracle_tex_sub_image(cpu, stack_top, 16, 16, pixels));
    CHECK(s_native_clear_calls == native_before + 16u);
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_COLOR_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 17u);
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(oracle_tex_image(cpu, stack_top, 0, (int32_t)ORACLE_GL_RGBA,
        16, 16, 0, ORACLE_GL_RGBA, ORACLE_GL_UNSIGNED_BYTE, pixels));
    CHECK(s_native_clear_calls == native_before + 18u);
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_DEPTH_BUFFER_BIT));
    name = 42u;
    CHECK(oracle_fbo_delete_texture(cpu, stack_top, &name));
    CHECK(s_native_clear_calls == native_before + 19u);
    /* A target bound away from without any clear or draw still owes: the
     * settlement rebinds it natively around the quad. */
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(oracle_bind_framebuffer(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 8u));
    bind_calls = s_native_bind_framebuffer_calls;
    CHECK(oracle_fbo_read_pixels(cpu, stack_top));
    CHECK(s_native_clear_calls == native_before + 20u);
    CHECK(s_native_bind_framebuffer_calls == bind_calls + 2u);
    CHECK(s_native_bind_framebuffer_name == 8u);
    CHECK(oracle_fbo_draw(cpu, stack_top));
    CHECK(s_native_clear_calls == native_before + 20u);
    /* A colour renderbuffer (rejected by vitaGL) fails closed: no colour
     * knowledge on that target. */
    CHECK(oracle_bind_framebuffer(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 7u));
    CHECK(oracle_fbo_attach_renderbuffer(
        cpu, stack_top, ORACLE_GL_COLOR_ATTACHMENT0, 3u));
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_COLOR_BUFFER_BIT));
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_COLOR_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 22u);
    CHECK(oracle_fbo_attach(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 5u));
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_COLOR_BUFFER_BIT));
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_COLOR_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 23u);
    /* The default framebuffer is never absorbed. */
    CHECK(oracle_bind_framebuffer(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 0u));
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_COLOR_BUFFER_BIT));
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_COLOR_BUFFER_BIT));
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 26u);
    /* Deleting the owing framebuffer drops its owed clear; the regenerated
     * name starts from scratch. */
    CHECK(oracle_bind_framebuffer(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 7u));
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 26u);
    name = 7u;
    CHECK(oracle_delete_framebuffer(cpu, stack_top, &name));
    name = 7u;
    CHECK(oracle_fbo_gen(cpu, stack_top, &name));
    CHECK(oracle_bind_framebuffer(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 7u));
    CHECK(oracle_fbo_draw(cpu, stack_top));
    CHECK(s_native_clear_calls == native_before + 26u);
    /* A scissor enable settles the owed clear, then poisons the policy for
     * good. */
    CHECK(oracle_fbo_attach(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 5u));
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_COLOR_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 27u);
    CHECK(oracle_fbo_clear(cpu, stack_top,
        ORACLE_GL_COLOR_BUFFER_BIT | ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 27u);
    CHECK(oracle_fbo_enable(cpu, stack_top, ORACLE_GL_SCISSOR_TEST));
    CHECK(s_native_clear_calls == native_before + 28u);
    CHECK(s_native_clear_mask == ORACLE_GL_DEPTH_BUFFER_BIT);
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_COLOR_BUFFER_BIT));
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_COLOR_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 30u);
#if defined(ISAAC_VITA_PHASE_PROFILE)
    /* Native clears = guest clears - absorbed + replayed (folds are free). */
    CHECK(g_isaac_vita_gl_phase_profile_counters.clear_replayed == 10u);
    CHECK(g_isaac_vita_gl_phase_profile_counters.clear_suppressed ==
          (s_fbo_guest_clear_calls - guest_before) -
          (s_native_clear_calls - native_before) +
          g_isaac_vita_gl_phase_profile_counters.clear_replayed);
    CHECK(g_isaac_vita_gl_phase_profile_counters.fbo_elision_poison == 1u);
#else
    (void)guest_before;
#endif
    return 0;
}
#endif

#if defined(ISAAC_VITA_FBO_CLEAR_ELISION_DEPTH_DROP)
/* Depth-drop sub-mode: colour clears are always native (colour notes never
 * consulted); an owed depth/stencil clear is replayed before a draw or an
 * unmodelled call, dropped at a tracked COLOR_ATTACHMENT0 re-attach of the
 * owing framebuffer (a), or dropped when stock vitaGL ends the owing scene
 * (d).  The closure x == r + a + d holds when no owed clear is folded. */
static int oracle_test_fbo_clear_elision_depth_drop(
    CPU *cpu, uint32_t stack_top)
{
    static uint8_t pixels[16 * 16 * 4];
    const uint32_t read_framebuffer = 0x00008ca8u;
    const uint32_t cube_positive_x = 0x00008515u;
    uint32_t name;
    unsigned native_before;
    unsigned guest_before;
    unsigned attach_calls;
    unsigned bind_calls;
#if defined(ISAAC_VITA_PHASE_PROFILE)
    uint32_t replayed_before = g_isaac_vita_gl_phase_profile_counters.clear_replayed;
    uint32_t suppressed_before = g_isaac_vita_gl_phase_profile_counters.clear_suppressed;
    uint32_t attach_before = g_isaac_vita_gl_phase_profile_counters.clear_dropped_attach;
    uint32_t scene_before = g_isaac_vita_gl_phase_profile_counters.clear_dropped_scene;
    uint32_t poison_before = g_isaac_vita_gl_phase_profile_counters.fbo_elision_poison;
#endif

    gl_vita_backend_uninstall();
    CHECK(gl_vita_backend_install());
    native_before = s_native_clear_calls;
    guest_before = s_fbo_guest_clear_calls;

    /* Colour requests are native even when the texture holds the colour. */
    CHECK(oracle_bind_framebuffer(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 7u));
    CHECK(oracle_fbo_attach(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 5u));
    CHECK(oracle_fbo_clear_color(cpu, stack_top, 0.1f, 0.2f, 0.3f, 1.0f));
    CHECK(oracle_fbo_clear(cpu, stack_top,
        ORACLE_GL_COLOR_BUFFER_BIT | ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 1u);
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_COLOR_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 2u);
    CHECK(s_native_clear_mask == ORACLE_GL_COLOR_BUFFER_BIT);
    CHECK(oracle_fbo_clear(cpu, stack_top,
        ORACLE_GL_COLOR_BUFFER_BIT | ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 3u);
    /* A depth-only request is owed and replayed by the first draw. */
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 3u);
    CHECK(oracle_fbo_draw(cpu, stack_top));
    CHECK(s_native_clear_calls == native_before + 4u);
    CHECK(s_native_clear_mask == ORACLE_GL_DEPTH_BUFFER_BIT);
    /* The owing framebuffer re-attaching a level-0 2D texture drops the owed
     * clear (vitaGL ends that scene at the next clear or draw). */
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_DEPTH_BUFFER_BIT));
    attach_calls = s_native_framebuffer_texture_calls;
    CHECK(oracle_fbo_attach(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 6u));
    CHECK(s_native_clear_calls == native_before + 4u);
    CHECK(s_native_framebuffer_texture_calls == attach_calls + 1u);
    CHECK(oracle_fbo_clear(cpu, stack_top,
        ORACLE_GL_COLOR_BUFFER_BIT | ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 5u);
    /* An attach on another framebuffer replays (with the native rebind). */
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(oracle_bind_framebuffer(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 8u));
    CHECK(s_native_clear_calls == native_before + 5u);
    CHECK(oracle_fbo_attach(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 5u));
    CHECK(s_native_clear_calls == native_before + 6u);
    CHECK(s_native_clear_mask == ORACLE_GL_DEPTH_BUFFER_BIT);
    /* A detach (texture 0) does not dirty the scene in vitaGL: replay. */
    CHECK(oracle_bind_framebuffer(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 7u));
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(oracle_fbo_attach_full(cpu, stack_top, ORACLE_GL_FRAMEBUFFER,
        ORACLE_GL_COLOR_ATTACHMENT0, ORACLE_GL_TEXTURE_2D, 0u, 0u));
    CHECK(s_native_clear_calls == native_before + 7u);
    CHECK(oracle_fbo_attach(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 5u));
    /* Untracked attachments (other level, other texture target) and the read
     * framebuffer target fail closed to the replay. */
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(oracle_fbo_attach_full(cpu, stack_top, ORACLE_GL_FRAMEBUFFER,
        ORACLE_GL_COLOR_ATTACHMENT0, ORACLE_GL_TEXTURE_2D, 6u, 1u));
    CHECK(s_native_clear_calls == native_before + 8u);
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(oracle_fbo_attach_full(cpu, stack_top, ORACLE_GL_FRAMEBUFFER,
        ORACLE_GL_COLOR_ATTACHMENT0, cube_positive_x, 6u, 0u));
    CHECK(s_native_clear_calls == native_before + 9u);
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(oracle_fbo_attach_full(cpu, stack_top, read_framebuffer,
        ORACLE_GL_COLOR_ATTACHMENT0, ORACLE_GL_TEXTURE_2D, 6u, 0u));
    CHECK(s_native_clear_calls == native_before + 10u);
    /* Scene-ending drops: a clear on another framebuffer, the present, the
     * deletion of the owing framebuffer. */
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(oracle_bind_framebuffer(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 8u));
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_COLOR_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 11u);
    CHECK(s_native_clear_mask == ORACLE_GL_COLOR_BUFFER_BIT);
    CHECK(oracle_bind_framebuffer(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 7u));
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_DEPTH_BUFFER_BIT));
    gl_vita_backend_fbo_present();
    CHECK(oracle_fbo_draw(cpu, stack_top));
    CHECK(s_native_clear_calls == native_before + 11u);
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_DEPTH_BUFFER_BIT));
    name = 7u;
    CHECK(oracle_delete_framebuffer(cpu, stack_top, &name));
    name = 7u;
    CHECK(oracle_fbo_gen(cpu, stack_top, &name));
    CHECK(oracle_bind_framebuffer(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 7u));
    CHECK(oracle_fbo_attach(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 5u));
    CHECK(oracle_fbo_draw(cpu, stack_top));
    CHECK(s_native_clear_calls == native_before + 11u);
    /* The unmodelled settlements replay the owed depth clear exactly as the
     * plain elision does: glReadPixels, a texture upload, a texture deletion,
     * and the readback of a target bound away from (natively rebound around
     * the quad). */
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(oracle_fbo_read_pixels(cpu, stack_top));
    CHECK(s_native_clear_calls == native_before + 12u);
    CHECK(s_native_clear_mask == ORACLE_GL_DEPTH_BUFFER_BIT);
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(oracle_bind_texture(cpu, stack_top, 42u));
    CHECK(oracle_tex_sub_image(cpu, stack_top, 16, 16, pixels));
    CHECK(s_native_clear_calls == native_before + 13u);
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_DEPTH_BUFFER_BIT));
    name = 42u;
    CHECK(oracle_fbo_delete_texture(cpu, stack_top, &name));
    CHECK(s_native_clear_calls == native_before + 14u);
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(oracle_bind_framebuffer(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 8u));
    bind_calls = s_native_bind_framebuffer_calls;
    CHECK(oracle_fbo_read_pixels(cpu, stack_top));
    CHECK(s_native_clear_calls == native_before + 15u);
    CHECK(s_native_bind_framebuffer_calls == bind_calls + 2u);
    CHECK(s_native_bind_framebuffer_name == 8u);
    CHECK(oracle_fbo_draw(cpu, stack_top));
    CHECK(s_native_clear_calls == native_before + 15u);
    /* A scissor enable settles the owed clear and poisons the policy for
     * good: every later clear is native. */
    CHECK(oracle_bind_framebuffer(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 7u));
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 15u);
    CHECK(oracle_fbo_enable(cpu, stack_top, ORACLE_GL_SCISSOR_TEST));
    CHECK(s_native_clear_calls == native_before + 16u);
    CHECK(s_native_clear_mask == ORACLE_GL_DEPTH_BUFFER_BIT);
    CHECK(oracle_fbo_clear(cpu, stack_top, ORACLE_GL_DEPTH_BUFFER_BIT));
    CHECK(s_native_clear_calls == native_before + 17u);
    CHECK(s_fbo_guest_clear_calls == guest_before + 21u);
#if defined(ISAAC_VITA_PHASE_PROFILE)
    {
        uint32_t replayed =
            g_isaac_vita_gl_phase_profile_counters.clear_replayed -
            replayed_before;
        uint32_t suppressed =
            g_isaac_vita_gl_phase_profile_counters.clear_suppressed -
            suppressed_before;
        uint32_t dropped_attach =
            g_isaac_vita_gl_phase_profile_counters.clear_dropped_attach -
            attach_before;
        uint32_t dropped_scene =
            g_isaac_vita_gl_phase_profile_counters.clear_dropped_scene -
            scene_before;

        CHECK(replayed == 11u);
        CHECK(dropped_attach == 1u);
        CHECK(dropped_scene == 3u);
        CHECK(suppressed == 15u);
        CHECK(g_isaac_vita_gl_phase_profile_counters.fbo_elision_poison ==
              poison_before + 1u);
        /* ph120.e closures: x == r + a + d (no folds here) and native clears
         * == guest clears - x + r. */
        CHECK(suppressed == replayed + dropped_attach + dropped_scene);
        CHECK(suppressed ==
              (s_fbo_guest_clear_calls - guest_before) -
              (s_native_clear_calls - native_before) + replayed);
    }
#endif
    return 0;
}
#endif

#if defined(ISAAC_VITA_FBO_RASTER_SCALE)
/* Reduced raster for NULL-defined screen-sized colour targets (test builds
 * define NUM/DEN = 1/2). */
static int oracle_test_fbo_raster_scale(CPU *cpu, uint32_t stack_top)
{
    static uint8_t pixels[960 * 4];
    int32_t viewport_redzone[6];
    uint32_t name;
    unsigned tex_before;
    unsigned viewport_before;

    gl_vita_backend_uninstall();
    /* The viewport recorder keeps 32 calls; start this scenario fresh. */
    s_native_viewport_calls = 0u;
    CHECK(gl_vita_backend_install());
    tex_before = s_native_tex_image_calls;

    CHECK(oracle_fbo_active_texture(cpu, stack_top, 0u));
    CHECK(oracle_bind_texture(cpu, stack_top, 5u));
    CHECK(oracle_tex_image(cpu, stack_top, 0, (int32_t)ORACLE_GL_RGBA,
        960, 540, 0, ORACLE_GL_RGBA, ORACLE_GL_UNSIGNED_BYTE, NULL));
    CHECK(s_native_tex_image_calls == tex_before + 1u);
    CHECK(s_native_tex_image_width == 480 && s_native_tex_image_height == 270);
    CHECK(s_native_tex_image_pixels == NULL);
    /* World-sized and data-carrying images keep their logical size. */
    CHECK(oracle_bind_texture(cpu, stack_top, 9u));
    CHECK(oracle_tex_image(cpu, stack_top, 0, (int32_t)ORACLE_GL_RGBA,
        480, 270, 0, ORACLE_GL_RGBA, ORACLE_GL_UNSIGNED_BYTE, NULL));
    CHECK(s_native_tex_image_width == 480 && s_native_tex_image_height == 270);
    CHECK(oracle_bind_texture(cpu, stack_top, 10u));
    CHECK(oracle_tex_image(cpu, stack_top, 0, (int32_t)ORACLE_GL_RGBA,
        960, 540, 0, ORACLE_GL_RGBA, ORACLE_GL_UNSIGNED_BYTE, pixels));
    CHECK(s_native_tex_image_width == 960 && s_native_tex_image_height == 540);
    CHECK(s_native_tex_image_calls == tex_before + 3u);

    /* Attaching the scaled texture scales the draw viewport; the guest's
     * GL_VIEWPORT readback stays logical. */
    CHECK(oracle_set_viewport(cpu, stack_top, 0, 0, 960, 540));
    CHECK(oracle_bind_framebuffer(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 7u));
    CHECK(oracle_last_viewport_is(0, 0, 960, 540));
    viewport_before = s_native_viewport_calls;
    CHECK(oracle_fbo_attach(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 5u));
    CHECK(s_native_viewport_calls == viewport_before + 1u);
    CHECK(oracle_last_viewport_is(0, 0, 480, 270));
    CHECK(oracle_set_viewport(cpu, stack_top, 1, 2, 5, 7));
    CHECK(oracle_last_viewport_is(0, 1, 3, 3));
    CHECK(oracle_set_viewport(cpu, stack_top, -1, -2, 3, 5));
    CHECK(oracle_last_viewport_is(-1, -1, 2, 2));
    CHECK(oracle_get_viewport(cpu, stack_top, viewport_redzone));
    CHECK(viewport_redzone[1] == -1 && viewport_redzone[2] == -2);
    CHECK(viewport_redzone[3] == 3 && viewport_redzone[4] == 5);
    CHECK(oracle_set_viewport(cpu, stack_top, 1, 2, 5, 7));
    /* Unscaled targets receive exact guest values; the default framebuffer
     * follows its own raster policy. */
    CHECK(oracle_bind_framebuffer(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 8u));
    CHECK(oracle_last_viewport_is(1, 2, 5, 7));
    CHECK(oracle_bind_framebuffer(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 0u));
#if defined(ISAAC_VITA_DISPLAY_RASTER_720)
    CHECK(oracle_last_viewport_is(0, 2, 4, 5));
#else
    CHECK(oracle_last_viewport_is(1, 2, 5, 7));
#endif
    CHECK(oracle_bind_framebuffer(cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 7u));
    CHECK(oracle_last_viewport_is(0, 1, 3, 3));

    /* A pixel upload restores the full allocation first (native
     * glTexImage2D 960x540 NULL) and unscales the attached target. */
    CHECK(oracle_bind_texture(cpu, stack_top, 5u));
    tex_before = s_native_tex_image_calls;
    viewport_before = s_native_viewport_calls;
    CHECK(oracle_tex_sub_image(cpu, stack_top, 16, 16, pixels));
    CHECK(s_native_tex_image_calls == tex_before + 1u);
    CHECK(s_native_tex_image_width == 960 && s_native_tex_image_height == 540);
    CHECK(s_native_tex_image_pixels == NULL);
    CHECK(s_native_viewport_calls == viewport_before + 1u);
    CHECK(oracle_last_viewport_is(1, 2, 5, 7));
    CHECK(oracle_set_viewport(cpu, stack_top, 0, 0, 960, 540));
    CHECK(oracle_last_viewport_is(0, 0, 960, 540));
    /* Redefining it without data scales again and replays the viewport. */
    CHECK(oracle_tex_image(cpu, stack_top, 0, (int32_t)ORACLE_GL_RGBA,
        960, 540, 0, ORACLE_GL_RGBA, ORACLE_GL_UNSIGNED_BYTE, NULL));
    CHECK(s_native_tex_image_width == 480 && s_native_tex_image_height == 270);
    CHECK(oracle_last_viewport_is(0, 0, 480, 270));
    /* Deleting the texture drops the scale. */
    name = 5u;
    CHECK(oracle_fbo_delete_texture(cpu, stack_top, &name));
    CHECK(oracle_set_viewport(cpu, stack_top, 0, 0, 960, 540));
    CHECK(oracle_last_viewport_is(0, 0, 960, 540));
#if defined(ISAAC_VITA_PHASE_PROFILE)
    CHECK(g_isaac_vita_gl_phase_profile_counters.fbo_raster_textures == 2u);
    CHECK(g_isaac_vita_gl_phase_profile_counters.fbo_raster_viewports >= 4u);
    CHECK(g_isaac_vita_gl_phase_profile_counters.fbo_raster_respecified == 1u);
#endif
    return 0;
}
#endif

#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
#include "gl_vita_backend_test_vitagl_undef.h"
#include "../vita/host_tests/vita_attrib_owner_mock.inc"

static int oracle_test_attrib_coalescing(CPU *cpu, uint32_t stack_top)
{
    const guest_gl_backend *backend;
    unsigned enabled_before, disabled_before, index;
    uint32_t faults = 0, draw[4] = {ORACLE_GL_TRIANGLES, 6u, ORACLE_GL_UNSIGNED_SHORT, 0x1000u};
    GLint queried = 0;
    char name[] = "Position";
    gl_vita_backend_uninstall();
    s_native_attrib_mask = 0x8000u; /* Native state is NOT reset on install. */
    CHECK(gl_vita_backend_install());
    backend = guest_gl_installed_backend();
#if defined(ISAAC_VITA_PHASE_PROFILE)
    memset(&g_isaac_vita_gl_phase_profile_counters, 0,
           sizeof g_isaac_vita_gl_phase_profile_counters);
#endif
    enabled_before = s_native_enable_attrib_calls;
    disabled_before = s_native_disable_attrib_calls;
    backend->glEnableVertexAttribArray(0u); /* Unknown -> synchronous learning. */
    CHECK(s_native_enable_attrib_calls == enabled_before + 1u);
    CHECK(s_native_attrib_mask == 0x8001u);
    backend->glDisableVertexAttribArray(0u);
    CHECK(gl_vita_backend_attrib_pending() == 1u && s_native_attrib_mask == 0x8001u);
    backend->glVertexAttribPointer(0u, 2, ORACLE_GL_FLOAT, 0u, 8, 1u);
    (void)backend->glGetAttribLocation(0u, (guest_gl_addr)(uintptr_t)name);
    CHECK(gl_vita_backend_attrib_pending() == 1u); /* Neither consumes mask. */
    backend->glEnableVertexAttribArray(0u);
    CHECK(!gl_vita_backend_attrib_pending());
    CHECK(s_native_disable_attrib_calls == disabled_before);
    CHECK(s_native_enable_attrib_calls == enabled_before + 1u);
#if defined(ISAAC_VITA_PHASE_PROFILE)
    CHECK(g_isaac_vita_gl_phase_profile_counters.attrib_toggle == 1u);
    CHECK(g_isaac_vita_gl_phase_profile_counters.attrib_deferred == 2u);
    CHECK(g_isaac_vita_gl_phase_profile_counters.attrib_cancelled == 2u);
#endif
    backend->glDisableVertexAttribArray(0u);
    backend->glDrawElements(0u, 0, 0u, 1u); /* Invalid/zero ordinary draw still flushes. */
    CHECK(s_native_attrib_observed == 0x8000u && !gl_vita_backend_attrib_pending());
    backend->glEnableVertexAttribArray(0u);
    s_native_canonical_quad_result = 1u;
    CHECK(oracle_dispatch_with_return(cpu, stack_top, ORACLE_TOKEN_DRAW_ELEMENTS,
                                      draw, 4u, 0x0056039du));
    CHECK(s_native_attrib_observed == 0x8001u);
#if defined(ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY)
    CHECK(s_native_canonical_attrib_observed == 0x8001u);
#endif
    backend->glDisableVertexAttribArray(0u);
    s_native_canonical_quad_result = 0u;
    CHECK(oracle_dispatch_with_return(cpu, stack_top, ORACLE_TOKEN_DRAW_ELEMENTS,
                                      draw, 4u, 0x0056039du));
    CHECK(s_native_attrib_observed == 0x8000u);
#if defined(ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY)
    CHECK(s_native_canonical_attrib_observed == 0x8000u);
#endif
    backend->glEnableVertexAttribArray(0u);
    s_native_attrib_invalid_mutation = 1u;
    backend->glDisableVertexAttribArray(32u);
    CHECK(s_native_attrib_observed == 0x8001u); /* Earlier enable reached native first. */
    CHECK(s_native_attrib_mask == 0x8000u && !gl_vita_backend_attrib_pending());
    s_native_attrib_invalid_mutation = 0u;
    enabled_before = s_native_enable_attrib_calls;
    backend->glEnableVertexAttribArray(0u);
    CHECK(s_native_enable_attrib_calls == enabled_before + 1u); /* Poison retrains. */
    backend->glDisableVertexAttribArray(0u);
    backend->glVertexAttribPointer(16u, 2, ORACLE_GL_FLOAT, 0u, 8, 1u);
    CHECK(s_native_attrib_observed == 0x8000u && !gl_vita_backend_attrib_pending());
    backend->glEnableVertexAttribArray(0u);
    CHECK(!gl_vita_backend_attrib_pending()); /* Invalid pointer also poisoned. */
    backend->glDisableVertexAttribArray(0u);
    backend->glGetIntegerv(0x8869u, (guest_gl_addr)(uintptr_t)&queried);
    CHECK(s_native_attrib_observed == 0x8000u && !gl_vita_backend_attrib_pending());
    backend->glEnableVertexAttribArray(0u);
    backend->glClear(0u);
    CHECK(s_native_attrib_observed == 0x8001u && !gl_vita_backend_attrib_pending());

    /* Actual freshly extracted owners; no typed uninstall beforehand and no
     * FBO_CLEAR_ELISION dependency. Native swaps observe the real policy mask. */
    backend->glDisableVertexAttribArray(0u);
    s_active = 1;
    attrib_owner_kage_vita_backend_deactivate();
    CHECK(!s_active && s_native_attrib_mask == 0x8000u && !gl_vita_backend_attrib_pending());
    backend->glEnableVertexAttribArray(0u);
    s_attrib_owner_expected = 0x8001u;
    attrib_owner_kage_loading_swap(0u);
    backend->glDisableVertexAttribArray(0u);
    s_attrib_owner_expected = 0x8000u;
    CHECK(attrib_owner_continue_overlay_begin(1u, 2u, &faults));
    backend->glEnableVertexAttribArray(0u);
    s_attrib_owner_expected = 0x8001u;
    attrib_owner_continue_overlay_stage(1u, 3u);
    CHECK(s_attrib_owner_swaps == 3u && !s_attrib_owner_bad && !faults);

    backend->glDisableVertexAttribArray(0u);
    gl_vita_backend_attrib_external_begin();
    CHECK(s_native_attrib_mask == 0x8000u && !gl_vita_backend_attrib_pending());
    s_native_attrib_mask |= 1u; /* Explicit untracked native owner mutation. */
    backend->glDisableVertexAttribArray(0u);
    CHECK(s_native_attrib_mask == 0x8000u); /* Unknown bit cannot stale-hit. */
    backend->glEnableVertexAttribArray(0u);
    CHECK(gl_vita_backend_attrib_pending() == 1u);
    CHECK(gl_vita_backend_install()); /* Retained-context reinstall flushes first. */
    CHECK(s_native_attrib_mask == 0x8001u && !gl_vita_backend_attrib_pending());
    backend = guest_gl_installed_backend();
    backend->glDisableVertexAttribArray(0u); /* Fresh unknown state, immediate. */
    CHECK(s_native_attrib_mask == 0x8000u);
    backend->glEnableVertexAttribArray(0u);
    gl_vita_backend_uninstall();
    CHECK(s_native_attrib_mask == 0x8001u && !gl_vita_backend_attrib_pending());
    CHECK(gl_vita_backend_install());
    backend = guest_gl_installed_backend();
    /* All 16 bits, reverse setter order, and readback as a consumer. No read
     * of client pointer contents is needed for either coalescing or flushing. */
    for (index = 0u; index < 16u; ++index)
        backend->glEnableVertexAttribArray(index);
    for (index = 16u; index != 0u; --index)
        backend->glDisableVertexAttribArray(index - 1u);
    CHECK(s_native_attrib_mask == 0xffffu && gl_vita_backend_attrib_pending() == 16u);
    backend->glReadPixels(0, 0, 0, 0, ORACLE_GL_RGBA, ORACLE_GL_UNSIGNED_BYTE, 0u);
    CHECK(s_native_attrib_mask == 0u && !gl_vita_backend_attrib_pending());
#if defined(ISAAC_VITA_PHASE_PROFILE)
    {
        IsaacVitaGlPhaseProfileCounters first, second;
        uint32_t p0 = gl_vita_backend_attrib_pending(), p1, p2;
        first = g_isaac_vita_gl_phase_profile_counters;
        backend->glEnableVertexAttribArray(0u); /* Pending crosses the window. */
        p1 = gl_vita_backend_attrib_pending();
        second = g_isaac_vita_gl_phase_profile_counters;
        CHECK(p0 == 0u && p1 == 1u);
        CHECK(second.attrib_toggle == first.attrib_toggle);
        CHECK(second.attrib_deferred - first.attrib_deferred == 1u);
        CHECK(second.attrib_cancelled == first.attrib_cancelled);
        backend->glDisableVertexAttribArray(0u); /* Cancels prior-window request. */
        p2 = gl_vita_backend_attrib_pending();
        CHECK(p2 == 0u);
        CHECK(g_isaac_vita_gl_phase_profile_counters.attrib_toggle == second.attrib_toggle);
        CHECK(g_isaac_vita_gl_phase_profile_counters.attrib_deferred - second.attrib_deferred == 1u);
        CHECK(g_isaac_vita_gl_phase_profile_counters.attrib_cancelled - second.attrib_cancelled == 2u);
        /* Window two has one request: native 0 + cancellations 2 + pending -1. */
        CHECK(0 + 2 + (int)p2 - (int)p1 == 1);
    }
#endif
    puts("attribute coalescing: actual masks, invalid ordering, owner swaps/reset PASS");
    return 0;
}
#endif

#if defined(ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE)
/* Member access by name below (the coalescing test above undefines the fake
 * native names only under its own option). */
#include "gl_vita_backend_test_vitagl_undef.h"
/* ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE (*-direct modes): the batch entry
 * points must leave the backend exactly where the per-call wrapper sequence
 * the Shader::EnableAttribs/DisableAttribs replays issue today leaves it.
 * The same scripted replay inputs are driven through both paths from a
 * fresh install and the vitaGL call trace (every fake native attribute /
 * draw / program call with its arguments, in order), the typed-state shadow
 * bytes, the coalescer's pending count, the fake native mask and every
 * phase counter must compare equal.  Scripts: enable then draw;
 * enable/disable/enable; disable without a prior enable; changed pointers
 * between draws; program switch between draws; glUseProgram/glDrawElements
 * interleaved with the replays; a -1 location (index 0xffffffff, the invalid
 * native path with its sync + poison); a negative stride (invalid pointer
 * path); a repeated location inside one replay (toggle hit path); 16
 * attributes.  Each script runs with locations supplied (the replay's memo
 * hit) and with names looked up by the batch (memo miss / memo OFF /
 * VERIFY).  Declines (count 17, component 0 or 5, NULL locations/tokens, a
 * table whose members are not this backend's wrappers) return 0 and leave
 * trace, state and counters untouched. */
typedef struct direct_spec {
    uint32_t program;
    uint32_t count;
    guest_gl_int locations[16];
    uint8_t components[16];
    guest_gl_sizei stride;
    guest_gl_addr base;
} direct_spec;

typedef struct direct_op {
    char kind;          /* 'E' enable replay, 'D' disable replay, 'd' draw, 'u' use program */
    uint32_t spec;      /* index into s_direct_specs (E/D) or the program (u) */
} direct_op;

static const direct_spec s_direct_specs[] = {
    /* 0: the frozen ColorOffset shape: 7 attributes, stride 48 */
    { 7u, 7u, { 0, 1, 2, 3, 4, 5, 6 }, { 2u, 2u, 4u, 4u, 3u, 1u, 3u }, 48, 0x98800000u },
    /* 1: same shape, next ring slot */
    { 7u, 7u, { 0, 1, 2, 3, 4, 5, 6 }, { 2u, 2u, 4u, 4u, 3u, 1u, 3u }, 48, 0x98800000u + 0x2400u },
    /* 2: another program, 3 attributes at other locations */
    { 9u, 3u, { 4, 0, 9 }, { 3u, 2u, 1u }, 24, 0x98900000u },
    /* 3: a -1 location among valid ones */
    { 7u, 4u, { 0, (guest_gl_int)0xffffffff, 2, 3 }, { 2u, 2u, 4u, 4u }, 48, 0x98800000u },
    /* 4: negative stride */
    { 7u, 2u, { 0, 1 }, { 2u, 2u }, -16, 0x98800000u },
    /* 5: repeated location inside one replay */
    { 7u, 3u, { 5, 5, 1 }, { 1u, 4u, 2u }, 32, 0x98a00000u },
    /* 6: all sixteen attributes */
    { 11u, 16u, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
      { 1u, 2u, 3u, 4u, 1u, 2u, 3u, 4u, 1u, 2u, 3u, 4u, 1u, 2u, 3u, 4u }, 160, 0x98b00000u },
};

static const direct_op s_direct_script[] = {
    { 'u', 7u }, { 'E', 0u }, { 'd', 0u }, { 'D', 0u },          /* enable, draw, disable */
    { 'E', 1u }, { 'd', 0u }, { 'D', 1u },                       /* changed pointers */
    { 'E', 0u }, { 'D', 0u }, { 'E', 0u }, { 'd', 0u }, { 'D', 0u }, /* enable/disable/enable */
    { 'D', 2u },                                                 /* disable without enable */
    { 'u', 9u }, { 'E', 2u }, { 'd', 0u }, { 'D', 2u },          /* program switch */
    { 'u', 7u }, { 'E', 3u }, { 'd', 0u }, { 'D', 3u },          /* -1 location */
    { 'E', 4u }, { 'd', 0u }, { 'D', 4u },                       /* negative stride */
    { 'E', 5u }, { 'u', 11u }, { 'd', 0u }, { 'D', 5u },         /* repeat + program mid-frame */
    { 'E', 6u }, { 'd', 0u }, { 'D', 6u },                       /* sixteen */
    { 'E', 0u }, { 'E', 1u }, { 'd', 0u }, { 'd', 0u }, { 'D', 1u }, { 'D', 0u },
};

static const char *const s_direct_names[16] = {
    "aPosition", "aTexCoord", "aColor", "aColorOffset", "aRenderData",
    "aScale", "aExtra0", "aExtra1", "aExtra2", "aExtra3", "aExtra4",
    "aExtra5", "aExtra6", "aExtra7", "aExtra8", "aExtra9"
};

typedef struct direct_snapshot {
    char trace[sizeof s_direct_trace];
    unsigned char typed[1024];   /* >= sizeof s_gl_typed_state */
    size_t typed_bytes;
    uint32_t pending;
    uint16_t mask;
    guest_gl_int locations[64][16];
    uint32_t replays;
#if defined(ISAAC_VITA_PHASE_PROFILE)
    IsaacVitaGlPhaseProfileCounters counters;
#endif
} direct_snapshot;

static direct_snapshot s_direct_percall, s_direct_batch;

static void direct_reset_backend(void)
{
    gl_vita_backend_uninstall();
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
    s_native_attrib_mask = 0x8000u;
#endif
    s_direct_trace_len = 0u;
    s_direct_trace[0] = '\0';
    s_direct_trace_overflow = 0;
}

/* One run of the script.  direct = 0: the replay's per-call wrapper sequence
 * (lookup, enable, pointer per attribute / lookup, disable); direct = 1: the
 * batch.  by_name: the batch (and the per-call lookups) resolve names. */
static int direct_run(int direct, int by_name, direct_snapshot *out)
{
    const guest_gl_backend *backend;
    IsaacVitaAttribReplayTokens tokens = { 0x7e000001u, 0x7e000002u,
                                           0x7e000003u, 0u };
    size_t op;

    memset(out, 0, sizeof *out);
    direct_reset_backend();
    CHECK(gl_vita_backend_install());
    backend = guest_gl_installed_backend();
#if defined(ISAAC_VITA_PHASE_PROFILE)
    memset(&g_isaac_vita_gl_phase_profile_counters, 0,
           sizeof g_isaac_vita_gl_phase_profile_counters);
#endif
    for (op = 0u; op < sizeof s_direct_script / sizeof s_direct_script[0];
            ++op) {
        const direct_op *o = &s_direct_script[op];
        const direct_spec *spec = &s_direct_specs[o->spec];
        guest_gl_addr names[16];
        guest_gl_int locations[16];
        uint32_t i;

        for (i = 0u; i < 16u; ++i) {
            names[i] = (guest_gl_addr)(uintptr_t)s_direct_names[i];
            locations[i] = spec->locations[i];
        }
        switch (o->kind) {
        case 'u':
            backend->glUseProgram(o->spec);
            break;
        case 'd':
            backend->glDrawElements(ORACLE_GL_TRIANGLES, 6,
                                    ORACLE_GL_UNSIGNED_SHORT, 0x1000u);
            break;
        case 'E':
            CHECK(out->replays < 64u);
            if (direct) {
                CHECK(gl_vita_backend_attribs_replay_enable(
                    backend, spec->program, spec->count,
                    by_name ? names : NULL, locations, spec->components,
                    spec->stride, spec->base, &tokens) == 1);
            } else {
                guest_gl_addr base = spec->base;
                for (i = 0u; i < spec->count; ++i) {
                    if (by_name)
                        locations[i] = backend->glGetAttribLocation(
                            spec->program, names[i]);
                    backend->glEnableVertexAttribArray(
                        (guest_gl_uint)locations[i]);
                    backend->glVertexAttribPointer(
                        (guest_gl_uint)locations[i],
                        (guest_gl_int)spec->components[i], ORACLE_GL_FLOAT,
                        0u, spec->stride, base);
                    base += (guest_gl_addr)spec->components[i] * 4u;
                }
            }
            memcpy(out->locations[out->replays++], locations,
                   sizeof locations);
            break;
        case 'D':
            CHECK(out->replays < 64u);
            if (direct) {
                CHECK(gl_vita_backend_attribs_replay_disable(
                    backend, spec->program, spec->count,
                    by_name ? names : NULL, locations, &tokens) == 1);
            } else {
                for (i = 0u; i < spec->count; ++i) {
                    if (by_name)
                        locations[i] = backend->glGetAttribLocation(
                            spec->program, names[i]);
                    backend->glDisableVertexAttribArray(
                        (guest_gl_uint)locations[i]);
                }
            }
            memcpy(out->locations[out->replays++], locations,
                   sizeof locations);
            break;
        default:
            CHECK(0);
        }
    }
    CHECK(!s_direct_trace_overflow);
    memcpy(out->trace, s_direct_trace, sizeof out->trace);
    out->typed_bytes = gl_vita_backend_oracle_typed_state(
        out->typed, sizeof out->typed);
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
    out->pending = gl_vita_backend_attrib_pending();
    out->mask = s_native_attrib_mask;
#endif
#if defined(ISAAC_VITA_PHASE_PROFILE)
    out->counters = g_isaac_vita_gl_phase_profile_counters;
#endif
    return 0;
}

static int direct_snapshots_equal(const direct_snapshot *a,
                                  const direct_snapshot *b)
{
    return strcmp(a->trace, b->trace) == 0 &&
        a->typed_bytes == b->typed_bytes &&
        memcmp(a->typed, b->typed, a->typed_bytes) == 0 &&
        a->pending == b->pending && a->mask == b->mask &&
        a->replays == b->replays &&
        memcmp(a->locations, b->locations, sizeof a->locations) == 0
#if defined(ISAAC_VITA_PHASE_PROFILE)
        && memcmp(&a->counters, &b->counters, sizeof a->counters) == 0
#endif
        ;
}

static int oracle_test_attrib_direct_state(void)
{
    int by_name;
    unsigned draws = 0u, pointers = 0u;
    const char *p;

    for (by_name = 0; by_name < 2; ++by_name) {
        CHECK(direct_run(0, by_name, &s_direct_percall) == 0);
        CHECK(direct_run(1, by_name, &s_direct_batch) == 0);
        if (!direct_snapshots_equal(&s_direct_percall, &s_direct_batch)) {
            fprintf(stderr, "direct-state mismatch (by_name=%d)\n--- per-call\n%s\n--- batch\n%s\n",
                    by_name, s_direct_percall.trace, s_direct_batch.trace);
            CHECK(0);
        }
        /* The script did reach vitaGL: its ten draws and the pointer writes
         * (70 without a typed-state pointer hit) appear in the trace. */
        draws = 0u;
        pointers = 0u;
        for (p = s_direct_batch.trace; *p; ++p) {
            if (strncmp(p, "d4,6,1403,", 10u) == 0) ++draws;
            if (*p == 'P') ++pointers;
        }
        CHECK(draws == 10u && pointers >= 40u && pointers <= 70u);
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
        CHECK(s_direct_batch.typed_bytes > 0u);
#endif
    }

    /* Declines: nothing happens. */
    {
        const guest_gl_backend *backend;
        guest_gl_backend foreign;
        IsaacVitaAttribReplayTokens tokens = { 1u, 2u, 3u, 0u };
        guest_gl_int locations[17] = { 0 };
        uint8_t components[17] = { 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u,
                                   1u, 1u, 1u, 1u, 1u, 1u, 1u };
        uint8_t bad_components[2] = { 0u, 5u };
        direct_snapshot before, after;

        CHECK(direct_run(1, 0, &before) == 0);
        backend = guest_gl_installed_backend();
        CHECK(gl_vita_backend_attribs_replay_enable(
            backend, 7u, 17u, NULL, locations, components, 16, 0x1000u,
            &tokens) == 0);
        CHECK(gl_vita_backend_attribs_replay_disable(
            backend, 7u, 17u, NULL, locations, &tokens) == 0);
        CHECK(gl_vita_backend_attribs_replay_enable(
            backend, 7u, 2u, NULL, locations, bad_components, 16, 0x1000u,
            &tokens) == 0);
        bad_components[0] = 1u;
        CHECK(gl_vita_backend_attribs_replay_enable(
            backend, 7u, 2u, NULL, locations, bad_components, 16, 0x1000u,
            &tokens) == 0);
        CHECK(gl_vita_backend_attribs_replay_enable(
            backend, 7u, 2u, NULL, NULL, components, 16, 0x1000u,
            &tokens) == 0);
        CHECK(gl_vita_backend_attribs_replay_enable(
            backend, 7u, 2u, NULL, locations, NULL, 16, 0x1000u,
            &tokens) == 0);
        CHECK(gl_vita_backend_attribs_replay_enable(
            backend, 7u, 2u, NULL, locations, components, 16, 0x1000u,
            NULL) == 0);
        CHECK(gl_vita_backend_attribs_replay_disable(
            backend, 7u, 2u, NULL, NULL, &tokens) == 0);
        CHECK(gl_vita_backend_attribs_replay_disable(
            backend, 7u, 2u, NULL, locations, NULL) == 0);
        CHECK(gl_vita_backend_attribs_replay_enable(
            NULL, 7u, 2u, NULL, locations, components, 16, 0x1000u,
            &tokens) == 0);
        /* A table whose members are not this backend's wrappers (the
         * replay oracle's recording backend, a partial bring-up table). */
        foreign = *backend;
        foreign.glVertexAttribPointer = NULL;
        CHECK(gl_vita_backend_attribs_replay_enable(
            &foreign, 7u, 2u, NULL, locations, components, 16, 0x1000u,
            &tokens) == 0);
        foreign = *backend;
        foreign.glEnableVertexAttribArray = oracle_backend_fake_toggle;
        CHECK(gl_vita_backend_attribs_replay_enable(
            &foreign, 7u, 2u, NULL, locations, components, 16, 0x1000u,
            &tokens) == 0);
        foreign = *backend;
        foreign.glDisableVertexAttribArray = oracle_backend_fake_toggle;
        CHECK(gl_vita_backend_attribs_replay_disable(
            &foreign, 7u, 2u, NULL, locations, &tokens) == 0);
        foreign = *backend;
        foreign.glGetAttribLocation = oracle_backend_fake_location;
        CHECK(gl_vita_backend_attribs_replay_disable(
            &foreign, 7u, 2u, NULL, locations, &tokens) == 0);
        CHECK(gl_vita_backend_attribs_replay_enable(
            &foreign, 7u, 2u, NULL, locations, components, 16, 0x1000u,
            &tokens) == 0);
        /* Untouched: trace, typed state, pending, counters. */
        memcpy(after.trace, s_direct_trace, sizeof after.trace);
        after.typed_bytes = gl_vita_backend_oracle_typed_state(
            after.typed, sizeof after.typed);
        after.pending = before.pending;
        after.mask = before.mask;
        after.replays = before.replays;
        memcpy(after.locations, before.locations, sizeof after.locations);
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
        after.pending = gl_vita_backend_attrib_pending();
        after.mask = s_native_attrib_mask;
#endif
#if defined(ISAAC_VITA_PHASE_PROFILE)
        after.counters = g_isaac_vita_gl_phase_profile_counters;
#endif
        CHECK(direct_snapshots_equal(&before, &after));
        CHECK(oracle_backend_fake_calls == 0u);
    }
    gl_vita_backend_uninstall();
    puts("attribute direct state: per-call and batch traces/state/counters equal; declines untouched PASS");
    return 0;
}
#endif

#if defined(ISAAC_VITA_GL_FILL_CENSUS)
/* GL fill census (ISAAC_VITA_GL_FILL_CENSUS): the shim projects every guest
 * draw through its Position/Transform/viewport shadows.  Synthetic quads in
 * pixel space through an orthographic Transform pin the kilo-pixel
 * arithmetic (full screen 960x540 = 506 kpx, half off-screen 253, rotated
 * 100x100 square 10, NaN vertex -> bad, 5x-wide quad -> big), every
 * eligibility miss, the program/blend classes, the viewport shadow across
 * typed-cache hits, the pass ordinal model (attach/readpixels/delete/present
 * boundaries, attachment size via the level-0 glTexImage2D table) and, under
 * the DUMP define, the one-frame draw list emitted only from take-window. */
#define ORACLE_FILL_TOKEN_GET_ATTRIB 0x7e307ce3u
#define ORACLE_FILL_TOKEN_CLEAR 0x7e194b2bu
#define ORACLE_FILL_TOKEN_ENABLE 0x7ed43cfdu
#define ORACLE_FILL_TOKEN_READ_PIXELS 0x7e038504u
#define ORACLE_FILL_GL_BLEND 0x00000be2u
#define ORACLE_FILL_GL_ZERO 0u
#define ORACLE_FILL_GL_LINES 0x00000001u
#define ORACLE_FILL_GL_SHORT 0x00001402u
#define ORACLE_FILL_GL_UNSIGNED_INT 0x00001405u
#define ORACLE_FILL_GL_COLOR_ATTACHMENT0 0x00008ce0u
#define ORACLE_FILL_GL_COLOR_BUFFER_BIT 0x00004000u
#define ORACLE_FILL_GL_DEPTH_BUFFER_BIT 0x00000100u
#define ORACLE_FILL_CANONICAL_RVA 0x0056039du
#define ORACLE_FILL_STRIDE 88u
#define ORACLE_FILL_FLOATS_PER_VERTEX (ORACLE_FILL_STRIDE / 4u)

static float s_fill_vertices[4u * ORACLE_FILL_FLOATS_PER_VERTEX];
static const uint16_t s_fill_indices[6] = { 0u, 2u, 1u, 1u, 2u, 3u };
static const uint32_t s_fill_indices32[6] = { 0u, 2u, 1u, 1u, 2u, 3u };
static float s_fill_transform[16];

#if defined(ISAAC_VITA_GL_FILL_CENSUS_DUMP) && \
    !defined(ISAAC_VITA_FXRAY_ALPHA_MASK) && \
    !defined(ISAAC_VITA_FBO_RASTER_SCALE)
#define ORACLE_FILL_LOG_CAPACITY 16u
static unsigned s_fill_log_calls;
static size_t s_fill_log_max_length;
static char s_fill_logs[ORACLE_FILL_LOG_CAPACITY][512];

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;
    char line[1024];
    int length;

    va_start(arguments, format);
    length = vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    line[sizeof line - 1u] = '\0';
    if (length > 0 && (size_t)length > s_fill_log_max_length)
        s_fill_log_max_length = (size_t)length;
    if (s_fill_log_calls < ORACLE_FILL_LOG_CAPACITY) {
        /* Bounded copy (not snprintf "%s": gcc -Wformat-truncation
         * objects to the 1024 -> 512 byte narrowing). */
        char *slot = s_fill_logs[s_fill_log_calls];
        size_t copy = strlen(line);

        if (copy > sizeof s_fill_logs[0] - 1u)
            copy = sizeof s_fill_logs[0] - 1u;
        memcpy(slot, line, copy);
        slot[copy] = '\0';
    }
    ++s_fill_log_calls;
}
#endif

/* Four vertices of an axis-aligned quad in pixel space at float offset 0 of
 * each 88-byte vertex, in the canonical (0,2,1)(1,2,3) winding order. */
static void oracle_fill_quad(float x0, float y0, float x1, float y1)
{
    memset(s_fill_vertices, 0, sizeof s_fill_vertices);
    s_fill_vertices[0u * ORACLE_FILL_FLOATS_PER_VERTEX] = x0;
    s_fill_vertices[0u * ORACLE_FILL_FLOATS_PER_VERTEX + 1u] = y0;
    s_fill_vertices[1u * ORACLE_FILL_FLOATS_PER_VERTEX] = x1;
    s_fill_vertices[1u * ORACLE_FILL_FLOATS_PER_VERTEX + 1u] = y0;
    s_fill_vertices[2u * ORACLE_FILL_FLOATS_PER_VERTEX] = x0;
    s_fill_vertices[2u * ORACLE_FILL_FLOATS_PER_VERTEX + 1u] = y1;
    s_fill_vertices[3u * ORACLE_FILL_FLOATS_PER_VERTEX] = x1;
    s_fill_vertices[3u * ORACLE_FILL_FLOATS_PER_VERTEX + 1u] = y1;
}

/* Pixel-space orthographic Transform (column major, y down), as the guest's
 * 2D sprite pipeline: x' = 2x/w - 1, y' = 1 - 2y/h. */
static void oracle_fill_ortho(float width, float height)
{
    memset(s_fill_transform, 0, sizeof s_fill_transform);
    s_fill_transform[0] = 2.0f / width;
    s_fill_transform[5] = -2.0f / height;
    s_fill_transform[10] = 1.0f;
    s_fill_transform[12] = -1.0f;
    s_fill_transform[13] = 1.0f;
    s_fill_transform[15] = 1.0f;
}

static int oracle_fill_use_program(
    CPU *cpu, uint32_t stack_top, uint32_t program)
{
    uint32_t arguments[1];

    arguments[0] = program;
    return oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_USE_PROGRAM, arguments, 1u);
}

static int oracle_fill_attrib_location(
    CPU *cpu, uint32_t stack_top, uint32_t program, const char *name)
{
    uint32_t arguments[2];

    arguments[0] = program;
    arguments[1] = (uint32_t)(uintptr_t)name;
    return oracle_dispatch(
        cpu, stack_top, ORACLE_FILL_TOKEN_GET_ATTRIB, arguments, 2u);
}

static int oracle_fill_uniform_location(
    CPU *cpu, uint32_t stack_top, uint32_t program, const char *name)
{
    uint32_t arguments[2];

    arguments[0] = program;
    arguments[1] = (uint32_t)(uintptr_t)name;
    return oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_GET_UNIFORM, arguments, 2u);
}

static int oracle_fill_uniform_matrix(
    CPU *cpu, uint32_t stack_top, int32_t location, uint32_t transpose)
{
    uint32_t arguments[4];

    arguments[0] = (uint32_t)location;
    arguments[1] = 1u;
    arguments[2] = transpose;
    arguments[3] = (uint32_t)(uintptr_t)s_fill_transform;
    return oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_UNIFORM_MATRIX4FV, arguments, 4u);
}

static int oracle_fill_attrib_pointer(
    CPU *cpu, uint32_t stack_top, uint32_t index, uint32_t size,
    uint32_t type, uint32_t stride, uint32_t pointer)
{
    uint32_t arguments[6];

    arguments[0] = index;
    arguments[1] = size;
    arguments[2] = type;
    arguments[3] = 0u;
    arguments[4] = stride;
    arguments[5] = pointer;
    return oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_ATTRIB_POINTER, arguments, 6u);
}

static int oracle_fill_attrib_toggle(
    CPU *cpu, uint32_t stack_top, uint32_t index, int enabled)
{
    uint32_t arguments[1];

    arguments[0] = index;
    return oracle_dispatch(
        cpu, stack_top,
        enabled ? ORACLE_TOKEN_ENABLE_ATTRIB : ORACLE_TOKEN_DISABLE_ATTRIB,
        arguments, 1u);
}

static int oracle_fill_blend(
    CPU *cpu, uint32_t stack_top, uint32_t source, uint32_t destination)
{
    uint32_t arguments[4];

    arguments[0] = source;
    arguments[1] = destination;
    arguments[2] = source;
    arguments[3] = destination;
    return oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BLEND_SEPARATE, arguments, 4u);
}

static int oracle_fill_enable(CPU *cpu, uint32_t stack_top, uint32_t cap)
{
    uint32_t arguments[1];

    arguments[0] = cap;
    return oracle_dispatch(
        cpu, stack_top, ORACLE_FILL_TOKEN_ENABLE, arguments, 1u);
}

static int oracle_fill_draw(
    CPU *cpu, uint32_t stack_top, uint32_t mode, uint32_t count,
    uint32_t type, uint32_t indices, uint32_t return_word)
{
    uint32_t arguments[4];

    arguments[0] = mode;
    arguments[1] = count;
    arguments[2] = type;
    arguments[3] = indices;
    return oracle_dispatch_with_return(
        cpu, stack_top, ORACLE_TOKEN_DRAW_ELEMENTS, arguments, 4u,
        return_word);
}

/* The ordinary guest quad: six GL_UNSIGNED_SHORT indices read from memory. */
static int oracle_fill_draw_quad(CPU *cpu, uint32_t stack_top)
{
    return oracle_fill_draw(
        cpu, stack_top, ORACLE_GL_TRIANGLES, 6u, ORACLE_GL_UNSIGNED_SHORT,
        (uint32_t)(uintptr_t)s_fill_indices, 0xaabbccddu);
}

static int oracle_fill_clear(CPU *cpu, uint32_t stack_top, uint32_t mask)
{
    uint32_t arguments[1];

    arguments[0] = mask;
    return oracle_dispatch(
        cpu, stack_top, ORACLE_FILL_TOKEN_CLEAR, arguments, 1u);
}

static int oracle_fill_bind_framebuffer(
    CPU *cpu, uint32_t stack_top, uint32_t framebuffer)
{
    return oracle_bind_framebuffer(
        cpu, stack_top, ORACLE_GL_FRAMEBUFFER, framebuffer);
}

static int oracle_fill_attach(
    CPU *cpu, uint32_t stack_top, uint32_t texture)
{
    uint32_t arguments[5];

    arguments[0] = ORACLE_GL_FRAMEBUFFER;
    arguments[1] = ORACLE_FILL_GL_COLOR_ATTACHMENT0;
    arguments[2] = ORACLE_GL_TEXTURE_2D;
    arguments[3] = texture;
    arguments[4] = 0u;
    return oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_FRAMEBUFFER_TEXTURE, arguments, 5u);
}

static int oracle_fill_bind_texture(
    CPU *cpu, uint32_t stack_top, uint32_t texture)
{
    uint32_t arguments[2];

    arguments[0] = ORACLE_GL_TEXTURE_2D;
    arguments[1] = texture;
    return oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_BIND_TEXTURE, arguments, 2u);
}

static int oracle_fill_tex_image(
    CPU *cpu, uint32_t stack_top, uint32_t width, uint32_t height)
{
    uint32_t arguments[9];

    arguments[0] = ORACLE_GL_TEXTURE_2D;
    arguments[1] = 0u;
    arguments[2] = ORACLE_GL_RGBA;
    arguments[3] = width;
    arguments[4] = height;
    arguments[5] = 0u;
    arguments[6] = ORACLE_GL_RGBA;
    arguments[7] = ORACLE_GL_UNSIGNED_BYTE;
    arguments[8] = 0u;
    return oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_TEX_IMAGE, arguments, 9u);
}

static int oracle_fill_read_pixels(CPU *cpu, uint32_t stack_top)
{
    static uint8_t pixels[4];
    uint32_t arguments[7];

    arguments[0] = 0u;
    arguments[1] = 0u;
    arguments[2] = 1u;
    arguments[3] = 1u;
    arguments[4] = ORACLE_GL_RGBA;
    arguments[5] = ORACLE_GL_UNSIGNED_BYTE;
    arguments[6] = (uint32_t)(uintptr_t)pixels;
    return oracle_dispatch(
        cpu, stack_top, ORACLE_FILL_TOKEN_READ_PIXELS, arguments, 7u);
}

static int oracle_fill_delete_framebuffer(
    CPU *cpu, uint32_t stack_top, uint32_t *name)
{
    uint32_t arguments[2];

    arguments[0] = 1u;
    arguments[1] = (uint32_t)(uintptr_t)name;
    return oracle_dispatch(
        cpu, stack_top, ORACLE_TOKEN_DELETE_FRAMEBUFFERS, arguments, 2u);
}

/* Draw, take the window and check the common single-draw shape. */
static int oracle_fill_single(
    CPU *cpu, uint32_t stack_top, IsaacVitaGlFillCensus *census)
{
    if (!oracle_fill_draw_quad(cpu, stack_top))
        return 0;
    gl_vita_backend_fill_census_take_window(census, 0u, 0u);
    return census->draws == 1u && census->pass_draws[4] == 1u;
}

static int oracle_test_fill_census(CPU *cpu, uint32_t stack_top)
{
    IsaacVitaGlFillCensus census;
    uint32_t framebuffer_name = 7u;
    uint32_t vertices = (uint32_t)(uintptr_t)s_fill_vertices;
    union {
        uint32_t bits;
        float value;
    } quiet_nan;

    quiet_nan.bits = 0x7fc00000u;
    gl_vita_backend_uninstall();
    CHECK(gl_vita_backend_install());
    gl_vita_backend_fill_census_take_window(&census, 0u, 0u);
    CHECK(census.draws == 0u && census.kpx_clipped == 0u);

    /* Program 7 carries the coloroffset layout (PixelationAmount queried),
     * program 8 is plain.  The facade answers 0 for every attribute name
     * and s_native_uniform_location for Transform. */
    CHECK(oracle_fill_use_program(cpu, stack_top, 7u));
    CHECK(oracle_fill_attrib_location(cpu, stack_top, 7u, "Position"));
    CHECK(oracle_fill_attrib_location(
        cpu, stack_top, 7u, "PixelationAmount"));
    CHECK(oracle_fill_attrib_location(cpu, stack_top, 7u, "TexCoord"));
    CHECK(oracle_fill_uniform_location(cpu, stack_top, 7u, "Transform"));
    CHECK(oracle_fill_uniform_location(cpu, stack_top, 7u, "Texture0"));
    CHECK(oracle_fill_attrib_location(cpu, stack_top, 8u, "Position"));
    CHECK(oracle_fill_uniform_location(cpu, stack_top, 8u, "Transform"));
    oracle_fill_quad(0.0f, 0.0f, 960.0f, 540.0f);
    CHECK(oracle_fill_attrib_pointer(
        cpu, stack_top, 0u, 3u, ORACLE_GL_FLOAT, ORACLE_FILL_STRIDE,
        vertices));
    CHECK(oracle_fill_attrib_toggle(cpu, stack_top, 0u, 1));

    /* No glViewport yet: counted, not measured. */
    CHECK(oracle_fill_single(cpu, stack_top, &census));
    CHECK(census.triangles == 2u && census.miss == 1u &&
          census.kpx_clipped == 0u && census.kpx_unclipped == 0u);
    CHECK(census.viewport_width == 0u && census.viewport_changes == 0u);

    /* Viewport known, Transform still unknown: counted, not measured. */
    CHECK(oracle_set_viewport(cpu, stack_top, 0, 0, 960, 540));
    CHECK(oracle_fill_single(cpu, stack_top, &census));
    CHECK(census.triangles == 2u && census.miss == 1u &&
          census.kpx_clipped == 0u);
    CHECK(census.viewport_width == 960u && census.viewport_height == 540u &&
          census.viewport_changes == 1u);

    /* Full-screen quad through the orthographic Transform: 960*540/1024 =
     * 506.25 -> 506 kpx, no anomaly, program class e, blend class d. */
    oracle_fill_ortho(960.0f, 540.0f);
    CHECK(oracle_fill_uniform_matrix(
        cpu, stack_top, s_native_uniform_location, 0u));
    CHECK(oracle_fill_enable(cpu, stack_top, ORACLE_FILL_GL_BLEND));
    CHECK(oracle_fill_blend(
        cpu, stack_top, ORACLE_GL_SRC_ALPHA, ORACLE_GL_ONE_MINUS_SRC_ALPHA));
    CHECK(oracle_fill_single(cpu, stack_top, &census));
    CHECK(census.triangles == 2u && census.miss == 0u);
    CHECK(census.kpx_unclipped == 506u && census.kpx_clipped == 506u &&
          census.max_draw_kpx == 506u);
    CHECK(census.big == 0u && census.bad == 0u && census.synthesized == 0u &&
          census.projective == 0u);
    CHECK(census.prog_kpx[0] == 506u && census.prog_kpx[1] == 0u);
    CHECK(census.blend_kpx[0] == 0u && census.blend_kpx[1] == 506u &&
          census.blend_kpx[2] == 0u);
    CHECK(census.pass_kpx[4] == 506u && census.pass_clears[4] == 0u);
    CHECK(census.kpx_clear == 0u && census.viewport_changes == 0u);
    /* 32x32 tiles of each triangle's clipped box (the whole viewport):
     * 30..31 columns by 17 rows, twice (float edges decide the last column). */
    CHECK(census.tiles >= 2u * 30u * 17u && census.tiles <= 2u * 31u * 17u);

    /* Redundant state (typed-cache hits, redundancy-cache paths): the
     * shadows already hold these values, the draw measures the same. */
    CHECK(oracle_fill_attrib_pointer(
        cpu, stack_top, 0u, 3u, ORACLE_GL_FLOAT, ORACLE_FILL_STRIDE,
        vertices));
    CHECK(oracle_fill_attrib_toggle(cpu, stack_top, 0u, 1));
    CHECK(oracle_set_viewport(cpu, stack_top, 0, 0, 960, 540));
    CHECK(oracle_fill_uniform_matrix(
        cpu, stack_top, s_native_uniform_location, 0u));
    CHECK(oracle_fill_blend(
        cpu, stack_top, ORACLE_GL_SRC_ALPHA, ORACLE_GL_ONE_MINUS_SRC_ALPHA));
    CHECK(oracle_fill_single(cpu, stack_top, &census));
    CHECK(census.kpx_clipped == 506u && census.miss == 0u &&
          census.viewport_changes == 0u);

    /* Transposed upload of the transposed matrix is the same Transform. */
    {
        float transposed[16];
        uint32_t row;
        uint32_t column;

        for (row = 0u; row < 4u; ++row)
            for (column = 0u; column < 4u; ++column)
                transposed[row * 4u + column] =
                    s_fill_transform[column * 4u + row];
        memcpy(s_fill_transform, transposed, sizeof transposed);
        CHECK(oracle_fill_uniform_matrix(
            cpu, stack_top, s_native_uniform_location, 1u));
        CHECK(oracle_fill_single(cpu, stack_top, &census));
        CHECK(census.kpx_clipped == 506u && census.miss == 0u);
        oracle_fill_ortho(960.0f, 540.0f);
        CHECK(oracle_fill_uniform_matrix(
            cpu, stack_top, s_native_uniform_location, 0u));
    }

    /* 32-bit indices are read as well. */
    CHECK(oracle_fill_draw(
        cpu, stack_top, ORACLE_GL_TRIANGLES, 6u, ORACLE_FILL_GL_UNSIGNED_INT,
        (uint32_t)(uintptr_t)s_fill_indices32, 0xaabbccddu));
    gl_vita_backend_fill_census_take_window(&census, 0u, 0u);
    CHECK(census.draws == 1u && census.kpx_clipped == 506u &&
          census.miss == 0u);

#if defined(ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY)
    /* Canonical zero-copy draw: the index array is never read; the census
     * synthesizes base+{0,2,1,1,2,3} and measures the same quad. */
    s_native_canonical_quad_result = 1u;
    CHECK(oracle_fill_draw(
        cpu, stack_top, ORACLE_GL_TRIANGLES, 6u, ORACLE_GL_UNSIGNED_SHORT,
        0x1000u, ORACLE_FILL_CANONICAL_RVA));
    gl_vita_backend_fill_census_take_window(&census, 0u, 0u);
    CHECK(census.draws == 1u && census.synthesized == 1u &&
          census.kpx_clipped == 506u && census.miss == 0u);
#endif

    /* Half off-screen to the right: unclipped 506, clipped 253. */
    oracle_fill_quad(480.0f, 0.0f, 1440.0f, 540.0f);
    CHECK(oracle_fill_single(cpu, stack_top, &census));
    CHECK(census.kpx_unclipped == 506u && census.kpx_clipped == 253u &&
          census.max_draw_kpx == 253u && census.big == 0u);

    /* Entirely off-screen: unclipped 506, clipped 0, no tiles. */
    oracle_fill_quad(1000.0f, 0.0f, 1960.0f, 540.0f);
    CHECK(oracle_fill_single(cpu, stack_top, &census));
    CHECK(census.kpx_unclipped == 506u && census.kpx_clipped == 0u &&
          census.tiles == 0u && census.max_draw_kpx == 0u);

    /* Rotated 100x100 square (diamond): area 9800 -> 10 kpx either way. */
    memset(s_fill_vertices, 0, sizeof s_fill_vertices);
    s_fill_vertices[0u * ORACLE_FILL_FLOATS_PER_VERTEX] = 480.0f;
    s_fill_vertices[0u * ORACLE_FILL_FLOATS_PER_VERTEX + 1u] = 200.0f;
    s_fill_vertices[1u * ORACLE_FILL_FLOATS_PER_VERTEX] = 550.0f;
    s_fill_vertices[1u * ORACLE_FILL_FLOATS_PER_VERTEX + 1u] = 270.0f;
    s_fill_vertices[2u * ORACLE_FILL_FLOATS_PER_VERTEX] = 410.0f;
    s_fill_vertices[2u * ORACLE_FILL_FLOATS_PER_VERTEX + 1u] = 270.0f;
    s_fill_vertices[3u * ORACLE_FILL_FLOATS_PER_VERTEX] = 480.0f;
    s_fill_vertices[3u * ORACLE_FILL_FLOATS_PER_VERTEX + 1u] = 340.0f;
    CHECK(oracle_fill_single(cpu, stack_top, &census));
    CHECK(census.kpx_unclipped == 10u && census.kpx_clipped == 10u);

    /* Five viewports wide: big on both triangles, unclipped 2531, clipped
     * back to the viewport's 506. */
    oracle_fill_quad(0.0f, 0.0f, 4800.0f, 540.0f);
    CHECK(oracle_fill_single(cpu, stack_top, &census));
    CHECK(census.big == 2u && census.kpx_unclipped == 2531u &&
          census.kpx_clipped == 506u && census.bad == 0u);

    /* NaN vertex 0: its triangle is bad, the other half still measures. */
    oracle_fill_quad(0.0f, 0.0f, 960.0f, 540.0f);
    s_fill_vertices[0] = quiet_nan.value;
    CHECK(oracle_fill_single(cpu, stack_top, &census));
    CHECK(census.bad == 1u && census.kpx_clipped == 253u &&
          census.triangles == 2u && census.miss == 0u);
    oracle_fill_quad(0.0f, 0.0f, 960.0f, 540.0f);

    /* Blend classes: additive (dst ONE) and other (dst ZERO). */
    CHECK(oracle_fill_blend(cpu, stack_top, ORACLE_GL_ONE, ORACLE_GL_ONE));
    CHECK(oracle_fill_single(cpu, stack_top, &census));
    CHECK(census.blend_kpx[0] == 506u && census.blend_kpx[1] == 0u &&
          census.blend_kpx[2] == 0u);
    CHECK(oracle_fill_blend(cpu, stack_top, ORACLE_GL_ONE, ORACLE_FILL_GL_ZERO));
    CHECK(oracle_fill_single(cpu, stack_top, &census));
    CHECK(census.blend_kpx[0] == 0u && census.blend_kpx[1] == 0u &&
          census.blend_kpx[2] == 506u);

    /* Program class o: program 8 with its own Transform upload. */
    CHECK(oracle_fill_use_program(cpu, stack_top, 8u));
    CHECK(oracle_fill_single(cpu, stack_top, &census));
    CHECK(census.miss == 1u && census.kpx_clipped == 0u);
    CHECK(oracle_fill_uniform_matrix(
        cpu, stack_top, s_native_uniform_location, 0u));
    CHECK(oracle_fill_single(cpu, stack_top, &census));
    CHECK(census.prog_kpx[0] == 0u && census.prog_kpx[1] == 506u &&
          census.kpx_clipped == 506u);

    /* Viewport shadow: a quarter viewport scales the same quad to
     * 480*270/1024 = 126.56 -> 127 kpx and counts one change. */
    CHECK(oracle_set_viewport(cpu, stack_top, 0, 0, 480, 270));
    CHECK(oracle_fill_single(cpu, stack_top, &census));
    CHECK(census.kpx_clipped == 127u && census.viewport_width == 480u &&
          census.viewport_height == 270u && census.viewport_changes == 1u);
    /* A negative size is a GL error and leaves the shadow alone. */
    CHECK(oracle_set_viewport(cpu, stack_top, 0, 0, -1, 270));
    CHECK(oracle_fill_single(cpu, stack_top, &census));
    CHECK(census.kpx_clipped == 127u && census.viewport_changes == 0u);
    CHECK(oracle_set_viewport(cpu, stack_top, 0, 0, 960, 540));

    /* Projective Transform (m15 = 2 halves every coordinate about the
     * centre): the full quad covers 480x270 -> 127 kpx, proj counted. */
    s_fill_transform[15] = 2.0f;
    CHECK(oracle_fill_uniform_matrix(
        cpu, stack_top, s_native_uniform_location, 0u));
    CHECK(oracle_fill_single(cpu, stack_top, &census));
    CHECK(census.projective == 1u && census.kpx_clipped == 127u &&
          census.viewport_changes == 1u);
    oracle_fill_ortho(960.0f, 540.0f);
    CHECK(oracle_fill_uniform_matrix(
        cpu, stack_top, s_native_uniform_location, 0u));

    /* Eligibility misses: disabled attribute, size 4, GL_SHORT, a stride
     * that is not a multiple of 4, GL_LINES, count % 3, a null index array.
     * Each is one draw; only triangle lists add to tri. */
    CHECK(oracle_fill_attrib_toggle(cpu, stack_top, 0u, 0));
    CHECK(oracle_fill_single(cpu, stack_top, &census));
    CHECK(census.miss == 1u && census.triangles == 2u &&
          census.kpx_clipped == 0u);
    CHECK(oracle_fill_attrib_toggle(cpu, stack_top, 0u, 1));
    CHECK(oracle_fill_attrib_pointer(
        cpu, stack_top, 0u, 4u, ORACLE_GL_FLOAT, ORACLE_FILL_STRIDE,
        vertices));
    CHECK(oracle_fill_single(cpu, stack_top, &census));
    CHECK(census.miss == 1u && census.kpx_clipped == 0u);
    CHECK(oracle_fill_attrib_pointer(
        cpu, stack_top, 0u, 3u, ORACLE_FILL_GL_SHORT, ORACLE_FILL_STRIDE,
        vertices));
    CHECK(oracle_fill_single(cpu, stack_top, &census));
    CHECK(census.miss == 1u && census.kpx_clipped == 0u);
    CHECK(oracle_fill_attrib_pointer(
        cpu, stack_top, 0u, 3u, ORACLE_GL_FLOAT, 6u, vertices));
    CHECK(oracle_fill_single(cpu, stack_top, &census));
    CHECK(census.miss == 1u && census.kpx_clipped == 0u);
    CHECK(oracle_fill_attrib_pointer(
        cpu, stack_top, 0u, 3u, ORACLE_GL_FLOAT, ORACLE_FILL_STRIDE,
        vertices));
    CHECK(oracle_fill_draw(
        cpu, stack_top, ORACLE_FILL_GL_LINES, 6u, ORACLE_GL_UNSIGNED_SHORT,
        (uint32_t)(uintptr_t)s_fill_indices, 0xaabbccddu));
    gl_vita_backend_fill_census_take_window(&census, 0u, 0u);
    CHECK(census.draws == 1u && census.triangles == 0u &&
          census.miss == 1u);
    CHECK(oracle_fill_draw(
        cpu, stack_top, ORACLE_GL_TRIANGLES, 5u, ORACLE_GL_UNSIGNED_SHORT,
        (uint32_t)(uintptr_t)s_fill_indices, 0xaabbccddu));
    gl_vita_backend_fill_census_take_window(&census, 0u, 0u);
    CHECK(census.draws == 1u && census.triangles == 0u &&
          census.miss == 1u);
    CHECK(oracle_fill_draw(
        cpu, stack_top, ORACLE_GL_TRIANGLES, 6u, ORACLE_GL_UNSIGNED_SHORT,
        0u, 0xaabbccddu));
    gl_vita_backend_fill_census_take_window(&census, 0u, 0u);
    CHECK(census.draws == 1u && census.triangles == 2u &&
          census.miss == 1u);
    /* Back to a measured draw after the miss run. */
    CHECK(oracle_fill_single(cpu, stack_top, &census));
    CHECK(census.miss == 0u && census.kpx_clipped == 506u);

    /* Pass ordinal model: offscreen passes 0..3+ since present, split by a
     * colour attach to the bound pass framebuffer, glReadPixels and delete;
     * display draws in the fifth bucket; the attachment size follows the
     * level-0 glTexImage2D of the attached texture. */
    gl_vita_backend_fill_census_present();
    CHECK(oracle_fill_bind_framebuffer(cpu, stack_top, 7u));
    CHECK(oracle_fill_bind_texture(cpu, stack_top, 5u));
    CHECK(oracle_fill_tex_image(cpu, stack_top, 512u, 512u));
    CHECK(oracle_fill_attach(cpu, stack_top, 5u));
    CHECK(oracle_fill_clear(cpu, stack_top, ORACLE_FILL_GL_COLOR_BUFFER_BIT));
    CHECK(oracle_fill_draw_quad(cpu, stack_top));
    CHECK(oracle_fill_bind_texture(cpu, stack_top, 6u));
    CHECK(oracle_fill_tex_image(cpu, stack_top, 1024u, 1024u));
    CHECK(oracle_fill_attach(cpu, stack_top, 6u));
    CHECK(oracle_fill_draw_quad(cpu, stack_top));
    CHECK(oracle_fill_read_pixels(cpu, stack_top));
    CHECK(oracle_fill_clear(cpu, stack_top, ORACLE_FILL_GL_COLOR_BUFFER_BIT));
    CHECK(oracle_fill_bind_framebuffer(cpu, stack_top, 0u));
    CHECK(oracle_fill_draw_quad(cpu, stack_top));
    CHECK(oracle_fill_bind_framebuffer(cpu, stack_top, 7u));
    CHECK(oracle_fill_draw_quad(cpu, stack_top));
    CHECK(oracle_fill_draw_quad(cpu, stack_top));
    CHECK(oracle_fill_delete_framebuffer(cpu, stack_top, &framebuffer_name));
    CHECK(oracle_fill_bind_framebuffer(cpu, stack_top, 0u));
    CHECK(oracle_fill_draw_quad(cpu, stack_top));
    CHECK(oracle_fill_bind_framebuffer(cpu, stack_top, 9u));
    CHECK(oracle_fill_clear(cpu, stack_top, ORACLE_FILL_GL_COLOR_BUFFER_BIT));
    gl_vita_backend_fill_census_present();
    CHECK(oracle_fill_bind_framebuffer(cpu, stack_top, 7u));
    CHECK(oracle_fill_draw_quad(cpu, stack_top));
    gl_vita_backend_fill_census_take_window(&census, 0u, 0u);
    CHECK(census.draws == 7u && census.triangles == 14u &&
          census.miss == 0u);
    CHECK(census.kpx_clipped == 7u * 506u && census.max_draw_kpx == 506u);
    CHECK(census.pass_draws[0] == 2u && census.pass_draws[1] == 1u &&
          census.pass_draws[2] == 0u && census.pass_draws[3] == 2u &&
          census.pass_draws[4] == 2u);
    CHECK(census.pass_kpx[0] == 1012u && census.pass_kpx[1] == 506u &&
          census.pass_kpx[2] == 0u && census.pass_kpx[3] == 1012u &&
          census.pass_kpx[4] == 1012u);
    CHECK(census.pass_clears[0] == 1u && census.pass_clears[1] == 0u &&
          census.pass_clears[2] == 1u && census.pass_clears[3] == 1u &&
          census.pass_clears[4] == 0u);
    /* 512*512/1024 + 1024*1024/1024; framebuffer 9 has no known target:
     * 0 kpx and one clear_unknown so the shortfall is visible. */
    CHECK(census.kpx_clear == 256u + 1024u);
    CHECK(census.clear_unknown == 1u);
    CHECK(census.attachment_width == 1024u &&
          census.attachment_height == 1024u);

    /* A display clear is the 960x544 surface: 510 kpx (not the viewport). */
    CHECK(oracle_fill_bind_framebuffer(cpu, stack_top, 0u));
    CHECK(oracle_fill_clear(cpu, stack_top, ORACLE_FILL_GL_COLOR_BUFFER_BIT));
    gl_vita_backend_fill_census_take_window(&census, 0u, 0u);
    CHECK(census.kpx_clear == 510u && census.pass_clears[4] == 1u &&
          census.draws == 0u && census.clear_unknown == 0u);

    /* Clears the elision absorbs never reach vitaGL and are not counted:
     * a depth-only clear on a tabled framebuffer is owed, the colour clear
     * that follows is native.  Without the elision both are native. */
    gl_vita_backend_fill_census_present();
    CHECK(oracle_fill_bind_framebuffer(cpu, stack_top, 7u));
    CHECK(oracle_fill_attach(cpu, stack_top, 6u));
    CHECK(oracle_fill_clear(cpu, stack_top, ORACLE_FILL_GL_DEPTH_BUFFER_BIT));
    CHECK(oracle_fill_clear(cpu, stack_top, ORACLE_FILL_GL_COLOR_BUFFER_BIT));
    gl_vita_backend_fill_census_take_window(&census, 0u, 0u);
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION)
    CHECK(census.pass_clears[0] == 1u && census.kpx_clear == 1024u);
#else
    CHECK(census.pass_clears[0] == 2u && census.kpx_clear == 2048u);
#endif
    CHECK(oracle_fill_bind_framebuffer(cpu, stack_top, 0u));

    /* An owed depth clear the elision materializes natively after the guest
     * bound away (a glTexImage2D forces it, gl_vita_fbo_owed_materialize
     * rebinds for the quad) is counted on the owing framebuffer, inside its
     * still-open pass, never on the bound one; without the elision the same
     * clear is native at once.  Either way: pass 0 = one draw + one clear of
     * the 1024x1024 attachment, disp = the display draw alone. */
    gl_vita_backend_fill_census_present();
    CHECK(oracle_fill_bind_framebuffer(cpu, stack_top, 7u));
    CHECK(oracle_fill_attach(cpu, stack_top, 6u));
    CHECK(oracle_fill_draw_quad(cpu, stack_top));
    CHECK(oracle_fill_clear(cpu, stack_top, ORACLE_FILL_GL_DEPTH_BUFFER_BIT));
    CHECK(oracle_fill_bind_framebuffer(cpu, stack_top, 0u));
    CHECK(oracle_fill_bind_texture(cpu, stack_top, 5u));
    CHECK(oracle_fill_tex_image(cpu, stack_top, 512u, 512u));
    CHECK(oracle_fill_draw_quad(cpu, stack_top));
    gl_vita_backend_fill_census_take_window(&census, 0u, 0u);
    CHECK(census.draws == 2u && census.kpx_clear == 1024u &&
          census.clear_unknown == 0u);
    CHECK(census.pass_draws[0] == 1u && census.pass_clears[0] == 1u &&
          census.pass_kpx[0] == 506u);
    CHECK(census.pass_draws[1] == 0u && census.pass_clears[1] == 0u);
    CHECK(census.pass_draws[4] == 1u && census.pass_clears[4] == 0u);

    /* An owed clear the elision drops at the scene end (a draw on another
     * framebuffer) never reaches vitaGL and is not counted (ph120.e 'd');
     * without the elision it is a native clear like any other. */
    gl_vita_backend_fill_census_present();
    CHECK(oracle_fill_bind_framebuffer(cpu, stack_top, 7u));
    CHECK(oracle_fill_clear(cpu, stack_top, ORACLE_FILL_GL_DEPTH_BUFFER_BIT));
    CHECK(oracle_fill_bind_framebuffer(cpu, stack_top, 0u));
    CHECK(oracle_fill_draw_quad(cpu, stack_top));
    gl_vita_backend_fill_census_take_window(&census, 0u, 0u);
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION)
    CHECK(census.pass_clears[0] == 0u && census.kpx_clear == 0u);
#else
    CHECK(census.pass_clears[0] == 1u && census.kpx_clear == 1024u);
#endif
    CHECK(census.pass_draws[4] == 1u && census.draws == 1u);

    /* glReadPixels splits a pass only when it reads the pass framebuffer
     * (vitaGL framebuffers.c:674, in_use == active_read): a read through
     * another binding leaves the open pass intact. */
    gl_vita_backend_fill_census_present();
    CHECK(oracle_fill_bind_framebuffer(cpu, stack_top, 7u));
    CHECK(oracle_fill_draw_quad(cpu, stack_top));
    CHECK(oracle_fill_bind_framebuffer(cpu, stack_top, 0u));
    CHECK(oracle_fill_read_pixels(cpu, stack_top));
    CHECK(oracle_fill_bind_framebuffer(cpu, stack_top, 7u));
    CHECK(oracle_fill_draw_quad(cpu, stack_top));
    CHECK(oracle_fill_read_pixels(cpu, stack_top));
    CHECK(oracle_fill_draw_quad(cpu, stack_top));
    gl_vita_backend_fill_census_take_window(&census, 0u, 0u);
    CHECK(census.draws == 3u && census.pass_draws[0] == 2u &&
          census.pass_draws[1] == 1u && census.pass_draws[4] == 0u);
    CHECK(oracle_fill_bind_framebuffer(cpu, stack_top, 0u));

#if defined(ISAAC_VITA_GL_FILL_CENSUS_DUMP)
    /* Boot receipt values (kage_vita_backend.c banner line). */
    {
        uint32_t dump_render_p50_us = 1u;
        uint32_t dump_min_window = 1u;
        uint32_t dump_window = 1u;
        uint32_t dump_frames = 1u;

        CHECK(gl_vita_backend_fill_census_dump_config(
                  &dump_render_p50_us, &dump_min_window, &dump_window,
                  &dump_frames) == 1u);
        CHECK(dump_render_p50_us == 60000u && dump_min_window == 30u &&
              dump_window == 0u && dump_frames == 2u);
    }
#endif

#if defined(ISAAC_VITA_GL_FILL_CENSUS_DUMP) && \
    !defined(ISAAC_VITA_FXRAY_ALPHA_MASK) && \
    !defined(ISAAC_VITA_FBO_RASTER_SCALE)
    /* One-frame draw dump: armed by take-window (window >= 30 and render
     * p50 >= 60000 us), captured between the next two presents, emitted by
     * the following take-window through isaac_vita_log, at most two frames
     * per launch.  No wrapper logs. */
    s_fill_log_calls = 0u;
    s_fill_log_max_length = 0u;
    CHECK(oracle_fill_bind_texture(cpu, stack_top, 0u));
    CHECK(oracle_fill_blend(cpu, stack_top, ORACLE_GL_ONE, ORACLE_GL_ONE));
    gl_vita_backend_fill_census_take_window(&census, 10u, 70000u);
    gl_vita_backend_fill_census_present();
    CHECK(oracle_fill_draw_quad(cpu, stack_top));
    gl_vita_backend_fill_census_present();
    gl_vita_backend_fill_census_take_window(&census, 40u, 1000u);
    gl_vita_backend_fill_census_present();
    CHECK(oracle_fill_draw_quad(cpu, stack_top));
    gl_vita_backend_fill_census_present();
    gl_vita_backend_fill_census_take_window(&census, 40u, 70000u);
    CHECK(s_fill_log_calls == 0u);
    gl_vita_backend_fill_census_present();
    CHECK(oracle_fill_clear(cpu, stack_top, ORACLE_FILL_GL_COLOR_BUFFER_BIT));
    CHECK(oracle_fill_draw_quad(cpu, stack_top));
    CHECK(oracle_fill_attrib_toggle(cpu, stack_top, 0u, 0));
    CHECK(oracle_fill_draw_quad(cpu, stack_top));
    CHECK(oracle_fill_attrib_toggle(cpu, stack_top, 0u, 1));
    CHECK(s_fill_log_calls == 0u);
    gl_vita_backend_fill_census_present();
    CHECK(s_fill_log_calls == 0u);
    gl_vita_backend_fill_census_take_window(&census, 41u, 70000u);
    CHECK(s_fill_log_calls == 4u);
    CHECK(strcmp(s_fill_logs[0],
                 "KAGE VITA FILL DUMP f=1 i=0 C ord=4 fb=0 mask=4000 "
                 "att=960x544") == 0);
    CHECK(strstr(s_fill_logs[1],
                 "KAGE VITA FILL DUMP f=1 i=1 D ord=4 fb=0 prog=8 "
                 "tex=0/0x0 bl=1,1,1 tri=2 kpx=506 box=") != NULL);
    CHECK(strstr(s_fill_logs[1], " vp=0,0,960,540 syn=0 skip=0 rva=")
          != NULL);
    CHECK(strstr(s_fill_logs[2],
                 "KAGE VITA FILL DUMP f=1 i=2 D ord=4 fb=0 prog=8 "
                 "tex=0/0x0 bl=1,1,1 tri=2 kpx=0 box=0,0,0,0 "
                 "vp=0,0,960,540 syn=0 skip=5 rva=") != NULL);
    CHECK(strcmp(s_fill_logs[3],
                 "KAGE VITA FILL DUMP f=1 end n=3 trunc=0") == 0);
    CHECK(s_fill_log_max_length > 0u && s_fill_log_max_length < 384u);
    /* Second frame allowed, third refused. */
    gl_vita_backend_fill_census_take_window(&census, 42u, 70000u);
    gl_vita_backend_fill_census_present();
    CHECK(oracle_fill_draw_quad(cpu, stack_top));
    gl_vita_backend_fill_census_present();
    gl_vita_backend_fill_census_take_window(&census, 43u, 70000u);
    CHECK(s_fill_log_calls == 6u);
    CHECK(strcmp(s_fill_logs[5],
                 "KAGE VITA FILL DUMP f=2 end n=1 trunc=0") == 0);
    gl_vita_backend_fill_census_present();
    CHECK(oracle_fill_draw_quad(cpu, stack_top));
    gl_vita_backend_fill_census_present();
    gl_vita_backend_fill_census_take_window(&census, 44u, 70000u);
    CHECK(s_fill_log_calls == 6u);
#endif

    /* Uninstall drops every shadow and table. */
    gl_vita_backend_uninstall();
    CHECK(gl_vita_backend_install());
    CHECK(oracle_fill_use_program(cpu, stack_top, 8u));
    CHECK(oracle_set_viewport(cpu, stack_top, 0, 0, 960, 540));
    CHECK(oracle_fill_attrib_toggle(cpu, stack_top, 0u, 1));
    CHECK(oracle_fill_attrib_pointer(
        cpu, stack_top, 0u, 3u, ORACLE_GL_FLOAT, ORACLE_FILL_STRIDE,
        vertices));
    CHECK(oracle_fill_single(cpu, stack_top, &census));
    CHECK(census.miss == 1u && census.kpx_clipped == 0u);
    return 0;
}
#endif

int main(void)
{
    static const char *const missing[11] = {
        "glClampColorARB",
        "glUniform1uiv",
        "glUniform2uiv",
        "glUniform3uiv",
        "glUniform4uiv",
        "glUniformMatrix2x3fv",
        "glUniformMatrix2x4fv",
        "glUniformMatrix3x2fv",
        "glUniformMatrix3x4fv",
        "glUniformMatrix4x2fv",
        "glUniformMatrix4x3fv"
    };
    uint32_t stack[96];
    uint32_t redzone[3];
    int32_t viewport_redzone[6];
    uint32_t renderbuffers[2];
    uint32_t arguments[4];
    uint64_t depth_bits;
    guest_gl_double guest_depth;
    uint32_t stack_top = (uint32_t)(uintptr_t)&stack[96];
    CPU cpu;
    IsaacVitaGlRenderTargetTelemetry rt_event;
    unsigned gen_calls_before;
    unsigned get_calls_before;
    unsigned viewport_calls_before;
    size_t index;

    memset(&cpu, 0, sizeof cpu);

#if defined(ISAAC_VITA_IO_PROFILE)
    memset(&rt_event, 0, sizeof rt_event);
    rt_event.site = "scene_reset:framebufferX";
    rt_event.width = 1024u;
    rt_event.height = 1024u;
    oracle_rt_reset();
    isaac_vita_vitagl_render_target_event(&rt_event);
    CHECK(s_memory_snapshot_calls == 0u);

    memset(&rt_event, 0, sizeof rt_event);
    rt_event.site = "scene_reset:framebuffer:reserved";
    rt_event.first_target = (const void *)(uintptr_t)0x81234000u;
    rt_event.width = 1024u;
    rt_event.height = 1024u;
    rt_event.recovered = 1u;
    oracle_rt_reset();
    isaac_vita_vitagl_render_target_event(&rt_event);
    CHECK(s_memory_snapshot_calls == 1u);
    CHECK(s_memory_snapshot_event_result == 0);
    CHECK(s_memory_snapshot_target == rt_event.first_target);
    CHECK(s_rt_log_calls == 1u && s_rt_fault_calls == 0u);
#endif

    /* The recovered record retains every raw result but must not close the
     * startup profile or fault.  This all-UINT32_MAX case is the exact longest
     * record the 32-bit formatter can emit. */
    memset(&rt_event, 0xff, sizeof rt_event);
    rt_event.site = "scene_reset:framebuffer:no-pending-targets";
    rt_event.recovered = 1u;
    oracle_rt_reset();
    isaac_vita_vitagl_render_target_event(&rt_event);
    CHECK(s_rt_log_calls == 1u);
    CHECK(s_rt_log_result == (int)ISAAC_VITAGL_RT_EVENT_MAX_BODY);
    CHECK(strlen(s_rt_log) == ISAAC_VITAGL_RT_EVENT_MAX_BODY);
    CHECK(s_rt_profile_calls == 0u && s_rt_fault_calls == 0u);
    CHECK(strstr(s_rt_log, "GXM RT RECOVERED") != NULL);
    CHECK(strstr(s_rt_log, "f=0xffffffff/0xffffffff") != NULL);
    CHECK(strstr(s_rt_log, "r=0xffffffff/0xffffffff") != NULL);
    CHECK(strstr(s_rt_log,
                 "fin=0xffffffff fc=4294967295 del=0xffffffff") != NULL);
    CHECK(strstr(s_rt_log,
                 "p=4294967295,4294967295,4294967295,4294967295") != NULL);

    memset(&rt_event, 0, sizeof rt_event);
    rt_event.site = "scene_reset:framebuffer:pool-full";
    rt_event.first_result = (int32_t)0x805b0027u;
    rt_event.pool_full = 1u;
    oracle_rt_reset();
    isaac_vita_vitagl_render_target_event(&rt_event);
    CHECK(s_rt_log_calls == 1u && s_rt_profile_calls == 1u);
    CHECK(s_rt_fault_calls == 1u && s_rt_fault_result == 0x805b0027u);
    CHECK(strcmp(s_rt_profile_reason, "render-target-acquire") == 0);
    CHECK(strcmp(s_rt_fault_message,
                 "vitaGL render-target acquire failed") == 0);

    memset(&rt_event, 0, sizeof rt_event);
    rt_event.site = "scene_reset:framebuffer:retry";
    rt_event.first_result = (int32_t)0x805b0027u;
    rt_event.retry_result = (int32_t)0x805b0004u;
    rt_event.retry_attempted = 1u;
    oracle_rt_reset();
    isaac_vita_vitagl_render_target_event(&rt_event);
    CHECK(s_rt_fault_calls == 1u && s_rt_fault_result == 0x805b0004u);

    memset(&rt_event, 0, sizeof rt_event);
    rt_event.site = "mark_rt_as_dirty:invariant";
    rt_event.invariant_failure = 1u;
    oracle_rt_reset();
    isaac_vita_vitagl_render_target_event(&rt_event);
    CHECK(s_rt_fault_calls == 1u && s_rt_fault_result == 0x805b0004u);

    oracle_rt_reset();
    isaac_vita_vitagl_render_target_event(NULL);
    CHECK(s_rt_log_calls == 1u && s_rt_profile_calls == 1u);
    CHECK(s_rt_fault_calls == 1u && s_rt_fault_result == 0x805b0004u);
    CHECK(strcmp(s_rt_fault_message,
                 "vitaGL render-target telemetry was null") == 0);

    CHECK(gl_vita_backend_install());
    CHECK(gl_vita_backend_resolved_count() == 62u);
    CHECK(gl_vita_backend_missing_count() == 11u);
    for (index = 0u; index < 11u; ++index)
        CHECK(strcmp(gl_vita_backend_missing_symbol(index), missing[index]) == 0);
    CHECK(gl_vita_backend_missing_symbol(11u) == NULL);

#if defined(ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY)
    /* Generated CALL lowering stacks canonical RVAs.  Exercise both frozen
     * consumers, retain relocated compatibility, then make hostile return,
     * shape, and driver failures prove the ordinary draw remains fail-closed. */
    arguments[0] = ORACLE_GL_TRIANGLES;
    arguments[1] = 6u;
    arguments[2] = ORACLE_GL_UNSIGNED_SHORT;
    arguments[3] = 0x1000u;
    s_native_canonical_quad_result = 1u;
    CHECK(oracle_dispatch_with_return(
        &cpu, stack_top, ORACLE_TOKEN_DRAW_ELEMENTS, arguments, 4u,
        0x0056039du));
    CHECK(oracle_dispatch_with_return(
        &cpu, stack_top, ORACLE_TOKEN_DRAW_ELEMENTS, arguments, 4u,
        0x0056d794u));
    CHECK(oracle_dispatch_with_return(
        &cpu, stack_top, ORACLE_TOKEN_DRAW_ELEMENTS, arguments, 4u,
        GUEST_IMAGE_BASE + 0x0056039du));
    CHECK(s_native_canonical_quad_calls == 3u);
    CHECK(s_native_canonical_quad_count == 6);
    CHECK(s_native_draw_elements_calls == 0u);

    CHECK(oracle_dispatch_with_return(
        &cpu, stack_top, ORACLE_TOKEN_DRAW_ELEMENTS, arguments, 4u,
        0x0056039cu));
    CHECK(oracle_dispatch_with_return(
        &cpu, stack_top, ORACLE_TOKEN_DRAW_ELEMENTS, arguments, 4u,
        GUEST_IMAGE_BASE + 0x0085f000u));
    CHECK(s_native_canonical_quad_calls == 3u);
    CHECK(s_native_draw_elements_calls == 2u);
    CHECK(s_native_draw_mode == ORACLE_GL_TRIANGLES);
    CHECK(s_native_draw_count == 6);
    CHECK(s_native_draw_type == ORACLE_GL_UNSIGNED_SHORT);
    CHECK((uintptr_t)s_native_draw_indices == 0x1000u);

    arguments[0] = 5u;
    CHECK(oracle_dispatch_with_return(
        &cpu, stack_top, ORACLE_TOKEN_DRAW_ELEMENTS, arguments, 4u,
        0x0056039du));
    CHECK(s_native_canonical_quad_calls == 3u);
    CHECK(s_native_draw_elements_calls == 3u);

    arguments[0] = ORACLE_GL_TRIANGLES;
    s_native_canonical_quad_result = 0u;
    CHECK(oracle_dispatch_with_return(
        &cpu, stack_top, ORACLE_TOKEN_DRAW_ELEMENTS, arguments, 4u,
        0x0056039du));
    CHECK(s_native_canonical_quad_calls == 4u);
    CHECK(s_native_draw_elements_calls == 4u);
#if defined(ISAAC_VITA_PHASE_PROFILE)
    CHECK(g_isaac_vita_gl_phase_profile_counters.draw_elements == 7u);
    CHECK(g_isaac_vita_gl_phase_profile_counters.canonical_quad_hits == 3u);
    CHECK(g_isaac_vita_gl_phase_profile_counters
              .canonical_quad_reject_callsite == 2u);
    CHECK(g_isaac_vita_gl_phase_profile_counters
              .canonical_quad_reject_shape == 1u);
    CHECK(g_isaac_vita_gl_phase_profile_counters
              .canonical_quad_reject_bounds == 0u);
    CHECK(g_isaac_vita_gl_phase_profile_counters
              .canonical_quad_reject_pointer == 0u);
    CHECK(g_isaac_vita_gl_phase_profile_counters
              .canonical_quad_driver_fallback == 1u);
    CHECK(g_isaac_vita_gl_phase_profile_counters
              .canonical_quad_index_bytes_saved == 36u);
#endif
#endif

    /* The A/B virtualizes only the default-framebuffer viewport.  OFF must
     * retain the old native passthrough, including native GL_VIEWPORT query
     * values; ON exposes the guest's unscaled logical state. */
    get_calls_before = s_native_get_integer_calls;
    CHECK(oracle_get_viewport(&cpu, stack_top, viewport_redzone));
#if defined(ORACLE_LOGICAL_VIEWPORT)
    CHECK(s_native_get_integer_calls == get_calls_before);
    CHECK(viewport_redzone[1] == 0 && viewport_redzone[2] == 0);
    CHECK(viewport_redzone[3] == 960 && viewport_redzone[4] == 540);
#else
    CHECK(s_native_get_integer_calls == get_calls_before + 1u);
    CHECK(viewport_redzone[1] == 11 && viewport_redzone[2] == 22);
    CHECK(viewport_redzone[3] == 333 && viewport_redzone[4] == 444);
#endif

    viewport_calls_before = s_native_viewport_calls;
    CHECK(oracle_set_viewport(&cpu, stack_top, 0, 0, 960, 540));
    CHECK(s_native_viewport_calls == viewport_calls_before + 1u);
#if defined(ISAAC_VITA_DISPLAY_RASTER_720)
    CHECK(oracle_last_viewport_is(0, 1, 720, 405));
#else
    CHECK(oracle_last_viewport_is(0, 0, 960, 540));
#endif

    CHECK(oracle_set_viewport(&cpu, stack_top, 1, 2, 5, 7));
#if defined(ISAAC_VITA_DISPLAY_RASTER_720)
    CHECK(oracle_last_viewport_is(0, 2, 4, 5));
#else
    CHECK(oracle_last_viewport_is(1, 2, 5, 7));
#endif
    CHECK(oracle_set_viewport(&cpu, stack_top, -1, -2, 3, 5));
#if defined(ISAAC_VITA_DISPLAY_RASTER_720)
    CHECK(oracle_last_viewport_is(-1, -1, 2, 4));
#else
    CHECK(oracle_last_viewport_is(-1, -2, 3, 5));
#endif

    /* READ_FRAMEBUFFER never changes the draw target and therefore never
     * replays the viewport.  DRAW_FRAMEBUFFER and FRAMEBUFFER do, in native
     * bind-then-viewport order, only when the draw binding changes. */
    oracle_reset_gl_events();
    viewport_calls_before = s_native_viewport_calls;
    CHECK(oracle_bind_framebuffer(
        &cpu, stack_top, ORACLE_GL_READ_FRAMEBUFFER, 91u));
    CHECK(s_native_bind_framebuffer_target == ORACLE_GL_READ_FRAMEBUFFER);
    CHECK(s_native_bind_framebuffer_name == 91u);
    CHECK(s_native_viewport_calls == viewport_calls_before);
    CHECK(s_gl_event_count == 1u &&
          s_gl_events[0] == ORACLE_GL_EVENT_BIND_FRAMEBUFFER);

    oracle_reset_gl_events();
    viewport_calls_before = s_native_viewport_calls;
    CHECK(oracle_bind_framebuffer(
        &cpu, stack_top, ORACLE_GL_DRAW_FRAMEBUFFER, 7u));
#if defined(ORACLE_LOGICAL_VIEWPORT)
    CHECK(s_native_viewport_calls == viewport_calls_before + 1u);
    CHECK(oracle_last_viewport_is(-1, -2, 3, 5));
    CHECK(s_gl_event_count == 2u &&
          s_gl_events[0] == ORACLE_GL_EVENT_BIND_FRAMEBUFFER &&
          s_gl_events[1] == ORACLE_GL_EVENT_VIEWPORT);
#else
    CHECK(s_native_viewport_calls == viewport_calls_before);
    CHECK(s_gl_event_count == 1u &&
          s_gl_events[0] == ORACLE_GL_EVENT_BIND_FRAMEBUFFER);
#endif

    /* A nonzero draw FBO receives exact guest values. */
    CHECK(oracle_set_viewport(&cpu, stack_top, 1, 2, 5, 7));
    CHECK(oracle_last_viewport_is(1, 2, 5, 7));

    oracle_reset_gl_events();
    viewport_calls_before = s_native_viewport_calls;
    CHECK(oracle_bind_framebuffer(
        &cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 0u));
#if defined(ORACLE_LOGICAL_VIEWPORT)
    CHECK(s_native_viewport_calls == viewport_calls_before + 1u);
# if defined(ISAAC_VITA_DISPLAY_RASTER_720)
    CHECK(oracle_last_viewport_is(0, 2, 4, 5));
# else
    CHECK(oracle_last_viewport_is(1, 2, 5, 7));
# endif
    CHECK(s_gl_event_count == 2u &&
          s_gl_events[0] == ORACLE_GL_EVENT_BIND_FRAMEBUFFER &&
          s_gl_events[1] == ORACLE_GL_EVENT_VIEWPORT);
#else
    CHECK(s_native_viewport_calls == viewport_calls_before);
    CHECK(s_gl_event_count == 1u &&
          s_gl_events[0] == ORACLE_GL_EVENT_BIND_FRAMEBUFFER);
#endif

    oracle_reset_gl_events();
    viewport_calls_before = s_native_viewport_calls;
    CHECK(oracle_bind_framebuffer(
        &cpu, stack_top, ORACLE_GL_FRAMEBUFFER, 9u));
#if defined(ORACLE_LOGICAL_VIEWPORT)
    CHECK(s_native_viewport_calls == viewport_calls_before + 1u);
    CHECK(oracle_last_viewport_is(1, 2, 5, 7));
    CHECK(s_gl_event_count == 2u &&
          s_gl_events[0] == ORACLE_GL_EVENT_BIND_FRAMEBUFFER &&
          s_gl_events[1] == ORACLE_GL_EVENT_VIEWPORT);
#else
    CHECK(s_native_viewport_calls == viewport_calls_before);
    CHECK(s_gl_event_count == 1u &&
          s_gl_events[0] == ORACLE_GL_EVENT_BIND_FRAMEBUFFER);
#endif

    renderbuffers[0] = 9u;
    viewport_calls_before = s_native_viewport_calls;
    CHECK(oracle_delete_framebuffer(&cpu, stack_top, &renderbuffers[0]));
#if defined(ORACLE_LOGICAL_VIEWPORT)
    CHECK(s_native_viewport_calls == viewport_calls_before + 1u);
# if defined(ISAAC_VITA_DISPLAY_RASTER_720)
    CHECK(oracle_last_viewport_is(0, 2, 4, 5));
# else
    CHECK(oracle_last_viewport_is(1, 2, 5, 7));
# endif
#else
    CHECK(s_native_viewport_calls == viewport_calls_before);
#endif

    /* Extreme signed origins and a maximal valid width prove the promoted
     * edge arithmetic cannot wrap. */
    CHECK(oracle_set_viewport(
        &cpu, stack_top, INT32_MIN, INT32_MAX, INT32_MAX, 0));
#if defined(ISAAC_VITA_DISPLAY_RASTER_720)
    CHECK(oracle_last_viewport_is(
        -1610612736, 1610612736, 1610612735, 0));
#else
    CHECK(oracle_last_viewport_is(
        INT32_MIN, INT32_MAX, INT32_MAX, 0));
#endif
    CHECK(oracle_get_viewport(&cpu, stack_top, viewport_redzone));
#if defined(ORACLE_LOGICAL_VIEWPORT)
    CHECK(viewport_redzone[1] == INT32_MIN);
    CHECK(viewport_redzone[2] == INT32_MAX);
    CHECK(viewport_redzone[3] == INT32_MAX);
    CHECK(viewport_redzone[4] == 0);

    /* Invalid sizes reach native GL unchanged and do not mutate the virtual
     * logical viewport that a later query/rebind will replay. */
    CHECK(oracle_set_viewport(&cpu, stack_top, 5, 6, -1, 8));
    CHECK(oracle_last_viewport_is(5, 6, -1, 8));
    CHECK(oracle_get_viewport(&cpu, stack_top, viewport_redzone));
    CHECK(viewport_redzone[1] == INT32_MIN);
    CHECK(viewport_redzone[2] == INT32_MAX);
    CHECK(viewport_redzone[3] == INT32_MAX);
    CHECK(viewport_redzone[4] == 0);
#endif

    /* Install/uninstall begins a fresh virtual context at the measured
     * logical 960x540 default-FBO viewport. */
    gl_vita_backend_uninstall();
    CHECK(gl_vita_backend_install());
    CHECK(oracle_get_viewport(&cpu, stack_top, viewport_redzone));
#if defined(ORACLE_LOGICAL_VIEWPORT)
# if defined(ISAAC_VITA_DISPLAY_RASTER_720)
    CHECK(oracle_last_viewport_is(0, 1, 720, 405));
# else
    CHECK(oracle_last_viewport_is(0, 0, 960, 540));
# endif
    CHECK(viewport_redzone[1] == 0 && viewport_redzone[2] == 0);
    CHECK(viewport_redzone[3] == 960 && viewport_redzone[4] == 540);
#else
    CHECK(viewport_redzone[1] == 11 && viewport_redzone[2] == 22);
    CHECK(viewport_redzone[3] == 333 && viewport_redzone[4] == 444);
#endif

    /* Isaac passes -1000.0 and relies on desktop OpenGL's clampd contract.
     * Exercise the generated 8-byte guest ABI as well as both clamp edges. */
    guest_depth = -1000.0;
    memcpy(&depth_bits, &guest_depth, sizeof depth_bits);
    arguments[0] = (uint32_t)depth_bits;
    arguments[1] = (uint32_t)(depth_bits >> 32);
    CHECK(oracle_dispatch(
        &cpu, stack_top, ORACLE_TOKEN_CLEAR_DEPTH, arguments, 2u));
    CHECK(s_native_clear_depth_calls == 1u && s_native_clear_depth == 0.0);

    guest_depth = 2.0;
    memcpy(&depth_bits, &guest_depth, sizeof depth_bits);
    arguments[0] = (uint32_t)depth_bits;
    arguments[1] = (uint32_t)(depth_bits >> 32);
    CHECK(oracle_dispatch(
        &cpu, stack_top, ORACLE_TOKEN_CLEAR_DEPTH, arguments, 2u));
    CHECK(s_native_clear_depth_calls == 2u && s_native_clear_depth == 1.0);

    guest_depth = 0.625;
    memcpy(&depth_bits, &guest_depth, sizeof depth_bits);
    arguments[0] = (uint32_t)depth_bits;
    arguments[1] = (uint32_t)(depth_bits >> 32);
    CHECK(oracle_dispatch(
        &cpu, stack_top, ORACLE_TOKEN_CLEAR_DEPTH, arguments, 2u));
    CHECK(s_native_clear_depth_calls == 3u && s_native_clear_depth == 0.625);

    /* High-level KAGE's raw initial object is 1024x1024 DEPTH24, not the
     * logical 960x540 display.  Query writes exactly one guest GLint. */
    CHECK(gl_vita_backend_register_renderbuffer(
        77u, ORACLE_GL_DEPTH24, 1024, 1024));
    CHECK(oracle_query(
        &cpu, stack_top, ORACLE_GL_RENDERBUFFER_WIDTH, redzone, 1024u));
    CHECK(oracle_query(
        &cpu, stack_top, ORACLE_GL_RENDERBUFFER_HEIGHT, redzone, 1024u));

    /* Generated-object lifecycle: Gen -> Bind -> Storage -> Query. */
    renderbuffers[0] = renderbuffers[1] = 0u;
    arguments[0] = 2u;
    arguments[1] = (uint32_t)(uintptr_t)renderbuffers;
    CHECK(oracle_dispatch(
        &cpu, stack_top, ORACLE_TOKEN_GEN_RBO, arguments, 2u));
    CHECK(renderbuffers[0] == 900u && renderbuffers[1] == 901u);
    arguments[0] = ORACLE_GL_RENDERBUFFER;
    arguments[1] = renderbuffers[0];
    CHECK(oracle_dispatch(
        &cpu, stack_top, ORACLE_TOKEN_BIND_RBO, arguments, 2u));
    arguments[0] = ORACLE_GL_RENDERBUFFER;
    arguments[1] = ORACLE_GL_DEPTH24;
    arguments[2] = 321u;
    arguments[3] = 123u;
    CHECK(oracle_dispatch(
        &cpu, stack_top, ORACLE_TOKEN_STORE_RBO, arguments, 4u));
    CHECK(oracle_query(
        &cpu, stack_top, ORACLE_GL_RENDERBUFFER_WIDTH, redzone, 321u));
    CHECK(oracle_query(
        &cpu, stack_top, ORACLE_GL_RENDERBUFFER_HEIGHT, redzone, 123u));

    /* Deleting the bound object clears the binding.  The controlled backend
     * fault leaves no stale CPU: the following bind/query must still work. */
    arguments[0] = 1u;
    arguments[1] = (uint32_t)(uintptr_t)&renderbuffers[0];
    CHECK(oracle_dispatch(
        &cpu, stack_top, ORACLE_TOKEN_DELETE_RBO, arguments, 2u));
    arguments[0] = ORACLE_GL_RENDERBUFFER;
    arguments[1] = ORACLE_GL_RENDERBUFFER_WIDTH;
    arguments[2] = (uint32_t)(uintptr_t)&redzone[1];
    CHECK(oracle_expect_fault(
        &cpu, stack_top, ORACLE_TOKEN_QUERY_RBO, arguments, 3u,
        ORACLE_TOKEN_QUERY_RBO,
        "Vita GL glGetRenderbufferParameteriv has no bound renderbuffer"));
    arguments[0] = ORACLE_GL_RENDERBUFFER;
    arguments[1] = renderbuffers[1];
    CHECK(oracle_dispatch(
        &cpu, stack_top, ORACLE_TOKEN_BIND_RBO, arguments, 2u));
    arguments[0] = ORACLE_GL_RENDERBUFFER;
    arguments[1] = ORACLE_GL_DEPTH24;
    arguments[2] = 640u;
    arguments[3] = 360u;
    CHECK(oracle_dispatch(
        &cpu, stack_top, ORACLE_TOKEN_STORE_RBO, arguments, 4u));
    CHECK(oracle_query(
        &cpu, stack_top, ORACLE_GL_RENDERBUFFER_WIDTH, redzone, 640u));

    arguments[0] = 0u;
    arguments[1] = ORACLE_GL_RENDERBUFFER_WIDTH;
    arguments[2] = (uint32_t)(uintptr_t)&redzone[1];
    CHECK(oracle_expect_fault(
        &cpu, stack_top, ORACLE_TOKEN_QUERY_RBO, arguments, 3u,
        ORACLE_TOKEN_QUERY_RBO,
        "Vita GL glGetRenderbufferParameteriv received an invalid target"));
    arguments[0] = ORACLE_GL_RENDERBUFFER;
    arguments[1] = 0x8d44u;
    CHECK(oracle_expect_fault(
        &cpu, stack_top, ORACLE_TOKEN_QUERY_RBO, arguments, 3u,
        ORACLE_TOKEN_QUERY_RBO,
        "Vita GL glGetRenderbufferParameteriv received an unsupported pname"));
    arguments[1] = ORACLE_GL_RENDERBUFFER_WIDTH;
    arguments[2] = 0u;
    CHECK(oracle_expect_fault(
        &cpu, stack_top, ORACLE_TOKEN_QUERY_RBO, arguments, 3u,
        ORACLE_TOKEN_QUERY_RBO,
        "Vita GL glGetRenderbufferParameteriv received a null output"));
    arguments[0] = ORACLE_GL_RENDERBUFFER;
    arguments[1] = 0xdeadu;
    CHECK(oracle_expect_fault(
        &cpu, stack_top, ORACLE_TOKEN_BIND_RBO, arguments, 2u,
        ORACLE_TOKEN_BIND_RBO,
        "Vita GL glBindRenderbuffer received an unknown renderbuffer"));

    /* Capacity is preflighted before native allocation, so exhaustion cannot
     * leak a generated tail. */
    gl_vita_backend_uninstall();
    CHECK(gl_vita_backend_install());
    for (index = 0u; index < 63u; ++index)
        CHECK(gl_vita_backend_register_renderbuffer(
            1000u + (uint32_t)index, ORACLE_GL_DEPTH24, 8, 8));
    gen_calls_before = s_native_gen_calls;
    arguments[0] = 2u;
    arguments[1] = (uint32_t)(uintptr_t)renderbuffers;
    CHECK(oracle_expect_fault(
        &cpu, stack_top, ORACLE_TOKEN_GEN_RBO, arguments, 2u,
        ORACLE_TOKEN_GEN_RBO,
        "Vita GL glGenRenderbuffers received an invalid output array"));
    CHECK(s_native_gen_calls == gen_calls_before);
    CHECK(gl_vita_backend_register_renderbuffer(
        2000u, ORACLE_GL_DEPTH24, 8, 8));
    CHECK(!gl_vita_backend_register_renderbuffer(
        2001u, ORACLE_GL_DEPTH24, 8, 8));
    CHECK(strcmp(gl_vita_backend_last_error(),
                 "Vita GL renderbuffer registry exhausted") == 0);
    CHECK(s_native_delete_calls == 1u);

#if defined(ISAAC_VITA_GL_REDUNDANCY_CACHE)
    CHECK(oracle_test_gl_redundancy(&cpu, stack_top) == 0);
#endif
#if defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO)
    CHECK(oracle_test_location_memo_generation(&cpu, stack_top) == 0);
#endif
#if defined(ISAAC_VITA_GL_TYPED_STATE_CACHE)
    CHECK(oracle_test_gl_typed_state(&cpu, stack_top) == 0);
#endif
#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
    CHECK(oracle_test_texture_churn(&cpu, stack_top) == 0);
#endif
#if defined(ISAAC_VITA_FXRAY_ALPHA_MASK)
    CHECK(oracle_test_fxray_alpha_mask(&cpu, stack_top) == 0);
#else
    CHECK(oracle_test_fxray_off_passthrough(&cpu, stack_top) == 0);
#endif
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION_DEPTH_DROP)
    CHECK(oracle_test_fbo_clear_elision_depth_drop(&cpu, stack_top) == 0);
#elif defined(ISAAC_VITA_FBO_CLEAR_ELISION)
    CHECK(oracle_test_fbo_clear_elision(&cpu, stack_top) == 0);
#endif
#if defined(ISAAC_VITA_FBO_RASTER_SCALE)
    CHECK(oracle_test_fbo_raster_scale(&cpu, stack_top) == 0);
#endif
#if defined(ISAAC_VITA_GL_FILL_CENSUS)
    CHECK(oracle_test_fill_census(&cpu, stack_top) == 0);
#endif

#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
    CHECK(oracle_test_attrib_coalescing(&cpu, stack_top) == 0);
#endif
#if defined(ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE)
    CHECK(oracle_test_attrib_direct_state() == 0);
#endif
#if defined(ISAAC_VITA_GL_WRAPPER_TIME)
    /* ph120.gd draw split (coalesce-wrapper-time mode): the gl part is the
     * gt draw bracket's own two reads, so us and calls match exactly; every
     * wrapper body passed ENTER once and BODY_DONE once (no canonical quad
     * path in this mode, so no fallback re-entry); the monotonic stub clock
     * never reverses.  Direct backend->glDrawElements calls (no bridge run)
     * still charge disp from the last bridge entry read and skip the tail,
     * so only the shared-read identities are pinned here. */
    CHECK(g_isaac_vita_gl_wrapper_time.draw[ISAAC_VITA_GL_DRAW_SPLIT_BODY]
              .total_us == g_isaac_vita_gl_time_profile.draw.total_us);
    CHECK(g_isaac_vita_gl_wrapper_time.draw[ISAAC_VITA_GL_DRAW_SPLIT_BODY]
              .calls == g_isaac_vita_gl_time_profile.draw.calls);
    CHECK(g_isaac_vita_gl_wrapper_time.draw[ISAAC_VITA_GL_DRAW_SPLIT_BODY]
              .calls > 0u);
    CHECK(g_isaac_vita_gl_wrapper_time.draw[ISAAC_VITA_GL_DRAW_SPLIT_DISPATCH]
              .calls ==
          g_isaac_vita_gl_wrapper_time.draw[ISAAC_VITA_GL_DRAW_SPLIT_BODY]
              .calls);
    CHECK(g_isaac_vita_gl_wrapper_time.draw[ISAAC_VITA_GL_DRAW_SPLIT_TAIL]
              .calls <=
          g_isaac_vita_gl_wrapper_time.draw[ISAAC_VITA_GL_DRAW_SPLIT_BODY]
              .calls);
    CHECK(g_isaac_vita_gl_wrapper_time.draw_canonical == 0u);
    CHECK(g_isaac_vita_gl_wrapper_time.bad_clock == 0u);
#endif
    gl_vita_backend_uninstall();
    CHECK(!gl_vita_backend_installed());
    CHECK(gl_vita_backend_resolved_count() == 0u);
    CHECK(gl_vita_backend_missing_count() == 73u);
#if defined(ISAAC_VITA_DISPLAY_RASTER_720)
    puts("Vita GL boundary oracle: PASS (720x408 default-FBO raster; logical viewport query/replay; signed edge scaling; RBO/clamp contracts)");
#else
    puts("Vita GL boundary oracle: PASS (display raster OFF exact passthrough; RBO/clamp contracts)");
#endif
    return 0;
}
