/* Executable x86 oracle for the shim-side glGetAttribLocation /
 * glGetUniformLocation memo (ISAAC_VITA_GL_LOCATION_CACHE, part of
 * ISAAC_VITA_GL_SHIM_FASTDISPATCH).
 *
 * The fake vitaGL below answers both queries as a pure function of a program
 * model that mutates exactly where pinned vitaGL 73dd57a mutates its program
 * object (glCreateProgram slot reuse, glAttachShader, glLinkProgram,
 * glDeleteProgram).  Every dispatched query is compared with a shadow model
 * evaluation, so a stale cached answer is a hard failure.  The same source is
 * built three times by recomp/test_gl_shim_fastdispatch.py: cache OFF (every
 * query native), cache ON (hits skip vitaGL) and cache ON + VERIFY (hits also
 * ask vitaGL and count mismatches); the native-call expectations differ per
 * variant, the returned values never do. */
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl_bridge.h"
#include "gl_vita_backend.h"
#if !defined(ISAAC_GL_VITA_LOCATION_ORACLE)
# error build with ISAAC_GL_VITA_LOCATION_ORACLE (routes glGetAttribLocation/glAttachShader to the model)
#endif
#include "gl_vita_backend_test_vitagl.h"

#if UINTPTR_MAX != UINT32_MAX
# error gl_vita_location_cache_oracle requires a 32-bit address space
#endif
#if !defined(ISAAC_VITA_GL_SHIM_FASTDISPATCH) || \
    !defined(ISAAC_VITA_PHASE_PROFILE)
# error build with ISAAC_VITA_GL_SHIM_FASTDISPATCH and ISAAC_VITA_PHASE_PROFILE
#endif

#define TOKEN_ATTRIB   0x7e307ce3u
#define TOKEN_UNIFORM  0x7e488552u
#define TOKEN_CREATE   0x7e2c4d40u
#define TOKEN_DELETE   0x7e79d194u
#define TOKEN_LINK     0x7e106822u
#define TOKEN_ATTACH   0x7ec5ca07u
#define TOKEN_COMPILE  0x7edbdc55u
#define TOKEN_DELSHADER 0x7e226ffeu

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "location cache oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

/* ---- program model ------------------------------------------------------ */

#define MODEL_PROGRAMS 16u

typedef struct model_program {
    int alive;
    uint32_t link_gen;
    uint32_t attach_gen;
} model_program;

static model_program s_model[MODEL_PROGRAMS + 1u];
static uint32_t s_model_epoch;
static unsigned s_native_attrib_calls;
static unsigned s_native_uniform_calls;
static unsigned s_native_link_calls;
static unsigned s_native_attach_calls;
static unsigned s_native_create_calls;
static unsigned s_native_delete_calls;

static uint32_t model_hash(const char *name)
{
    uint32_t hash = 2166136261u;
    while (*name) {
        hash ^= (unsigned char)*name++;
        hash *= 16777619u;
    }
    return hash;
}

/* Copy the production bucket functions only to choose deliberate collisions;
 * returned locations are still checked against the independent model. */
static uint32_t address_slot(const char *name, uint32_t program, uint32_t kind)
{
    uint32_t address = (uint32_t)(uintptr_t)name;
    return (address ^ (address >> 6) ^ (address >> 12) ^ program ^ kind) & 63u;
}

static uint32_t content_slot(const char *name, uint32_t program, uint32_t kind)
{
    uint32_t hash = 2166136261u ^ (program * 0x9e3779b9u);
    hash = (hash ^ kind) * 16777619u;
    while (*name)
        hash = (hash ^ (unsigned char)*name++) * 16777619u;
    return (hash ^ (hash >> 16)) & 255u;
}

static GLint model_attrib(GLuint program, const char *name)
{
    const model_program *p;
    if (!program || program > MODEL_PROGRAMS || !name)
        return -1;
    p = &s_model[program];
    if (!p->alive || name[0] == 'x' || !name[0])
        return -1;
    return (GLint)((model_hash(name) ^ (p->link_gen * 7u) ^
                    (p->attach_gen * 13u) ^ (program * 31u)) % 16u);
}

static GLint model_uniform(GLuint program, const char *name)
{
    const model_program *p;
    if (!program || program > MODEL_PROGRAMS || !name)
        return -1;
    p = &s_model[program];
    if (!p->alive || name[0] == 'x' || !name[0])
        return -1;
    /* vitaGL returns -(address of the uniform record): pointer-shaped. */
    return -(GLint)(0x81000000u +
                    ((model_hash(name) * 3u ^ p->link_gen * 11u ^
                      p->attach_gen * 17u ^ program * 29u) % 4096u) * 4u);
}

GLint oracle_glGetAttribLocation(GLuint program, const GLchar *name)
{
    ++s_native_attrib_calls;
    return model_attrib(program, name);
}

GLint oracle_glGetUniformLocation(GLuint program, const GLchar *name)
{
    ++s_native_uniform_calls;
    return model_uniform(program, name);
}

GLuint oracle_glCreateProgram(void)
{
    GLuint i;
    ++s_native_create_calls;
    for (i = 1u; i <= MODEL_PROGRAMS; ++i) {
        if (!s_model[i].alive) {
            s_model[i].alive = 1;
            s_model[i].link_gen = ++s_model_epoch * 0x100u;
            s_model[i].attach_gen = 0u;
            return i;
        }
    }
    return 0u;
}

void oracle_glDeleteProgram(GLuint program)
{
    ++s_native_delete_calls;
    if (program && program <= MODEL_PROGRAMS)
        s_model[program].alive = 0;
}

void oracle_glLinkProgram(GLuint program)
{
    ++s_native_link_calls;
    if (program && program <= MODEL_PROGRAMS)
        ++s_model[program].link_gen;
}

void oracle_glAttachShader(GLuint program, GLuint shader)
{
    ++s_native_attach_calls;
    if (program && program <= MODEL_PROGRAMS)
        s_model[program].attach_gen += shader;
}

/* ---- inert fakes for the rest of the facade ------------------------------ */

void oracle_glBindRenderbuffer(GLenum target, GLuint renderbuffer)
{ (void)target; (void)renderbuffer; }
void oracle_glBindFramebuffer(GLenum target, GLuint framebuffer)
{ (void)target; (void)framebuffer; }
void oracle_glActiveTexture(GLenum texture) { (void)texture; }
void oracle_glBindTexture(GLenum target, GLuint texture)
{ (void)target; (void)texture; }
void oracle_glBlendFuncSeparate(GLenum a, GLenum b, GLenum c, GLenum d)
{ (void)a; (void)b; (void)c; (void)d; }
void oracle_glClearDepth(GLdouble depth) { (void)depth; }
void oracle_glClear(GLbitfield mask) { (void)mask; }
void oracle_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{ (void)r; (void)g; (void)b; (void)a; }
void oracle_glEnable(GLenum capability) { (void)capability; }
void oracle_glDeleteFramebuffers(GLsizei count, const GLuint *framebuffers)
{ (void)count; (void)framebuffers; }
void oracle_glDeleteRenderbuffers(GLsizei count, const GLuint *renderbuffers)
{ (void)count; (void)renderbuffers; }
void oracle_glDrawElements(GLenum mode, GLsizei count, GLenum type,
                           const void *indices)
{ (void)mode; (void)count; (void)type; (void)indices; }
void oracle_glGenRenderbuffers(GLsizei count, GLuint *renderbuffers)
{
    GLsizei i;
    for (i = 0; i < count; ++i)
        renderbuffers[i] = 900u + (GLuint)i;
}
void oracle_glGenTextures(GLsizei count, GLuint *textures)
{
    GLsizei i;
    for (i = 0; i < count; ++i)
        textures[i] = 100u + (GLuint)i;
}
void oracle_glRenderbufferStorage(GLenum target, GLenum format,
                                  GLsizei width, GLsizei height)
{ (void)target; (void)format; (void)width; (void)height; }
void oracle_glGetIntegerv(GLenum name, GLint *value)
{ (void)name; if (value) *value = 0; }
void oracle_glDeleteTextures(GLsizei count, const GLuint *textures)
{ (void)count; (void)textures; }
void oracle_glFramebufferTexture2D(GLenum target, GLenum attachment,
                                   GLenum texture_target, GLuint texture,
                                   GLint level)
{ (void)target; (void)attachment; (void)texture_target; (void)texture;
  (void)level; }
void oracle_glTexImage2D(GLenum target, GLint level, GLint internal_format,
                         GLsizei width, GLsizei height, GLint border,
                         GLenum format, GLenum type, const void *pixels)
{ (void)target; (void)level; (void)internal_format; (void)width;
  (void)height; (void)border; (void)format; (void)type; (void)pixels; }
void oracle_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y,
                            GLsizei width, GLsizei height, GLenum format,
                            GLenum type, const void *pixels)
{ (void)target; (void)level; (void)x; (void)y; (void)width; (void)height;
  (void)format; (void)type; (void)pixels; }
void oracle_glDepthFunc(GLenum function) { (void)function; }
void oracle_glDisableVertexAttribArray(GLuint index) { (void)index; }
void oracle_glGetProgramiv(GLuint program, GLenum name, GLint *value)
{ (void)program; (void)name; if (value) *value = 1; }
void oracle_glGetShaderiv(GLuint shader, GLenum name, GLint *value)
{ (void)shader; (void)name; if (value) *value = 1; }
void oracle_glEnableVertexAttribArray(GLuint index) { (void)index; }
void oracle_glShaderSource(GLuint shader, GLsizei count,
                           const GLchar *const *strings, const GLint *lengths)
{ (void)shader; (void)count; (void)strings; (void)lengths; }
void oracle_glUniform1i(GLint location, GLint value)
{ (void)location; (void)value; }
void oracle_glUniform4fv(GLint location, GLsizei count, const GLfloat *value)
{ (void)location; (void)count; (void)value; }
void oracle_glUniformMatrix4fv(GLint location, GLsizei count,
                               GLboolean transpose, const GLfloat *value)
{ (void)location; (void)count; (void)transpose; (void)value; }
void oracle_glUseProgram(GLuint program) { (void)program; }
void oracle_glVertexAttribPointer(GLuint index, GLint size, GLenum type,
                                  GLboolean normalized, GLsizei stride,
                                  const void *pointer)
{ (void)index; (void)size; (void)type; (void)normalized; (void)stride;
  (void)pointer; }
void oracle_glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{ (void)x; (void)y; (void)width; (void)height; }
void oracle_glNoop(void) {}
GLuint oracle_glReturnUint(void) { return 1u; }
GLint oracle_glReturnInt(void) { return 0; }
const GLubyte *oracle_glReturnString(void)
{
    static const GLubyte value[] = "oracle";
    return value;
}
GLboolean vglIsaacDrawCanonicalQuads(GLsizei count) { (void)count; return 0; }
void vglIsaacMarkColorOffsetShader(uint32_t shader, uint32_t a, uint32_t b,
                                   uint32_t c)
{ (void)shader; (void)a; (void)b; (void)c; }

/* ---- runtime seams the backend needs ------------------------------------ */

static jmp_buf s_fault_jump;
static int s_fault_armed;

void guest_fault(CPU *__restrict c, uint32_t address, const char *message)
{
    c->fault_addr = address;
    c->fault = message;
    if (s_fault_armed)
        longjmp(s_fault_jump, 1);
    fprintf(stderr, "unexpected guest fault %08x: %s\n", address, message);
    abort();
}

int kage_vita_backend_ready(void) { return 1; }

void isaac_gl_vita_oracle_rt_log(const char *format, ...)
{ (void)format; }
void isaac_gl_vita_oracle_rt_profile_report(const char *reason)
{ (void)reason; }
void isaac_gl_vita_oracle_rt_fault(uint32_t result, const char *message)
{ (void)result; (void)message; }
void isaac_vita_log(const char *format, ...) { (void)format; }

/* ---- dispatch helper (same shape as gl_vita_backend_oracle.c) ---------- */

static uint32_t s_stack[96];
static uint32_t s_stack_top;

static int dispatch(CPU *cpu, uint32_t token, const uint32_t *arguments,
                    uint32_t argument_count)
{
    uint32_t index;

    cpu->esp = s_stack_top;
    for (index = argument_count; index != 0u; --index)
        gpush(cpu, arguments[index - 1u]);
    gpush(cpu, 0xaabbccddu);
    if (!guest_gl_dispatch(cpu, token)) {
        fprintf(stderr, "dispatch: token %08x not dispatched (fault=%s)\n",
                token, cpu->fault ? cpu->fault : "-");
        return 0;
    }
    if (cpu->esp != s_stack_top) {
        fprintf(stderr, "dispatch: token %08x left esp %+d (fault=%s)\n",
                token, (int)(cpu->esp - s_stack_top),
                cpu->fault ? cpu->fault : "-");
        return 0;
    }
    return 1;
}

static int query(CPU *cpu, uint32_t token, uint32_t program,
                 const char *name, GLint *result)
{
    uint32_t arguments[2];
    arguments[0] = program;
    arguments[1] = (uint32_t)(uintptr_t)name;
    if (!dispatch(cpu, token, arguments, 2u))
        return 0;
    *result = (GLint)cpu->eax;
    return 1;
}

static unsigned queries_total(void)
{
    return g_isaac_vita_gl_phase_profile_counters.attrib_location +
           g_isaac_vita_gl_phase_profile_counters.uniform_location;
}

static unsigned natives_total(void)
{
    return s_native_attrib_calls + s_native_uniform_calls;
}

/* Query attrib + uniform for (program, name) and compare with the model. */
static int check_pair(CPU *cpu, uint32_t program, const char *name)
{
    GLint value;
    if (!query(cpu, TOKEN_ATTRIB, program, name, &value))
        return 0;
    if (value != model_attrib(program, name)) {
        fprintf(stderr, "attrib mismatch program=%u name=%s got=%d model=%d\n",
                program, name ? name : "(null)", value,
                model_attrib(program, name));
        return 0;
    }
    if (!query(cpu, TOKEN_UNIFORM, program, name, &value))
        return 0;
    if (value != model_uniform(program, name)) {
        fprintf(stderr, "uniform mismatch program=%u name=%s got=%d model=%d\n",
                program, name ? name : "(null)", value,
                model_uniform(program, name));
        return 0;
    }
    return 1;
}

#if defined(ISAAC_VITA_GL_LOCATION_CACHE)
# if defined(ISAAC_VITA_GL_LOCATION_CACHE_VERIFY)
#  define VARIANT "cache+verify"
# else
#  define VARIANT "cache"
# endif
#else
# define VARIANT "no-cache"
#endif

/* Expected native calls for a round of `queries` requests of which `misses`
 * must reach vitaGL in the cache variant. */
static unsigned expected_natives(unsigned queries, unsigned misses)
{
#if defined(ISAAC_VITA_GL_LOCATION_CACHE)
# if defined(ISAAC_VITA_GL_LOCATION_CACHE_VERIFY)
    (void)misses;
    return queries;            /* every hit also asks vitaGL */
# else
    (void)queries;
    return misses;
# endif
#else
    (void)misses;
    return queries;
#endif
}

int main(void)
{
    static const char *const names[6] = {
        "aPosition", "aTexCoord", "aColor", "xUnknownName", "Texture0",
        "Transform"
    };
    static char long_name[65];
    static char high_name[8] = { 'a', (char)0x80, 'b', 0, 0, 0, 0, 0 };
    static char control_name[8] = { 'a', '\t', 'b', 0, 0, 0, 0, 0 };
    static char many_names[300][12];
    static char mutable_name[80] = "aMutableOriginal";
    uint32_t arguments[2];
    CPU cpu;
    GLint value;
    unsigned before;
    unsigned q_before;
    uint32_t p1, p2, p3;
    unsigned i, j, collision_a, collision_b;

    memset(&cpu, 0, sizeof cpu);
    s_stack_top = (uint32_t)(uintptr_t)&s_stack[96];
    memset(long_name, 'L', 64);
    long_name[64] = '\0';
    for (i = 0u; i < 300u; ++i)
        snprintf(many_names[i], sizeof many_names[i], "u%u", i);

    CHECK(gl_vita_backend_install());
    s_native_attrib_calls = 0u;
    s_native_uniform_calls = 0u;
    memset(&g_isaac_vita_gl_phase_profile_counters, 0,
           sizeof g_isaac_vita_gl_phase_profile_counters);

    /* Two programs, then every (program, name) once: all native. */
    CHECK(dispatch(&cpu, TOKEN_CREATE, arguments, 0u));
    p1 = cpu.eax;
    CHECK(dispatch(&cpu, TOKEN_CREATE, arguments, 0u));
    p2 = cpu.eax;
    CHECK(p1 == 1u && p2 == 2u);
    before = natives_total();
    q_before = queries_total();
    for (i = 0u; i < 6u; ++i) {
        CHECK(check_pair(&cpu, p1, names[i]));
        CHECK(check_pair(&cpu, p2, names[i]));
    }
    CHECK(queries_total() == q_before + 24u);
    CHECK(natives_total() == before + expected_natives(24u, 24u));

    /* The same requests again: the cache variant answers all of them. */
    before = natives_total();
    for (i = 0u; i < 6u; ++i) {
        CHECK(check_pair(&cpu, p1, names[i]));
        CHECK(check_pair(&cpu, p2, names[i]));
    }
    CHECK(natives_total() == before + expected_natives(24u, 0u));

    /* glLinkProgram(p1) changes p1's answers; every cached location is
     * invalidated (one generation), so the next round is native again and
     * must equal the new model values. */
    arguments[0] = p1;
    CHECK(dispatch(&cpu, TOKEN_LINK, arguments, 1u));
    CHECK(s_native_link_calls == 1u);
    before = natives_total();
    for (i = 0u; i < 6u; ++i) {
        CHECK(check_pair(&cpu, p1, names[i]));
        CHECK(check_pair(&cpu, p2, names[i]));
    }
    CHECK(natives_total() == before + expected_natives(24u, 24u));
    before = natives_total();
    for (i = 0u; i < 6u; ++i)
        CHECK(check_pair(&cpu, p1, names[i]));
    CHECK(natives_total() == before + expected_natives(12u, 0u));

    /* glAttachShader(p2, 5) changes p2's answers. */
    arguments[0] = p2;
    arguments[1] = 5u;
    CHECK(dispatch(&cpu, TOKEN_ATTACH, arguments, 2u));
    CHECK(s_native_attach_calls == 1u);
    before = natives_total();
    for (i = 0u; i < 6u; ++i)
        CHECK(check_pair(&cpu, p2, names[i]));
    CHECK(natives_total() == before + expected_natives(12u, 12u));

    /* glCompileShader / glDeleteShader are also mutators (the GXP an attached
     * program reads): they only cost a miss round, values stay model-exact. */
    arguments[0] = 9u;
    CHECK(dispatch(&cpu, TOKEN_COMPILE, arguments, 1u));
    before = natives_total();
    for (i = 0u; i < 6u; ++i)
        CHECK(check_pair(&cpu, p2, names[i]));
    CHECK(natives_total() == before + expected_natives(12u, 12u));
    CHECK(dispatch(&cpu, TOKEN_DELSHADER, arguments, 1u));
    before = natives_total();
    for (i = 0u; i < 6u; ++i)
        CHECK(check_pair(&cpu, p2, names[i]));
    CHECK(natives_total() == before + expected_natives(12u, 12u));

    /* Delete p1 and create again: vitaGL reuses the lowest free slot, the new
     * program has a fresh identity behind the same name. */
    arguments[0] = p1;
    CHECK(dispatch(&cpu, TOKEN_DELETE, arguments, 1u));
    CHECK(dispatch(&cpu, TOKEN_CREATE, arguments, 0u));
    p3 = cpu.eax;
    CHECK(p3 == p1);
    before = natives_total();
    for (i = 0u; i < 6u; ++i)
        CHECK(check_pair(&cpu, p3, names[i]));
    CHECK(natives_total() == before + expected_natives(12u, 12u));
    before = natives_total();
    for (i = 0u; i < 6u; ++i)
        CHECK(check_pair(&cpu, p3, names[i]));
    CHECK(natives_total() == before + expected_natives(12u, 0u));

    /* A deleted (dead) program keeps answering -1 through the model; the
     * cache stores that too, and glCreateProgram invalidates it. */
    arguments[0] = p2;
    CHECK(dispatch(&cpu, TOKEN_DELETE, arguments, 1u));
    CHECK(check_pair(&cpu, p2, names[0]));
    CHECK(check_pair(&cpu, p2, names[0]));
    CHECK(model_attrib(p2, names[0]) == -1);

    /* Fail-open shapes reach vitaGL on every request in every variant:
     * program 0, NULL name, empty name, a 64-byte name, a high byte and a
     * control byte in the name. */
    before = natives_total();
    CHECK(check_pair(&cpu, 0u, names[0]));
    CHECK(check_pair(&cpu, 0u, names[0]));
    CHECK(check_pair(&cpu, p3, NULL));
    CHECK(check_pair(&cpu, p3, NULL));
    CHECK(check_pair(&cpu, p3, ""));
    CHECK(check_pair(&cpu, p3, ""));
    CHECK(check_pair(&cpu, p3, long_name));
    CHECK(check_pair(&cpu, p3, long_name));
    CHECK(check_pair(&cpu, p3, high_name));
    CHECK(check_pair(&cpu, p3, high_name));
    CHECK(check_pair(&cpu, p3, control_name));
    CHECK(check_pair(&cpu, p3, control_name));
    CHECK(natives_total() == before + 24u);
    /* A 63-byte name is cacheable. */
    long_name[63] = '\0';
    before = natives_total();
    CHECK(check_pair(&cpu, p3, long_name));
    CHECK(check_pair(&cpu, p3, long_name));
    CHECK(natives_total() == before + expected_natives(4u, 2u));

    /* Cache an address, then change its bytes without changing the pointer.
     * Shortening must stop at the new NUL; extending must check the old NUL. */
    CHECK(check_pair(&cpu, p3, mutable_name));
    CHECK(check_pair(&cpu, p3, mutable_name));
    mutable_name[3] = '\0';
    CHECK(check_pair(&cpu, p3, mutable_name));
    CHECK(check_pair(&cpu, p3, mutable_name));
    strcpy(mutable_name, "aMutableExtended");
    CHECK(check_pair(&cpu, p3, mutable_name));
    CHECK(check_pair(&cpu, p3, mutable_name));
    mutable_name[1] = (char)0x80;
    before = natives_total();
    CHECK(check_pair(&cpu, p3, mutable_name));
    CHECK(check_pair(&cpu, p3, mutable_name));
    CHECK(natives_total() == before + 4u); /* never cache invalid bytes */
    mutable_name[1] = 'M';
    before = natives_total();
    CHECK(check_pair(&cpu, p3, mutable_name));
    CHECK(natives_total() == before + expected_natives(2u, 0u));
    strcpy(mutable_name, "aMutableOriginal");
    CHECK(check_pair(&cpu, p3, mutable_name));
    CHECK(check_pair(&cpu, p3, mutable_name));

    /* Two addresses collide in the 64-entry hints, but not in the content
     * memo. Evicting the hint must still find the original content hit. */
    for (i = 1u; i < 300u; ++i) {
        if (address_slot(many_names[0], p3, 1u) ==
                address_slot(many_names[i], p3, 1u) &&
                content_slot(many_names[0], p3, 1u) !=
                content_slot(many_names[i], p3, 1u))
            break;
    }
    CHECK(i < 300u);
    before = natives_total();
    for (j = 0u; j < 4u; ++j) {
        const char *name = many_names[(j & 1u) ? i : 0u];
        CHECK(query(&cpu, TOKEN_ATTRIB, p3, name, &value));
        CHECK(value == model_attrib(p3, name));
    }
    CHECK(natives_total() == before + expected_natives(4u, 2u));

    /* Overwrite a content slot while the old address hint survives. Same
     * program/kind/generation is insufficient: the current bytes must match. */
    collision_a = collision_b = 300u;
    for (i = 0u; i < 300u && collision_a == 300u; ++i) {
        for (j = i + 1u; j < 300u; ++j) {
            if (content_slot(many_names[i], p3, 2u) ==
                    content_slot(many_names[j], p3, 2u) &&
                    address_slot(many_names[i], p3, 2u) !=
                    address_slot(many_names[j], p3, 2u) &&
                    model_uniform(p3, many_names[i]) !=
                    model_uniform(p3, many_names[j])) {
                collision_a = i;
                collision_b = j;
                break;
            }
        }
    }
    CHECK(collision_a < 300u && collision_b < 300u);
    before = natives_total();
    for (i = 0u; i < 3u; ++i) {
        const char *name = many_names[i == 1u ? collision_b : collision_a];
        CHECK(query(&cpu, TOKEN_UNIFORM, p3, name, &value));
        CHECK(value == model_uniform(p3, name));
    }
    CHECK(natives_total() == before + 3u);

    /* More distinct names than the table holds: values stay exact. */
    for (i = 0u; i < 300u; ++i)
        CHECK(check_pair(&cpu, p3, many_names[i]));
    for (i = 0u; i < 300u; ++i)
        CHECK(check_pair(&cpu, p3, many_names[i]));

    /* Redundancy-cache observation still happens on hits: a hit followed by
     * the same query again is answered identically. */
    CHECK(query(&cpu, TOKEN_UNIFORM, p3, names[4], &value));
    CHECK(value == model_uniform(p3, names[4]));

    /* Counters: every request counted; hits = requests - natives (cache);
     * VERIFY never saw a disagreement. */
    CHECK(queries_total() ==
          g_isaac_vita_gl_phase_profile_counters.attrib_location +
          g_isaac_vita_gl_phase_profile_counters.uniform_location);
#if defined(ISAAC_VITA_GL_LOCATION_CACHE)
    CHECK(g_isaac_vita_gl_phase_profile_counters.location_cache_hit > 0u);
# if defined(ISAAC_VITA_GL_LOCATION_CACHE_VERIFY)
    CHECK(natives_total() == queries_total());
# else
    CHECK(natives_total() + g_isaac_vita_gl_phase_profile_counters.location_cache_hit ==
          queries_total());
# endif
#else
    CHECK(g_isaac_vita_gl_phase_profile_counters.location_cache_hit == 0u);
    CHECK(natives_total() == queries_total());
#endif
    CHECK(g_isaac_vita_gl_phase_profile_counters.location_cache_mismatch == 0u);

    /* Backend teardown resets the memo: the first request after a fresh
     * install is native in every variant. */
    gl_vita_backend_uninstall();
    CHECK(gl_vita_backend_install());
    before = natives_total();
    CHECK(check_pair(&cpu, p3, names[0]));
    CHECK(natives_total() == before + 2u);

    printf("Vita GL location cache oracle: PASS (%s; queries=%u natives=%u "
           "hits=%u mismatches=%u)\n", VARIANT, queries_total(),
           natives_total(),
           g_isaac_vita_gl_phase_profile_counters.location_cache_hit,
           g_isaac_vita_gl_phase_profile_counters.location_cache_mismatch);
    return 0;
}
