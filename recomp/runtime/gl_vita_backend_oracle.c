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
    ++s_native_enable_attrib_calls;
    s_native_attrib_index = index;
}

void oracle_glDisableVertexAttribArray(GLuint index)
{
    ++s_native_disable_attrib_calls;
    s_native_attrib_index = index;
}

void oracle_glVertexAttribPointer(
    GLuint index, GLint size, GLenum type, GLboolean normalized,
    GLsizei stride, const void *pointer)
{
    ++s_native_attrib_pointer_calls;
    s_native_attrib_index = index;
    s_native_attrib_size = size;
    s_native_attrib_type = type;
    s_native_attrib_normalized = normalized;
    s_native_attrib_stride = stride;
    s_native_attrib_pointer = pointer;
}

void oracle_glGetIntegerv(GLenum name, GLint *value)
{
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
    ++s_native_draw_elements_calls;
    s_native_draw_mode = mode;
    s_native_draw_count = count;
    s_native_draw_type = type;
    s_native_draw_indices = indices;
}

GLboolean vglIsaacDrawCanonicalQuads(GLsizei count)
{
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

#if defined(ISAAC_VITA_FBO_CLEAR_ELISION)
/* Exact clear elision on offscreen targets: colour no-ops are absorbed,
 * depth/stencil clears are owed to the first draw of the same GXM scene. */
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
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION)
    CHECK(oracle_test_fbo_clear_elision(&cpu, stack_top) == 0);
#endif
#if defined(ISAAC_VITA_FBO_RASTER_SCALE)
    CHECK(oracle_test_fbo_raster_scale(&cpu, stack_top) == 0);
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
