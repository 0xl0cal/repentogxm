/* Host oracle for ISAAC_VITA_FBO_DEFERRED_ATTACH (x86, 32-bit address space).
 *
 * Built twice by recomp/vita/test_fbo_deferred_attach.py for each flavour,
 * without and with the knob.  The real gl_vita_backend.c and gl_bridge.c are
 * linked; the guest calls go through guest_gl_dispatch exactly as
 * runtime/gl_vita_backend_oracle.c does (its dispatch helper is copied
 * below).  The native side is a mock vitaGL that prints every call it
 * receives and keeps the part of vitaGL's framebuffer state the knob could
 * disturb, copied from the stock reference tree read for V2-CONSUMERS.md:
 *
 *   _glFramebufferTexture2D (shared.h): the framebuffer copies the texture's
 *     width, height and data pointer AT THE CALL; a non-zero texture sets
 *     dirty_framebuffer iff in_use_framebuffer == active_write_fb; texture 0
 *     returns before that.
 *   scene_reset (gxm.c): a draw or clear opens a new scene iff
 *     in_use_framebuffer != active_write_fb, dirty_framebuffer, or a swap
 *     happened since the last scene.
 *   glCheckFramebufferStatus (framebuffers.c): COMPLETE iff no framebuffer
 *     is bound or it has a texture.
 *
 * Trace lines (stdout):
 *   N <call> ...          a native call as vitaGL received it
 *   C <consumer> <state>  the model state a consuming native call observed
 *   G <call> = <value>    a value returned to the guest
 *   T status <target>     the target of the guest status query that follows
 *   S <name>              scenario boundary
 *   D d=.. x=.. s=.. f=.. the knob's counters at a scenario end (ON only)
 * The driver asserts the ON trace equals the OFF trace minus exactly the
 * superseded attaches, with identical C and G lines.
 *
 * Scenario 13 issues the two native calls of manual_kage_vita.c's
 * destroy_render_targets itself (that unit is not host-buildable); the driver
 * checks in the source that the unit flushes before them, and builds this
 * file once more with ORACLE_SKIP_EXTERNAL_FLUSH to prove the comparison
 * rejects the sequence without the flush.
 *
 * The facade header maps glCheckFramebufferStatus to oracle_glReturnUint and
 * glFramebufferRenderbuffer (and glReadPixels without the readback fix) to
 * oracle_glNoop; no scenario here calls any other user of those two. */
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
# error fbo_deferred_attach_oracle requires a 32-bit address space
#endif

/* Copied from runtime/gl_vita_backend_oracle.c (ORACLE_GL_*). */
#define T_GL_FRAMEBUFFER         0x00008d40u
#define T_GL_READ_FRAMEBUFFER    0x00008ca8u
#define T_GL_DRAW_FRAMEBUFFER    0x00008ca9u
#define T_GL_TEXTURE_2D          0x00000de1u
#define T_GL_TEXTURE_BINDING_2D  0x00008069u
#define T_GL_RGBA                0x00001908u
#define T_GL_UNSIGNED_BYTE       0x00001401u
#define T_GL_TRIANGLES           0x00000004u
#define T_GL_UNSIGNED_SHORT      0x00001403u
#define T_GL_RENDERBUFFER        0x00008d41u
/* Copied from runtime/gl_vita_backend.c. */
#define T_GL_COLOR_ATTACHMENT0   0x00008ce0u /* GL_VITA_FBO_COLOR_ATTACHMENT0 */
#define T_GL_DEPTH_ATTACHMENT    0x00008d00u /* GL_VITA_FBO_DEPTH_ATTACHMENT */
#define T_GL_COLOR_BUFFER_BIT    0x00004000u /* GL_VITA_FBO_COLOR_BUFFER_BIT */
#define T_GL_DEPTH_BUFFER_BIT    0x00000100u /* GL_VITA_FBO_DEPTH_BUFFER_BIT */
#define T_GL_FRAMEBUFFER_COMPLETE 0x00008cd5u /* GL_VITA_FF_FRAMEBUFFER_COMPLETE */
#define T_GL_FRAMEBUFFER_MISSING  0x00008cd7u /* vitaGL.h GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT */
#define T_GL_ALPHA_BITS          0x00000d55u /* vitaGL.h GL_ALPHA_BITS */
#define T_GL_SCISSOR_TEST        0x00000c11u /* GL_VITA_FBO_SCISSOR_TEST */

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "deferred-attach oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

/* ------------------------------------------------------------ mock vitaGL */
#define M_TEXTURES 32u
#define M_FRAMEBUFFERS 8u

typedef struct m_texture {
    uint32_t width, height, generation;
    uint8_t *data;
} m_texture;

typedef struct m_framebuffer {
    uint32_t texture, width, height, generation;
} m_framebuffer;

static m_texture s_tex[M_TEXTURES];
static m_framebuffer s_fb[M_FRAMEBUFFERS];
static uint32_t s_write_fb, s_read_fb, s_bound_texture;
static uint32_t s_in_use_fb, s_dirty_fb, s_needs_reset = 1u, s_scenes;
static uint32_t s_generation;
static unsigned s_banner_calls;
static char s_banner[512];

static void m_state(const char *consumer)
{
    uint32_t i;

    printf("C %s w=%u r=%u use=%u dirty=%u scenes=%u", consumer,
           s_write_fb, s_read_fb, s_in_use_fb, s_dirty_fb, s_scenes);
    for (i = 1u; i < M_FRAMEBUFFERS; ++i) {
        if (s_fb[i].texture || s_fb[i].width)
            printf(" fb%u=(%u,%ux%u,g%u)", i, s_fb[i].texture,
                   s_fb[i].width, s_fb[i].height, s_fb[i].generation);
    }
    printf("\n");
}

static void m_scene_reset(void)
{
    if (s_in_use_fb != s_write_fb || s_dirty_fb || s_needs_reset) {
        ++s_scenes;
        s_in_use_fb = s_write_fb;
        s_dirty_fb = 0u;
        s_needs_reset = 0u;
    }
}

void oracle_glBindFramebuffer(GLenum target, GLuint framebuffer)
{
    printf("N bindfb %x %u\n", target, framebuffer);
    CHECK(framebuffer < M_FRAMEBUFFERS);
    if (target == T_GL_FRAMEBUFFER || target == T_GL_DRAW_FRAMEBUFFER)
        s_write_fb = framebuffer;
    if (target == T_GL_FRAMEBUFFER || target == T_GL_READ_FRAMEBUFFER)
        s_read_fb = framebuffer;
}

void oracle_glFramebufferTexture2D(
    GLenum target, GLenum attachment, GLenum texture_target,
    GLuint texture, GLint level)
{
    uint32_t fb = target == T_GL_READ_FRAMEBUFFER ? s_read_fb : s_write_fb;

    printf("N attach %x %x %x %u %d fb=%u\n", target, attachment,
           texture_target, texture, level, fb);
    CHECK(texture < M_TEXTURES);
    if (!fb || attachment != T_GL_COLOR_ATTACHMENT0)
        return;
    if (!texture) {
        s_fb[fb].texture = 0u;
        return;
    }
    s_fb[fb].texture = texture;
    s_fb[fb].width = s_tex[texture].width;
    s_fb[fb].height = s_tex[texture].height;
    s_fb[fb].generation = s_tex[texture].generation;
    if (s_in_use_fb == s_write_fb)
        s_dirty_fb = 1u;
}

GLuint oracle_glReturnUint()
{
    GLuint status = (!s_write_fb || s_fb[s_write_fb].texture) ?
        T_GL_FRAMEBUFFER_COMPLETE : T_GL_FRAMEBUFFER_MISSING;

    printf("N status\n");
    m_state("status");
    return status;
}

void oracle_glNoop()
{
    printf("N noop\n");
    m_state("noop");
}

void oracle_glClear(GLbitfield mask)
{
    printf("N clear %x\n", mask);
    m_scene_reset();
    m_state("clear");
}

void oracle_glDrawElements(
    GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    (void)indices;
    printf("N draw %x %d %x\n", mode, count, type);
    m_scene_reset();
    m_state("draw");
}

GLboolean vglIsaacDrawCanonicalQuads(GLsizei count)
{
    (void)count;
    return 0u;
}

void oracle_glBindTexture(GLenum target, GLuint texture)
{
    printf("N bindtex %x %u\n", target, texture);
    CHECK(texture < M_TEXTURES);
    s_bound_texture = texture;
}

void oracle_glTexImage2D(
    GLenum target, GLint level, GLint internal_format,
    GLsizei width, GLsizei height, GLint border,
    GLenum format, GLenum type, const void *pixels)
{
    m_texture *t = &s_tex[s_bound_texture];

    (void)internal_format; (void)border; (void)pixels;
    printf("N teximage %x %d %dx%d %x %x tex=%u\n", target, level, width,
           height, format, type, s_bound_texture);
    free(t->data);
    t->width = (uint32_t)width;
    t->height = (uint32_t)height;
    t->generation = ++s_generation;
    t->data = calloc((((size_t)width + 7u) & ~(size_t)7u) * 4u,
                     (size_t)height ? (size_t)height : 1u);
    CHECK(t->data != NULL);
    memset(t->data, (int)(0x40u + s_bound_texture),
           (((size_t)width + 7u) & ~(size_t)7u) * 4u * (size_t)height);
    m_state("teximage");
}

void oracle_glTexSubImage2D(
    GLenum target, GLint level, GLint x_offset, GLint y_offset,
    GLsizei width, GLsizei height, GLenum format, GLenum type,
    const void *pixels)
{
    (void)target; (void)level; (void)format; (void)type; (void)pixels;
    printf("N texsub %d,%d %dx%d tex=%u\n", x_offset, y_offset, width,
           height, s_bound_texture);
    m_state("texsub");
}

void oracle_glDeleteTextures(GLsizei count, const GLuint *textures)
{
    GLsizei i;
    uint32_t f;

    for (i = 0; i < count; ++i) {
        printf("N deltex %u\n", textures[i]);
        CHECK(textures[i] < M_TEXTURES);
        /* textures.c: the active framebuffers' attachment is inspected. */
        for (f = 1u; f < M_FRAMEBUFFERS; ++f) {
            if (s_fb[f].texture == textures[i])
                printf("N deltex-attached fb=%u active=%u\n", f,
                       (unsigned)(f == s_write_fb || f == s_read_fb));
        }
        free(s_tex[textures[i]].data);
        memset(&s_tex[textures[i]], 0, sizeof s_tex[0]);
    }
    m_state("deltex");
}

void oracle_glDeleteFramebuffers(GLsizei count, const GLuint *framebuffers)
{
    GLsizei i;

    for (i = 0; i < count; ++i) {
        printf("N delfb %u\n", framebuffers[i]);
        CHECK(framebuffers[i] < M_FRAMEBUFFERS);
        m_state("delfb");
        memset(&s_fb[framebuffers[i]], 0, sizeof s_fb[0]);
        if (s_write_fb == framebuffers[i]) s_write_fb = 0u;
        if (s_read_fb == framebuffers[i]) s_read_fb = 0u;
    }
}

void oracle_glGetIntegerv(GLenum name, GLint *value)
{
    if (name == T_GL_TEXTURE_BINDING_2D) {
        /* The readback copy's own save/restore: texture state only. */
        printf("N getint texbinding\n");
        *value = (GLint)s_bound_texture;
        return;
    }
    printf("N getint %x\n", name);
    m_state("getint");
    *value = (name == T_GL_ALPHA_BITS && s_write_fb &&
              s_fb[s_write_fb].texture) ? 8 : 0;
}

void oracle_glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
    printf("N viewport %d,%d %dx%d\n", x, y, width, height);
}

#if defined(ISAAC_VITA_FBO_READBACK_FIX)
struct SceGxmTexture { uint32_t texture; };
static struct SceGxmTexture s_gxm_view;
SceGxmContext *gxm_context;

void sceGxmFinish(SceGxmContext *context)
{
    (void)context;
    printf("N gxmfinish\n");
}

void oracle_glReadPixels(
    GLint x, GLint y, GLsizei width, GLsizei height,
    GLenum format, GLenum type, void *pixels)
{
    (void)format; (void)type;
    printf("N readpixels %d,%d %dx%d\n", x, y, width, height);
    m_state("readpixels");
    /* What the model's read framebuffer holds, so the guest-visible value
     * depends on the attachment vitaGL has at this call. */
    memset(pixels, (int)(0x40u + s_fb[s_read_fb].texture),
           (size_t)width * (size_t)height * 4u);
}

SceGxmTexture *vglGetGxmTexture(GLenum target)
{
    (void)target;
    s_gxm_view.texture = s_bound_texture;
    return s_tex[s_bound_texture].data ? &s_gxm_view : NULL;
}

void *vglGetTexDataPointer(GLenum target)
{
    (void)target;
    return s_tex[s_bound_texture].data;
}

uint32_t sceGxmTextureGetFormat(const SceGxmTexture *texture)
{ (void)texture; return SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR; }
uint32_t sceGxmTextureGetType(const SceGxmTexture *texture)
{ (void)texture; return 0x60000000u; /* GL_VITA_READBACK_GXM_LINEAR */ }
unsigned int sceGxmTextureGetWidth(const SceGxmTexture *texture)
{ return s_tex[texture->texture].width; }
unsigned int sceGxmTextureGetHeight(const SceGxmTexture *texture)
{ return s_tex[texture->texture].height; }
unsigned int sceGxmTextureGetStride(const SceGxmTexture *texture)
{ return ((s_tex[texture->texture].width + 7u) & ~7u) * 4u; }
void *sceGxmTextureGetData(const SceGxmTexture *texture)
{ return s_tex[texture->texture].data; }
#endif

/* Calls no scenario makes natively in a way that matters: printed, inert. */
void oracle_glBindRenderbuffer(GLenum t, GLuint r) { (void)t; (void)r; }
void oracle_glActiveTexture(GLenum t) { (void)t; }
void oracle_glBlendFuncSeparate(GLenum a, GLenum b, GLenum c, GLenum d)
{ (void)a; (void)b; (void)c; (void)d; }
void oracle_glClearDepth(GLdouble depth) { printf("N cleardepth %g\n", depth); }
void oracle_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{ (void)r; (void)g; (void)b; (void)a; }
void oracle_glEnable(GLenum capability) { printf("N enable %x\n", capability); }
void oracle_glDeleteRenderbuffers(GLsizei c, const GLuint *r) { (void)c; (void)r; }
void oracle_glGenRenderbuffers(GLsizei c, GLuint *r) { (void)c; (void)r; }
void oracle_glGenTextures(GLsizei c, GLuint *t) { (void)c; (void)t; }
void oracle_glRenderbufferStorage(GLenum t, GLenum f, GLsizei w, GLsizei h)
{ (void)t; (void)f; (void)w; (void)h; }
GLuint oracle_glCreateProgram(void) { return 1u; }
void oracle_glDeleteProgram(GLuint p) { (void)p; }
void oracle_glDepthFunc(GLenum f) { (void)f; }
void oracle_glDisableVertexAttribArray(GLuint i) { (void)i; }
void oracle_glGetProgramiv(GLuint p, GLenum n, GLint *v) { (void)p; (void)n; *v = 0; }
void oracle_glGetShaderiv(GLuint s, GLenum n, GLint *v) { (void)s; (void)n; *v = 0; }
GLint oracle_glGetUniformLocation(GLuint p, const GLchar *n) { (void)p; (void)n; return 0; }
void oracle_glLinkProgram(GLuint p) { (void)p; }
void oracle_glEnableVertexAttribArray(GLuint i) { (void)i; }
void oracle_glShaderSource(GLuint s, GLsizei c, const GLchar *const *t, const GLint *l)
{ (void)s; (void)c; (void)t; (void)l; }
void oracle_glUniform1i(GLint l, GLint v) { (void)l; (void)v; }
void oracle_glUniform4fv(GLint l, GLsizei c, const GLfloat *v) { (void)l; (void)c; (void)v; }
void oracle_glUniformMatrix4fv(GLint l, GLsizei c, GLboolean t, const GLfloat *v)
{ (void)l; (void)c; (void)t; (void)v; }
void oracle_glUseProgram(GLuint p) { (void)p; }
void oracle_glVertexAttribPointer(GLuint i, GLint s, GLenum t, GLboolean n,
                                  GLsizei st, const void *p)
{ (void)i; (void)s; (void)t; (void)n; (void)st; (void)p; }
GLint oracle_glReturnInt() { return 0; }
const GLubyte *oracle_glReturnString() { return (const GLubyte *)""; }
void vglIsaacMarkColorOffsetShader(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{ (void)a; (void)b; (void)c; (void)d; }

/* ------------------------------------------------------------ runtime stubs */
int kage_vita_backend_ready(void) { return 1; }

void guest_fault(CPU *__restrict c, uint32_t address, const char *message)
{
    (void)c;
    fprintf(stderr, "guest fault %08x: %s\n", address, message ? message : "");
    exit(2);
}

void isaac_gl_vita_oracle_rt_log(const char *format, ...) { (void)format; }
void isaac_gl_vita_oracle_rt_profile_report(const char *reason) { (void)reason; }
void isaac_gl_vita_oracle_rt_fault(uint32_t result, const char *message)
{ (void)result; (void)message; }
uint64_t sceKernelGetProcessTimeWide(void) { static uint64_t now; return now += 7u; }

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;
    char line[512];

    va_start(arguments, format);
    vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    line[sizeof line - 1u] = '\0';
    if (!strncmp(line, "KAGE VITA FBO DEFERRED ATTACH:", 30)) {
        ++s_banner_calls;
        memcpy(s_banner, line, sizeof s_banner);
    }
    /* Other backend logs (readback receipts) are not part of the trace. */
}

/* ------------------------------------------------------------ guest side */
static CPU s_cpu;
static uint32_t s_stack[96];
static uint32_t s_stack_top;

/* Copied from runtime/gl_vita_backend_oracle.c oracle_dispatch_with_return. */
static uint32_t g_call(const char *name, const uint32_t *arguments,
                       uint32_t argument_count)
{
    uint32_t index;
    uint32_t token = guest_gl_resolve(name);

    CHECK(token != 0u);
    s_cpu.esp = s_stack_top;
    s_cpu.eax = 0u;
    for (index = argument_count; index != 0u; --index)
        gpush(&s_cpu, arguments[index - 1u]);
    gpush(&s_cpu, 0xaabbccddu);
    CHECK(guest_gl_dispatch(&s_cpu, token));
    CHECK(s_cpu.esp == s_stack_top);
    return s_cpu.eax;
}

static void g_bind(uint32_t target, uint32_t fb)
{ uint32_t a[2] = {target, fb}; (void)g_call("glBindFramebuffer", a, 2u); }

static void g_attach_full(uint32_t target, uint32_t attachment,
                          uint32_t texture_target, uint32_t texture, uint32_t level)
{
    uint32_t a[5] = {target, attachment, texture_target, texture, level};
    (void)g_call("glFramebufferTexture2D", a, 5u);
}

static void g_attach(uint32_t texture)
{ g_attach_full(T_GL_FRAMEBUFFER, T_GL_COLOR_ATTACHMENT0, T_GL_TEXTURE_2D, texture, 0u); }

static void g_status(uint32_t target)
{
    uint32_t a[1] = {target};
    printf("T status %x\n", target);
    printf("G status = %x\n", g_call("glCheckFramebufferStatus", a, 1u));
}

static void g_viewport(uint32_t w, uint32_t h)
{ uint32_t a[4] = {0u, 0u, w, h}; (void)g_call("glViewport", a, 4u); }

static void g_draw(void)
{
    uint32_t a[4] = {T_GL_TRIANGLES, 6u, T_GL_UNSIGNED_SHORT, 0x1000u};
    (void)g_call("glDrawElements", a, 4u);
}

static void g_clear(uint32_t mask)
{ uint32_t a[1] = {mask}; (void)g_call("glClear", a, 1u); }

static void g_bind_texture(uint32_t texture)
{ uint32_t a[2] = {T_GL_TEXTURE_2D, texture}; (void)g_call("glBindTexture", a, 2u); }

static void g_tex_image(uint32_t texture, uint32_t w, uint32_t h)
{
    uint32_t a[9] = {T_GL_TEXTURE_2D, 0u, T_GL_RGBA, w, h, 0u, T_GL_RGBA,
                     T_GL_UNSIGNED_BYTE, 0u};
    g_bind_texture(texture);
    (void)g_call("glTexImage2D", a, 9u);
}

static void g_tex_sub_image(uint32_t texture)
{
    static uint8_t texel[4] = {1u, 2u, 3u, 4u};
    uint32_t a[9] = {T_GL_TEXTURE_2D, 0u, 0u, 0u, 1u, 1u, T_GL_RGBA,
                     T_GL_UNSIGNED_BYTE, 0u};
    a[8] = (uint32_t)(uintptr_t)texel;
    g_bind_texture(texture);
    (void)g_call("glTexSubImage2D", a, 9u);
}

static void g_read(void)
{
    static uint8_t texel[4];
    uint32_t a[7] = {3u, 2u, 1u, 1u, T_GL_RGBA, T_GL_UNSIGNED_BYTE, 0u};

    memset(texel, 0xa5, sizeof texel);
    a[6] = (uint32_t)(uintptr_t)texel;
    (void)g_call("glReadPixels", a, 7u);
    printf("G read = %02x%02x%02x%02x\n", texel[0], texel[1], texel[2], texel[3]);
}

static void g_delete_texture(uint32_t texture)
{
    static uint32_t name;
    uint32_t a[2] = {1u, 0u};

    name = texture;
    a[1] = (uint32_t)(uintptr_t)&name;
    (void)g_call("glDeleteTextures", a, 2u);
}

static void g_delete_framebuffer(uint32_t fb)
{
    static uint32_t name;
    uint32_t a[2] = {1u, 0u};

    name = fb;
    a[1] = (uint32_t)(uintptr_t)&name;
    (void)g_call("glDeleteFramebuffers", a, 2u);
}

static void g_get_alpha_bits(void)
{
    static int32_t value;
    uint32_t a[2] = {T_GL_ALPHA_BITS, 0u};

    value = -1;
    a[1] = (uint32_t)(uintptr_t)&value;
    (void)g_call("glGetIntegerv", a, 2u);
    printf("G alphabits = %d\n", value);
}

static void g_enable(uint32_t capability)
{ uint32_t a[1] = {capability}; (void)g_call("glEnable", a, 1u); }

static void g_renderbuffer(void)
{
    uint32_t a[4] = {T_GL_FRAMEBUFFER, T_GL_DEPTH_ATTACHMENT, T_GL_RENDERBUFFER, 0u};
    (void)g_call("glFramebufferRenderbuffer", a, 4u);
}

/* The present owner's sequence (kage_vita_backend.c kvb_present_closure):
 * the backend hooks, then vglSwapBuffers. */
static void g_swap(void)
{
#if defined(ISAAC_VITA_FBO_DEFERRED_ATTACH)
    gl_vita_backend_fbo_deferred_attach_present();
#endif
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION)
    gl_vita_backend_fbo_present();
#endif
    printf("N swap\n");
    m_state("swap");
    s_needs_reset = 1u;
}

#define FBO 5u      /* the manager framebuffer */
#define FBO2 6u
#define TEX_A 11u   /* 480x272 */
#define TEX_A2 12u  /* 480x272 */
#define TEX_B 13u   /* 784x472 */
#define TEX_C 14u   /* deleted / re-specified by scenarios */

static void scenario_begin(const char *name)
{
    printf("S %s\n", name);
    /* Common prefix: a settled pass on the manager framebuffer. */
    g_bind(T_GL_FRAMEBUFFER, FBO);
    g_attach(TEX_A);
    g_viewport(480u, 272u);
    g_clear(T_GL_COLOR_BUFFER_BIT | T_GL_DEPTH_BUFFER_BIT);
    g_draw();
}

static void scenario_end(void)
{
    /* Close on a consumer so nothing stays pending between scenarios. */
    g_bind(T_GL_FRAMEBUFFER, 0u);
    g_draw();
    g_swap();
#if defined(ISAAC_VITA_FBO_DEFERRED_ATTACH)
    {
        IsaacVitaFboDeferredAttachCounters c;
        unsigned i;

        gl_vita_backend_fbo_deferred_attach_take(&c);
        printf("D d=%u x=%u s=%u f=", c.deferred, c.dropped, c.status_local);
        for (i = 0u; i < ISAAC_VITA_FBO_DEFER_FLUSH_CLASSES; ++i)
            printf("%s%u", i ? "," : "", c.flushed[i]);
        printf("\n");
    }
#endif
}

/* The game's GetTexel: attach, status, 1x1 read, leave. */
static void get_texel(uint32_t texture)
{
    g_bind(T_GL_FRAMEBUFFER, FBO);
    g_attach(texture);
    g_status(T_GL_FRAMEBUFFER);
    g_read();
    g_bind(T_GL_FRAMEBUFFER, 0u);
}

static void pass(uint32_t texture, uint32_t w, uint32_t h)
{
    g_bind(T_GL_FRAMEBUFFER, FBO);
    g_attach(texture);
    g_status(T_GL_FRAMEBUFFER);
    g_viewport(w, h);
    g_clear(T_GL_COLOR_BUFFER_BIT | T_GL_DEPTH_BUFFER_BIT);
    g_draw();
    g_draw();
    g_clear(T_GL_DEPTH_BUFFER_BIT);
    g_bind(T_GL_FRAMEBUFFER, 0u);
}

int main(void)
{
    unsigned frame;

    s_stack_top = (uint32_t)(uintptr_t)&s_stack[96];
    memset(&s_cpu, 0, sizeof s_cpu);
    CHECK(gl_vita_backend_install());
#if defined(ISAAC_VITA_FBO_DEFERRED_ATTACH)
    CHECK(s_banner_calls == 1u);
    CHECK(!strcmp(s_banner,
        "KAGE VITA FBO DEFERRED ATTACH: on slot=1 shape=color0,tex2d,level0 "
        "flush=draw,clear,owed,read,present,texture,other status=local "
        "bind=noflush receipt=ph120.da"));
    printf("B %s\n", s_banner);
#else
    CHECK(s_banner_calls == 0u);
#endif
    g_tex_image(TEX_A, 480u, 272u);
    g_tex_image(TEX_A2, 480u, 272u);
    g_tex_image(TEX_B, 784u, 472u);
    g_tex_image(TEX_C, 480u, 272u);

    /* 1. attach A then B (different sizes), each consumed by a draw. */
    scenario_begin("attach-a-draw-attach-b-draw");
    pass(TEX_A2, 480u, 272u);
    pass(TEX_B, 784u, 472u);
    pass(TEX_A, 480u, 272u);
    scenario_end();

    /* 2. the read-only attach, then the pass re-attaches: the B attach is
     * superseded when the backend serves the read, consumed when vitaGL
     * does. */
    scenario_begin("read-only-attach-then-reattach");
    get_texel(TEX_B);
    pass(TEX_A, 480u, 272u);
    get_texel(TEX_B);
    get_texel(TEX_B);
    pass(TEX_A2, 480u, 272u);
    scenario_end();

    /* 3. attach + viewport + draw: the viewport does not consume. */
    scenario_begin("attach-viewport-draw");
    g_bind(T_GL_FRAMEBUFFER, FBO);
    g_attach(TEX_B);
    g_viewport(784u, 472u);
    g_attach(TEX_A2);
    g_viewport(480u, 272u);
    g_draw();
    scenario_end();

    /* 4. attach + clear (native: colour bit on an unknown colour). */
    scenario_begin("attach-clear");
    g_bind(T_GL_FRAMEBUFFER, FBO);
    g_attach(TEX_B);
    g_clear(T_GL_COLOR_BUFFER_BIT | T_GL_DEPTH_BUFFER_BIT);
    g_attach(TEX_A);
    g_clear(T_GL_DEPTH_BUFFER_BIT);
    g_attach(TEX_B);
    g_attach(TEX_A2);
    g_draw();
    scenario_end();

    /* 5. attach + delete of the attached texture, and of another one. */
    scenario_begin("attach-delete-texture");
    g_bind(T_GL_FRAMEBUFFER, FBO);
    g_attach(TEX_C);
    g_delete_texture(TEX_C);
    g_attach(TEX_B);
    g_tex_image(TEX_C, 480u, 272u);
    g_attach(TEX_C);
    g_tex_image(TEX_C, 64u, 56u);       /* re-specify after the attach */
    g_draw();
    g_attach(TEX_B);
    g_tex_sub_image(TEX_A);
    g_attach(TEX_A);
    g_draw();
    g_tex_image(TEX_C, 480u, 272u);
    scenario_end();

    /* 6. attach + bind another framebuffer: no flush at the bind; a draw on
     * the other framebuffer still sees the attach done; an attach on the
     * other framebuffer forwards the first one. */
    scenario_begin("attach-bind-other-fbo");
    g_bind(T_GL_FRAMEBUFFER, FBO);
    g_attach(TEX_B);
    g_bind(T_GL_FRAMEBUFFER, 0u);
    g_bind(T_GL_FRAMEBUFFER, FBO);
    g_attach(TEX_A);
    g_bind(T_GL_FRAMEBUFFER, 0u);
    g_draw();
    g_bind(T_GL_FRAMEBUFFER, FBO);
    g_attach(TEX_B);
    g_bind(T_GL_FRAMEBUFFER, FBO2);
    g_status(T_GL_FRAMEBUFFER);
    g_attach(TEX_A2);
    g_status(T_GL_FRAMEBUFFER);
    g_draw();
    g_bind(T_GL_FRAMEBUFFER, FBO);
    g_status(T_GL_FRAMEBUFFER);
    g_draw();
    g_bind(T_GL_READ_FRAMEBUFFER, FBO);
    g_attach(TEX_A);
    g_status(T_GL_READ_FRAMEBUFFER);
    /* A pending attach on FBO must reach vitaGL before another framebuffer
     * is deleted (the delete unbinds and may reset the scene). */
    g_attach(TEX_B);
    g_bind(T_GL_FRAMEBUFFER, FBO2);
    g_delete_framebuffer(FBO2);
    scenario_end();

    /* 7. attach + swap: forwarded before the present. */
    scenario_begin("attach-swap");
    g_bind(T_GL_FRAMEBUFFER, FBO);
    g_attach(TEX_B);
    g_bind(T_GL_FRAMEBUFFER, 0u);
    g_swap();
    g_bind(T_GL_FRAMEBUFFER, FBO);
    g_attach(TEX_A);
    g_draw();
    scenario_end();

    /* 8. attach + a read vitaGL must serve (2x2 at the texture edge is still
     * inside; a format the copy refuses forces the native path). */
    scenario_begin("attach-native-readpixels");
    g_bind(T_GL_FRAMEBUFFER, FBO);
    g_attach(TEX_B);
    {
        static uint8_t texel[4];
        uint32_t a[7] = {0u, 0u, 1u, 1u, 0x1907u /* ORACLE_GL_RGB */,
                         T_GL_UNSIGNED_BYTE, 0u};

        memset(texel, 0xa5, sizeof texel);
        a[6] = (uint32_t)(uintptr_t)texel;
        (void)g_call("glReadPixels", a, 7u);
        printf("G read = %02x%02x%02x%02x\n", texel[0], texel[1], texel[2], texel[3]);
    }
    g_attach(TEX_A);
    g_get_alpha_bits();
    g_attach(TEX_B);
    g_renderbuffer();
    g_attach(TEX_A);
    g_draw();
    scenario_end();

    /* 9. detach, other attachment points and shapes: never deferred. */
    scenario_begin("detach");
    g_bind(T_GL_FRAMEBUFFER, FBO);
    g_attach(TEX_B);
    g_attach(0u);
    g_status(T_GL_FRAMEBUFFER);
    g_attach(TEX_A);
    g_attach_full(T_GL_FRAMEBUFFER, T_GL_DEPTH_ATTACHMENT, T_GL_TEXTURE_2D, TEX_A2, 0u);
    g_attach(TEX_B);
    g_attach_full(T_GL_FRAMEBUFFER, T_GL_COLOR_ATTACHMENT0, T_GL_TEXTURE_2D, TEX_A2, 1u);
    g_attach(TEX_A);
    g_draw();
    g_bind(T_GL_FRAMEBUFFER, 0u);
    g_attach(TEX_B);                    /* default framebuffer: forwarded */
    g_bind(T_GL_FRAMEBUFFER, FBO);
    g_draw();
    scenario_end();

    /* 10. the dry-room frame of FABLE S1: five passes into 480x272
     * surfaces, one GetTexel of the 784x472 surface every 10th frame. */
    scenario_begin("fable-s1-dry-frames");
    for (frame = 0u; frame < 20u; ++frame) {
        unsigned p;

        if (frame % 10u == 0u)
            get_texel(TEX_B);
        for (p = 0u; p < 5u; ++p)
            pass((p & 1u) ? TEX_A2 : TEX_A, 480u, 272u);
        g_bind(T_GL_FRAMEBUFFER, 0u);
        g_draw();
        g_swap();
    }
    scenario_end();

    /* 11. an absorbed depth clear is owed while the attach is still held
     * (CLEAR_ELISION flavours; natively cleared otherwise).  The owed quad
     * is a native clear on the owing framebuffer: the held attach must reach
     * vitaGL before it, and outside its bind window.
     *   a. a detach materialises in the attach wrapper;
     *   b. a read on another framebuffer materialises bound away;
     *   c. a re-attach drops the owed clear (DEPTH_DROP: the held attach is
     *      superseded) or materialises it first (elision only: consumed). */
    scenario_begin("attach-owed-clear");
    g_bind(T_GL_FRAMEBUFFER, FBO);
    g_attach(TEX_B);
    g_clear(T_GL_DEPTH_BUFFER_BIT);
    g_attach(0u);
    g_attach(TEX_A2);
    g_clear(T_GL_DEPTH_BUFFER_BIT);
    g_bind(T_GL_FRAMEBUFFER, 0u);
    g_read();
    g_bind(T_GL_FRAMEBUFFER, FBO);
    g_attach(TEX_B);
    g_clear(T_GL_DEPTH_BUFFER_BIT);
    g_attach(TEX_A);
    g_draw();
    scenario_end();

    /* 12. the same debt settled by the scissor poison (glEnable), bound
     * away.  Last of the GL scenarios: the poison ends clear elision for
     * good. */
    scenario_begin("attach-owed-scissor");
    g_bind(T_GL_FRAMEBUFFER, FBO);
    g_attach(TEX_B);
    g_clear(T_GL_DEPTH_BUFFER_BIT);
    g_bind(T_GL_FRAMEBUFFER, 0u);
    g_enable(T_GL_SCISSOR_TEST);
    g_bind(T_GL_FRAMEBUFFER, FBO);
    g_attach(TEX_A);
    g_draw();
    scenario_end();

    /* 13. KAGE Shutdown after a GetTexel with no present in between
     * (manual_kage_vita.c sub_00560e30): destroy_render_targets unbinds and
     * deletes the manager framebuffer in vitaGL directly, then the backend
     * is uninstalled.  The sequence below is destroy_render_targets': the
     * knob's external flush, then the two native calls.  Without that flush
     * the delete sees the framebuffer without the attach and uninstall
     * attaches into the deleted framebuffer. */
    printf("S shutdown-native-destroy\n");
    get_texel(TEX_B);
#if defined(ISAAC_VITA_FBO_DEFERRED_ATTACH) && \
        !defined(ORACLE_SKIP_EXTERNAL_FLUSH)
    gl_vita_backend_fbo_deferred_attach_external(NULL);
#endif
    {
        GLuint manager = FBO;

        glBindFramebuffer((GLenum)T_GL_FRAMEBUFFER, 0u);
        glDeleteFramebuffers(1, &manager);
    }
    gl_vita_backend_uninstall();
#if defined(ISAAC_VITA_FBO_DEFERRED_ATTACH)
    {
        IsaacVitaFboDeferredAttachCounters c;
        unsigned i;

        gl_vita_backend_fbo_deferred_attach_take(&c);
        printf("D d=%u x=%u s=%u f=", c.deferred, c.dropped, c.status_local);
        for (i = 0u; i < ISAAC_VITA_FBO_DEFER_FLUSH_CLASSES; ++i)
            printf("%s%u", i ? "," : "", c.flushed[i]);
        printf("\n");
    }
#endif
    printf("E done\n");
    return 0;
}
