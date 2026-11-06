/* Layer-2 recorder oracle: one scripted guest GL sequence, run as direct
 * (recorder OFF build) or deferred inline recorder (ON build, drained only at
 * synchronous calls and the final present), each with and without poisoning
 * the guest client buffers after every call. Native reads are traced with
 * byte hashes at read time; the four trace files must be identical.
 * usage: scenario.exe TRACE_FILE POISON(0|1) */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "gl_bridge.h"
#include "gl_vita_backend.h"
#include "gl_vita_backend_test_vitagl.h"
#include "gl_vita_backend_test_vitagl_undef.h" /* keep oracle_* decls, drop gl* names */
#if defined(ISAAC_VITA_GL_THREAD_RECORD) && ISAAC_VITA_GL_THREAD_RECORD
#include "gl_record_backend.h"
#endif

/* Copied from gl_vita_backend_oracle.c:24,28,41,43,49,93,94 and 2614. */
#define ORACLE_GL_FRAMEBUFFER    0x00008d40u
#define ORACLE_GL_TEXTURE_2D     0x00000de1u
#define ORACLE_GL_RGBA           0x00001908u
#define ORACLE_GL_UNSIGNED_BYTE  0x00001401u
#define ORACLE_GL_FLOAT          0x00001406u
#define ORACLE_GL_TRIANGLES      0x00000004u
#define ORACLE_GL_UNSIGNED_SHORT 0x00001403u

size_t gl_vita_backend_oracle_typed_state(void *out, size_t capacity);

static FILE *s_trace;
static int s_poison;
/* Every client buffer of the script, at ONE fixed address in both
 * executables, so raw guest pointers kept in shim state compare equal. */
#define ARENA_BYTES (1u << 16)
#define ARENA_ADDRESS 0x30000000u
static uint8_t *s_arena;

/* Host analogue of vitaGL's THREAD_SAFE(): every native call must come from
 * the thread that ran the fake vglInit (the render thread in worker mode). */
static DWORD s_vgl_thread;
static void scn_vgl_init(void) { s_vgl_thread = GetCurrentThreadId(); }

static void rt(const char *format, ...)
{
    va_list args;
    if (s_vgl_thread && GetCurrentThreadId() != s_vgl_thread) {
        fprintf(stderr, "native GL call from thread %lu, vglInit thread %lu\n",
                (unsigned long)GetCurrentThreadId(), (unsigned long)s_vgl_thread);
        abort();
    }
    va_start(args, format);
    vfprintf(s_trace, format, args);
    va_end(args);
    fputc('\n', s_trace);
}

/* Guest-visible results are their own ordered stream: deferral legitimately
 * moves native calls relative to them, never within either stream. */
static char s_results[1u << 20];
static size_t s_results_len;
static void rr(const char *format, ...)
{
    va_list args;
    int n;
    va_start(args, format);
    n = vsnprintf(s_results + s_results_len, sizeof s_results - s_results_len - 1u, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= sizeof s_results - s_results_len - 1u) abort();
    s_results_len += (size_t)n;
    s_results[s_results_len++] = '\n';
}

static uint32_t rt_hash(const void *bytes, size_t count)
{
    const uint8_t *p = (const uint8_t *)bytes;
    uint32_t hash = UINT32_C(2166136261);
    while (count--) hash = (hash ^ *p++) * UINT32_C(16777619);
    return hash;
}

/* Arena offsets are stable across the two executables; raw addresses are not.
 * A ring copy would print "ext" and fail the comparison loudly. */
static long rt_offset(const void *pointer)
{
    uintptr_t p = (uintptr_t)pointer, base = (uintptr_t)s_arena;
    return pointer && p >= base && p < base + ARENA_BYTES ? (long)(p - base) : -1L;
}

/* Fake vertex fetch: what vitaGL reads at draw time. */
static const void *s_attrib_pointer[16];
static GLint s_attrib_size[16];
static GLenum s_attrib_type[16];
static GLsizei s_attrib_stride[16];
static uint16_t s_attrib_enabled;

/* Pointer calls are vitaGL STATE (custom_shaders.c:4743..4794); a recorder
 * may rebind onto its copy around a draw. What must match is the effective
 * configuration at the draw, the attributes' relative layout and the bytes
 * read, never the absolute address or the number of state stores. */
static void rt_vertices(uint32_t top)
{
    uint32_t i;
    const uint8_t *first = NULL;
    for (i = 0u; i < 16u; ++i) {
        uint32_t element, stride, bytes;
        if (!(s_attrib_enabled & (1u << i)) || !s_attrib_pointer[i] || !top) continue;
        if (!first) first = (const uint8_t *)s_attrib_pointer[i];
        element = (uint32_t)s_attrib_size[i] * (s_attrib_type[i] == ORACLE_GL_FLOAT ? 4u : 1u);
        stride = s_attrib_stride[i] > 0 ? (uint32_t)s_attrib_stride[i] : element;
        bytes = (top - 1u) * stride + element;
        rt(" v%u cfg=%d,%x,%d rel=%ld n=%u h=%08x", i, s_attrib_size[i], s_attrib_type[i],
           s_attrib_stride[i], (long)((const uint8_t *)s_attrib_pointer[i] - first), bytes,
           rt_hash(s_attrib_pointer[i], bytes));
    }
}

void rt_glActiveTexture(GLenum t) { rt("activeTexture %x", t); oracle_glActiveTexture(t); }
void rt_glBindTexture(GLenum a, GLuint t) { rt("bindTexture %x %u", a, t); oracle_glBindTexture(a, t); }
void rt_glBlendFuncSeparate(GLenum a, GLenum b, GLenum c, GLenum d)
{ rt("blend %x %x %x %x", a, b, c, d); oracle_glBlendFuncSeparate(a, b, c, d); }
void rt_glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{ rt("viewport %d %d %d %d", x, y, w, h); oracle_glViewport(x, y, w, h); }
void rt_glUseProgram(GLuint p) { rt("useProgram %u", p); oracle_glUseProgram(p); }
void rt_glDepthFunc(GLenum f) { rt("depth %x", f); oracle_glDepthFunc(f); }
void rt_glClear(GLbitfield m) { rt("clear %x", m); oracle_glClear(m); }
void rt_glEnable(GLenum c) { rt("enable %x", c); oracle_glEnable(c); }
void rt_glEnableVertexAttribArray(GLuint i)
{ rt("enableAttrib %u", i); if (i < 16u) s_attrib_enabled |= (uint16_t)(1u << i); oracle_glEnableVertexAttribArray(i); }
void rt_glDisableVertexAttribArray(GLuint i)
{ rt("disableAttrib %u", i); if (i < 16u) s_attrib_enabled &= (uint16_t)~(1u << i); oracle_glDisableVertexAttribArray(i); }
void rt_glVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean n,
    GLsizei stride, const void *pointer)
{
    if (i < 16u) {
        s_attrib_pointer[i] = pointer; s_attrib_size[i] = size;
        s_attrib_type[i] = type; s_attrib_stride[i] = stride;
    }
    oracle_glVertexAttribPointer(i, size, type, n, stride, pointer);
}
void rt_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    uint32_t top = 0u, i;
    rt("drawElements %x %d %x", mode, count, type);
    if (type == ORACLE_GL_UNSIGNED_SHORT && indices && count > 0) {
        const uint16_t *index = (const uint16_t *)indices;
        for (i = 0u; i < (uint32_t)count; ++i) if (index[i] + 1u > top) top = index[i] + 1u;
        rt(" i n=%d h=%08x", count, rt_hash(indices, (size_t)count * 2u));
    }
    rt_vertices(top);
    oracle_glDrawElements(mode, count, type, indices);
}
GLboolean rt_vglIsaacDrawCanonicalQuads(GLsizei count)
{
    GLboolean drawn;
    rt("canonical %d", count);
    rt_vertices((uint32_t)count / 6u * 4u);
    drawn = vglIsaacDrawCanonicalQuads(count);
    rt(" -> %u", drawn);
    return drawn;
}
void rt_glUniform1i(GLint l, GLint v) { rt("uniform1i %d %d", l, v); oracle_glUniform1i(l, v); }
void rt_glUniform4fv(GLint l, GLsizei n, const GLfloat *v)
{
    rt("uniform4fv %d %d h=%08x", l, n, n > 0 && v ? rt_hash(v, (size_t)n * 16u) : 0u);
    oracle_glUniform4fv(l, n, v);
}
void rt_glUniformMatrix4fv(GLint l, GLsizei n, GLboolean t, const GLfloat *v)
{
    rt("uniformMatrix4fv %d %d %u h=%08x", l, n, t, n > 0 && v ? rt_hash(v, (size_t)n * 64u) : 0u);
    oracle_glUniformMatrix4fv(l, n, t, v);
}
GLint rt_glGetUniformLocation(GLuint p, const GLchar *name)
{
    GLint r = oracle_glGetUniformLocation(p, name);
    rt("getUniformLocation %u h=%08x -> %d", p, rt_hash(name, strlen(name)), r);
    return r;
}
GLint rt_glGetAttribLocation(GLuint p, const GLchar *name)
{
    /* Distinct per (program, name) so a stale memo answer is visible. */
    uint32_t h = rt_hash(name, strlen(name)) ^ p;
    GLint r = (GLint)(h % 7u) - 1;
    rt("getAttribLocation %u h=%08x -> %d", p, h, r);
    return r;
}
static void rt_names(const char *what, GLsizei n, const GLuint *names)
{
    rt("%s %d h=%08x", what, n, n > 0 && names ? rt_hash(names, (size_t)n * 4u) : 0u);
}
void rt_glDeleteTextures(GLsizei n, const GLuint *v) { rt_names("deleteTextures", n, v); oracle_glDeleteTextures(n, v); }
void rt_glDeleteFramebuffers(GLsizei n, const GLuint *v) { rt_names("deleteFramebuffers", n, v); oracle_glDeleteFramebuffers(n, v); }
void rt_glDeleteRenderbuffers(GLsizei n, const GLuint *v) { rt_names("deleteRenderbuffers", n, v); oracle_glDeleteRenderbuffers(n, v); }

/* vitaGL status contract: framebuffers.c:530..546, shared.h:1365..1436. */
static GLuint s_fb_write, s_fb_name[16], s_fb_tex[16];
static GLuint *rt_fb_slot(GLuint name)
{
    uint32_t i;
    for (i = 0u; i < 16u; ++i) if (s_fb_name[i] == name) return &s_fb_tex[i];
    for (i = 0u; i < 16u; ++i) if (!s_fb_name[i]) { s_fb_name[i] = name; s_fb_tex[i] = 0u; return &s_fb_tex[i]; }
    abort();
}
void rt_glBindFramebuffer(GLenum target, GLuint name)
{
    rt("bindFramebuffer %x %u", target, name);
    if (target == ORACLE_GL_FRAMEBUFFER || target == 0x8ca9u) s_fb_write = name;
    oracle_glBindFramebuffer(target, name);
}
void rt_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum tt, GLuint tex, GLint level)
{
    rt("framebufferTexture2D %x %x %x %u %d", target, attachment, tt, tex, level);
    if (s_fb_write && attachment == 0x8ce0u) *rt_fb_slot(s_fb_write) = tex;
    oracle_glFramebufferTexture2D(target, attachment, tt, tex, level);
}
GLenum rt_glCheckFramebufferStatus(GLenum target)
{
    GLenum r = (!s_fb_write || *rt_fb_slot(s_fb_write)) ? 0x8cd5u : 0x8cd6u;
    rt("checkFramebufferStatus %x -> %x", target, r);
    return r;
}
void rt_glGenRenderbuffers(GLsizei n, GLuint *names)
{
    oracle_glGenRenderbuffers(n, names);
    rt_names("genRenderbuffers", n, names);
}
void rt_glBindRenderbuffer(GLenum t, GLuint n) { rt("bindRenderbuffer %x %u", t, n); oracle_glBindRenderbuffer(t, n); }
void rt_glRenderbufferStorage(GLenum t, GLenum f, GLsizei w, GLsizei h)
{ rt("renderbufferStorage %x %x %d %d", t, f, w, h); oracle_glRenderbufferStorage(t, f, w, h); }
void rt_glGetIntegerv(GLenum name, GLint *value)
{
    oracle_glGetIntegerv(name, value);
    rt("getIntegerv %x h=%08x", name, rt_hash(value, 16u));
}
void rt_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void *pixels)
{
    rt("readPixels %d %d %d %d %x %x off=%ld", x, y, w, h, f, t, rt_offset(pixels));
#if defined(ISAAC_VITA_FBO_READBACK_FIX)
    oracle_glReadPixels(x, y, w, h, f, t, pixels);
#endif
}
GLuint rt_glCreateProgram(void) { GLuint p = oracle_glCreateProgram(); rt("createProgram -> %u", p); return p; }
void rt_glLinkProgram(GLuint p) { rt("linkProgram %u", p); oracle_glLinkProgram(p); }
void rt_glDeleteProgram(GLuint p) { rt("deleteProgram %u", p); oracle_glDeleteProgram(p); }

/* ---- the script ---------------------------------------------------------- */
#define A(offset) ((guest_gl_addr)(uintptr_t)(s_arena + (offset)))
#define POISON(offset, bytes) do { if (s_poison) memset(s_arena + (offset), 0xa5, (bytes)); } while (0)
#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
/* The device present (kage_vita_backend_present): no drain, wait for the
 * previous present closure, post this one; one frame in flight. */
static uint32_t s_scn_present_pos, s_scn_presents;
static void scn_present_closure(void *unused) { (void)unused; isaac_gl_record_worker_frame_end(0u); }
# define PRESENT() (isaac_gl_record_worker_wait(s_scn_present_pos), ++s_scn_presents, \
    (void)(s_scn_present_pos = isaac_gl_record_worker_post(scn_present_closure, NULL)))
#elif defined(ISAAC_VITA_GL_THREAD_RECORD) && ISAAC_VITA_GL_THREAD_RECORD
# define PRESENT() isaac_gl_record_drain()
#else
# define PRESENT() ((void)0)
#endif
enum { AR_NAMES = 0, AR_VALUES = 4096, AR_INDICES = 8192, AR_VERTICES = 12288, AR_DELETES = 32768, AR_OUT = 33792 };

static void put_name(uint32_t offset, const char *text) { memcpy(s_arena + offset, text, strlen(text) + 1u); }

/* Copied from gl_vita_backend_oracle.c oracle_dispatch_with_return. */
static int dispatch(CPU *cpu, uint32_t stack_top, uint32_t token,
    const uint32_t *arguments, uint32_t count, uint32_t return_word)
{
    uint32_t index;
    cpu->esp = stack_top;
    for (index = count; index != 0u; --index) gpush(cpu, arguments[index - 1u]);
    gpush(cpu, return_word);
    if (!guest_gl_dispatch(cpu, token)) return 0;
    return cpu->esp == stack_top;
}

/* Every script call takes the real guest path: resolver token, x86 stack
 * frame, generated adapter, installed table (recorder or shim). */
static CPU *s_cpu;
static uint32_t s_stack_top;
static uint32_t scn_call(uint32_t return_word, const char *name, uint32_t count, ...)
{
    uint32_t arguments[9], i, token = guest_gl_resolve(name);
    va_list list;
    va_start(list, count);
    for (i = 0u; i < count; ++i) arguments[i] = va_arg(list, uint32_t);
    va_end(list);
    if (!token || count > 9u ||
            !dispatch(s_cpu, s_stack_top, token, arguments, count, return_word)) {
        rt("dispatch failed %s", name);
        fclose(s_trace);
        exit(3);
    }
    return s_cpu->eax;
}
#define SCN(...) scn_call(0xaabbccddu, __VA_ARGS__)

#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
static int s_install_result;
static void scn_install_on_consumer(void *unused) { (void)unused; s_install_result = gl_vita_backend_install(); }
static int scn_install(void) { isaac_gl_record_worker_call(scn_install_on_consumer, NULL); return s_install_result; }
#else
static int scn_install(void) { return gl_vita_backend_install(); }
#endif

static void fill_vertices(uint32_t seed, uint32_t bytes)
{
    uint32_t i;
    for (i = 0u; i < bytes; ++i) s_arena[AR_VERTICES + i] = (uint8_t)(seed + i * 7u);
}

/* Attribute batch (ISAAC_VITA_GL_RECORD_ATTRIB_BATCH): one Shader attribute
 * replay.  Recorder variants hand it to the recorder in ONE call
 * (isaac_gl_record_attribs_enable/_disable, after ready() accepted the table
 * the recorder installed); the direct variant, and every variant without
 * the knob, issue the per-call sequence the replay's own loop makes
 * (host_vita_shader_attrib_fastpath.c: enable(loc), pointer(loc, ncomp,
 * GL_FLOAT, 0, stride, base), base += 4 * ncomp; disable(loc)).  The traces
 * must be identical. */
#if defined(ISAAC_VITA_GL_RECORD_ATTRIB_BATCH) && \
    defined(ISAAC_VITA_GL_THREAD_RECORD) && ISAAC_VITA_GL_THREAD_RECORD
# define SCN_BATCH 1
static uint32_t s_scn_batches[2];   /* enable, disable */
static void scn_batch_ready(uint32_t count, int enable)
{
    if (!isaac_gl_record_attribs_batch_ready(isaac_gl_record_oracle_installed(),
                                             count, enable)) {
        rt("batch refused count=%u enable=%d", count, enable);
        fclose(s_trace);
        exit(10);
    }
    ++s_scn_batches[enable ? 0 : 1];
}
/* The producer shadow right after a batch must be what the per-call
 * record_gl* notes leave (gl_record_backend.c shadow_enable/shadow_pointer:
 * an index >= 16 clears every known bit, the last write of a slot wins).
 * The trace cannot see it: a wrong note (say, a pointer that does not
 * advance) only resizes later draw snapshots within bytes no draw reads. */
static void scn_batch_check_shadow(uint32_t count, const guest_gl_int *locations,
    const uint8_t *components, int32_t stride, uint32_t base, int enable)
{
    uint32_t i, k, after = 0u, pointer = base, got[8] = {0u};
    for (i = 0u; i < count; ++i)
        if ((uint32_t)locations[i] >= 16u) after = i + 1u;
    for (i = 0u; i < count; ++i) {
        uint32_t loc = (uint32_t)locations[i], known = i >= after ? 1u : 0u;
        int last = loc < 16u, bad;
        for (k = i + 1u; last && k < count; ++k)
            if ((uint32_t)locations[k] == loc) last = 0;
        if (last) {
            if (!isaac_gl_record_oracle_shadow_slot(0, loc, got)) {
                bad = 1;
            } else {
                bad = got[5] != (enable ? 1u : 0u) || got[6] != known;
                if (enable)
                    bad |= got[0] != pointer || got[1] != ORACLE_GL_FLOAT ||
                        got[2] != components[i] || got[3] != (uint32_t)stride ||
                        got[4] != 0u || got[7] != known;
            }
            if (bad) {
                rt("batch producer shadow slot %u (element %u of %u, enable=%d) "
                   "= %x,%x,%x,%x,%x en=%u known=%u,%u", loc, i, count, enable,
                   got[0], got[1], got[2], got[3], got[4], got[5], got[6], got[7]);
                fclose(s_trace);
                exit(14);
            }
        }
        if (enable) pointer += (uint32_t)components[i] * 4u;
    }
}
#endif
static void scn_attribs_enable(uint32_t count, const guest_gl_int *locations,
    const uint8_t *components, int32_t stride, uint32_t base)
{
#if defined(SCN_BATCH)
    scn_batch_ready(count, 1);
    isaac_gl_record_attribs_enable(count, locations, components, stride, base);
    scn_batch_check_shadow(count, locations, components, stride, base, 1);
#else
    uint32_t i;
    for (i = 0u; i < count; ++i) {
        SCN("glEnableVertexAttribArray", 1u, (uint32_t)locations[i]);
        SCN("glVertexAttribPointer", 6u, (uint32_t)locations[i], (uint32_t)components[i],
            (uint32_t)ORACLE_GL_FLOAT, (uint32_t)0u, (uint32_t)stride, base);
        base += (uint32_t)components[i] * 4u;
    }
#endif
}
static void scn_attribs_disable(uint32_t count, const guest_gl_int *locations)
{
#if defined(SCN_BATCH)
    scn_batch_ready(count, 0);
    isaac_gl_record_attribs_disable(count, locations);
    scn_batch_check_shadow(count, locations, NULL, 0, 0u, 0);
#else
    uint32_t i;
    for (i = 0u; i < count; ++i)
        SCN("glDisableVertexAttribArray", 1u, (uint32_t)locations[i]);
#endif
}

static void scn_batch_section(void)
{
    /* ColorOffset-like components {2,2,4,4,3,1,3} (copied from
     * gl_vita_backend_oracle.c s_direct_specs[0]),
     * cycled for 8 and 16. */
    static const uint8_t mix[7] = {2u, 2u, 4u, 4u, 3u, 1u, 3u};
    static const uint16_t quad[6] = {0, 1, 2, 2, 1, 3};
    static const guest_gl_int hostile[5] = {-1, 16, 3, 3, 9};
    static const uint8_t hostile_components[5] = {4u, 1u, 2u, 3u, 4u};
    static const uint32_t counts[4] = {1u, 7u, 8u, 16u};
    guest_gl_int locations[16];
    uint8_t components[16];
    uint32_t i, c, n, stride;

#if defined(SCN_BATCH)
    {
        /* ready() refuses a count outside 1..16 and a foreign table. */
        guest_gl_backend foreign = *isaac_gl_record_oracle_installed();
        foreign.glVertexAttribPointer = NULL;
        if (isaac_gl_record_attribs_batch_ready(isaac_gl_record_oracle_installed(), 0u, 1) ||
                isaac_gl_record_attribs_batch_ready(isaac_gl_record_oracle_installed(), 17u, 0) ||
                isaac_gl_record_attribs_batch_ready(&foreign, 7u, 1) ||
                !isaac_gl_record_attribs_batch_ready(&foreign, 7u, 0)) {
            rt("batch ready() accepted a refused shape");
            fclose(s_trace);
            exit(11);
        }
    }
#endif
    /* Counts 1, 7, 8, 16: enable, a draw that snapshots every enabled
     * attribute (its client ring rewritten right after the draw, before any
     * replay), disable. */
    for (c = 0u; c < 4u; ++c) {
        n = counts[c];
        stride = 0u;
        for (i = 0u; i < n; ++i) {
            locations[i] = (guest_gl_int)(n == 8u ? 7u - i : i);
            components[i] = mix[i % 7u];
            stride += components[i] * 4u;
        }
        memcpy(s_arena + AR_INDICES, quad, sizeof quad);
        fill_vertices(50u + n, 4u * stride + 64u);
        scn_attribs_enable(n, locations, components, (int32_t)stride, A(AR_VERTICES + 4u * (n & 3u)));
        SCN("glDrawElements", 4u, (uint32_t)(ORACLE_GL_TRIANGLES), (uint32_t)(6),
            (uint32_t)(ORACLE_GL_UNSIGNED_SHORT), (uint32_t)(A(AR_INDICES)));
        fill_vertices(150u + n, 4u * stride + 64u);   /* KAGE reuse */
        POISON(AR_INDICES, 12u);
        scn_attribs_disable(n, locations);
    }
    /* Cancel pairs for the coalescer: enable -> disable -> enable, then a
     * snapshot draw and an S record right behind the last batch. */
    for (i = 0u; i < 3u; ++i) {
        locations[i] = (guest_gl_int)i;
        components[i] = mix[i];
    }
    scn_attribs_enable(3u, locations, components, 32, A(AR_VERTICES));
    scn_attribs_disable(3u, locations);
    memcpy(s_arena + AR_INDICES, quad, sizeof quad);
    fill_vertices(77u, 4u * 32u + 64u);
    scn_attribs_enable(3u, locations, components, 32, A(AR_VERTICES));
    rr("ret batch status %x", SCN("glCheckFramebufferStatus", 1u, (uint32_t)(ORACLE_GL_FRAMEBUFFER)));
    scn_attribs_enable(3u, locations, components, 32, A(AR_VERTICES + 16u));
    SCN("glDrawElements", 4u, (uint32_t)(ORACLE_GL_TRIANGLES), (uint32_t)(6),
        (uint32_t)(ORACLE_GL_UNSIGNED_SHORT), (uint32_t)(A(AR_INDICES)));
    fill_vertices(78u, 4u * 32u + 64u);
    POISON(AR_INDICES, 12u);
    /* -1, 16 and a duplicated location in one replay, negative stride (the
     * recorder's shadows forget every slot here; the loop below makes all 16
     * known again). */
    scn_attribs_enable(5u, hostile, hostile_components, -8, A(AR_VERTICES + 1024u));
    scn_attribs_disable(5u, hostile);
    /* Batches only, every count 1..16 in turn, until the ring has wrapped
     * several times: each wrap marker is followed by a batch record (worker
     * variants count them below).  Without the knob every variant issues the
     * same per-call sequence, ~26 records per pair under the worker's random
     * jitter, so a short run keeps the knob-OFF suite at its old duration. */
#if defined(ISAAC_VITA_GL_RECORD_ATTRIB_BATCH)
# define SCN_BATCH_WRAP_PAIRS 9000u
#else
# define SCN_BATCH_WRAP_PAIRS 48u
#endif
    for (i = 0u; i < SCN_BATCH_WRAP_PAIRS; ++i) {
        uint32_t k;
        n = 1u + i % 16u;
        for (k = 0u; k < n; ++k) {
            locations[k] = (guest_gl_int)((k + i) % 16u);
            components[k] = mix[(k + i) % 7u];
        }
        scn_attribs_enable(n, locations, components, 4 * (int32_t)n, A(AR_VERTICES + 4u * (i & 7u)));
        scn_attribs_disable(n, locations);
    }
    /* Back to the attribute state the script had before this section (the
     * sections below draw with it): 0 and 1 enabled, three pointers. */
    SCN("glEnableVertexAttribArray", 1u, (uint32_t)(0u));
    SCN("glEnableVertexAttribArray", 1u, (uint32_t)(1u));
    SCN("glVertexAttribPointer", 6u, (uint32_t)(0u), (uint32_t)(2), (uint32_t)(ORACLE_GL_FLOAT), (uint32_t)(0u), (uint32_t)(20), (uint32_t)(A(AR_VERTICES)));
    SCN("glVertexAttribPointer", 6u, (uint32_t)(1u), (uint32_t)(2), (uint32_t)(ORACLE_GL_FLOAT), (uint32_t)(0u), (uint32_t)(20), (uint32_t)(A(AR_VERTICES + 8u)));
    SCN("glVertexAttribPointer", 6u, (uint32_t)(2u), (uint32_t)(4), (uint32_t)(ORACLE_GL_UNSIGNED_BYTE), (uint32_t)(1u), (uint32_t)(20), (uint32_t)(A(AR_VERTICES + 16u)));
}

static void run(CPU *cpu, uint32_t stack_top)
{
    static const char *const attribs[] = {"Position", "TexCoord", "Color", "Position", "TexCoord"};
    static const uint16_t quad[6] = {0, 1, 2, 2, 1, 3};
    uint32_t i, program, rbo, draw[4];
    guest_gl_int location;
    char text[96];

    s_cpu = cpu;
    s_stack_top = stack_top;
    if (!scn_install()) { rt("install failed"); return; }
    SCN("glViewport", 4u, (uint32_t)(0), (uint32_t)(0), (uint32_t)(960), (uint32_t)(540));
    SCN("glBlendFuncSeparate", 4u, (uint32_t)(0x302u), (uint32_t)(0x303u), (uint32_t)(1u), (uint32_t)(0x303u));
    SCN("glBlendFuncSeparate", 4u, (uint32_t)(0x302u), (uint32_t)(0x303u), (uint32_t)(1u), (uint32_t)(0x303u));   /* typed hit */
    SCN("glDepthFunc", 1u, (uint32_t)(0x203u));
    SCN("glEnable", 1u, (uint32_t)(0x0be2u));
    program = SCN("glCreateProgram", 0u);
    rr("ret createProgram %u", program);
    SCN("glLinkProgram", 1u, (uint32_t)(program));
    SCN("glUseProgram", 1u, (uint32_t)(program));
    SCN("glUseProgram", 1u, (uint32_t)(program));                              /* redundancy skip */

    /* Locations: hits, repeats, a 64-byte uncacheable name, a large live set. */
    for (i = 0u; i < 5u; ++i) {
        put_name(AR_NAMES, attribs[i]);
        location = SCN("glGetAttribLocation", 2u, (uint32_t)(program), (uint32_t)(A(AR_NAMES)));
        rr("ret attrib %s %d", attribs[i], location);
        POISON(AR_NAMES, 64u);
    }
    put_name(AR_NAMES, "Transform");
    location = SCN("glGetUniformLocation", 2u, (uint32_t)(program), (uint32_t)(A(AR_NAMES)));
    rr("ret uniform Transform %d", location);
    POISON(AR_NAMES, 64u);
    memset(text, 'x', 64u); text[64] = 0;
    put_name(AR_NAMES, text);
    rr("ret uniform long %d", SCN("glGetUniformLocation", 2u, (uint32_t)(program), (uint32_t)(A(AR_NAMES))));
    POISON(AR_NAMES, 80u);
    for (i = 0u; i < 300u; ++i) {
        sprintf(text, "live_%u", i % 150u);  /* two passes over 150 live names */
        put_name(AR_NAMES, text);
        rr("ret attrib %s %d", text, SCN("glGetAttribLocation", 2u, (uint32_t)(program), (uint32_t)(A(AR_NAMES))));
        POISON(AR_NAMES, 32u);
    }

    /* Uniforms: -1, values, redundancy repeat, matrix, sampler int. */
    for (i = 0u; i < 16u; ++i) ((float *)(s_arena + AR_VALUES))[i] = (float)i * 0.5f;
    SCN("glUniform4fv", 3u, (uint32_t)(-1), (uint32_t)(1), (uint32_t)(A(AR_VALUES)));
    SCN("glUniform4fv", 3u, (uint32_t)(location), (uint32_t)(1), (uint32_t)(A(AR_VALUES)));
    POISON(AR_VALUES, 64u);
    for (i = 0u; i < 16u; ++i) ((float *)(s_arena + AR_VALUES))[i] = (float)i * 0.5f;
    SCN("glUniform4fv", 3u, (uint32_t)(location), (uint32_t)(1), (uint32_t)(A(AR_VALUES)));                /* skip candidate */
    POISON(AR_VALUES, 64u);
    for (i = 0u; i < 16u; ++i) ((float *)(s_arena + AR_VALUES))[i] = (float)i;
    SCN("glUniformMatrix4fv", 4u, (uint32_t)(location), (uint32_t)(1), (uint32_t)(0u), (uint32_t)(A(AR_VALUES)));
    POISON(AR_VALUES, 64u);
    SCN("glUniform1i", 2u, (uint32_t)(location), (uint32_t)(0));

    /* Attributes: interleaved 20-byte layout, hostile index and stride. */
    for (i = 0u; i < 3u; ++i) SCN("glEnableVertexAttribArray", 1u, (uint32_t)(i));
    SCN("glVertexAttribPointer", 6u, (uint32_t)(16u), (uint32_t)(2), (uint32_t)(ORACLE_GL_FLOAT), (uint32_t)(0u), (uint32_t)(20), (uint32_t)(A(AR_VERTICES)));
    SCN("glVertexAttribPointer", 6u, (uint32_t)(1u), (uint32_t)(2), (uint32_t)(ORACLE_GL_FLOAT), (uint32_t)(0u), (uint32_t)(-4), (uint32_t)(A(AR_VERTICES + 8u)));
    SCN("glVertexAttribPointer", 6u, (uint32_t)(0u), (uint32_t)(2), (uint32_t)(ORACLE_GL_FLOAT), (uint32_t)(0u), (uint32_t)(20), (uint32_t)(A(AR_VERTICES)));
    SCN("glVertexAttribPointer", 6u, (uint32_t)(1u), (uint32_t)(2), (uint32_t)(ORACLE_GL_FLOAT), (uint32_t)(0u), (uint32_t)(20), (uint32_t)(A(AR_VERTICES + 8u)));
    SCN("glVertexAttribPointer", 6u, (uint32_t)(2u), (uint32_t)(4), (uint32_t)(ORACLE_GL_UNSIGNED_BYTE), (uint32_t)(1u), (uint32_t)(20), (uint32_t)(A(AR_VERTICES + 16u)));
    SCN("glActiveTexture", 1u, (uint32_t)(0x84c0u));
    SCN("glBindTexture", 2u, (uint32_t)(ORACLE_GL_TEXTURE_2D), (uint32_t)(5u));
    SCN("glBindTexture", 2u, (uint32_t)(ORACLE_GL_TEXTURE_2D), (uint32_t)(5u));           /* typed hit */

    /* Draws: canonical call site (return RVA), then a plain one. The client
     * vertex ring is reused right after each draw (KAGE behaviour). */
    for (i = 0u; i < 12u; ++i) ((uint16_t *)(s_arena + AR_INDICES))[i] = (uint16_t)(quad[i % 6u] + 4u * (i / 6u));
    fill_vertices(1u, 8u * 20u);
    draw[0] = ORACLE_GL_TRIANGLES; draw[1] = 12u; draw[2] = ORACLE_GL_UNSIGNED_SHORT; draw[3] = A(AR_INDICES);
    /* 0x0056d794 = KAGE_VITA_CANONICAL_QUAD_RETURN_RVA_GLOBAL (kage_vita_canonical_quads.h:7). */
    scn_call(0x0056d794u, "glDrawElements", 4u, draw[0], draw[1], draw[2], draw[3]);
    POISON(AR_VERTICES, 8u * 20u); POISON(AR_INDICES, 24u);
    memcpy(s_arena + AR_INDICES, quad, sizeof quad);
    fill_vertices(9u, 4u * 20u);
    SCN("glDrawElements", 4u, (uint32_t)(ORACLE_GL_TRIANGLES), (uint32_t)(6), (uint32_t)(ORACLE_GL_UNSIGNED_SHORT), (uint32_t)(A(AR_INDICES)));
    POISON(AR_VERTICES, 4u * 20u); POISON(AR_INDICES, 12u);
    /* KAGE pattern without poison: the same arrays rewritten right after each
     * draw, before any replay. Each draw must see its own contents. */
    for (i = 0u; i < 3u; ++i) {
        memcpy(s_arena + AR_INDICES, quad, sizeof quad);
        fill_vertices(33u + 11u * i, 4u * 20u);
        SCN("glDrawElements", 4u, (uint32_t)(ORACLE_GL_TRIANGLES), (uint32_t)(6),
            (uint32_t)(ORACLE_GL_UNSIGNED_SHORT), (uint32_t)(A(AR_INDICES)));
    }
    fill_vertices(200u, 4u * 20u);  /* rewritten after the last record */
    SCN("glDisableVertexAttribArray", 1u, (uint32_t)(2u));
    scn_batch_section();

    /* Framebuffer status: unknown -> S, attached -> shadow, default -> shadow. */
    SCN("glBindFramebuffer", 2u, (uint32_t)(ORACLE_GL_FRAMEBUFFER), (uint32_t)(9u));
    rr("ret status %x", SCN("glCheckFramebufferStatus", 1u, (uint32_t)(ORACLE_GL_FRAMEBUFFER)));
    SCN("glFramebufferTexture2D", 5u, (uint32_t)(ORACLE_GL_FRAMEBUFFER), (uint32_t)(0x8ce0u), (uint32_t)(ORACLE_GL_TEXTURE_2D), (uint32_t)(5u), (uint32_t)(0));
    rr("ret status %x", SCN("glCheckFramebufferStatus", 1u, (uint32_t)(ORACLE_GL_FRAMEBUFFER)));
    SCN("glClear", 1u, (uint32_t)(0x4100u));
    SCN("glBindFramebuffer", 2u, (uint32_t)(ORACLE_GL_FRAMEBUFFER), (uint32_t)(0u));
    rr("ret status %x", SCN("glCheckFramebufferStatus", 1u, (uint32_t)(ORACLE_GL_FRAMEBUFFER)));
    ((uint32_t *)(s_arena + AR_DELETES))[0] = 7u; ((uint32_t *)(s_arena + AR_DELETES))[1] = 8u;
    SCN("glDeleteTextures", 2u, (uint32_t)(2), (uint32_t)(A(AR_DELETES)));
    POISON(AR_DELETES, 8u);

    /* Renderbuffer: Gen (S), bind/storage, queries (no record), delete. */
    SCN("glGenRenderbuffers", 2u, (uint32_t)(1), (uint32_t)(A(AR_OUT)));
    rbo = ((uint32_t *)(s_arena + AR_OUT))[0];
    rr("ret genRenderbuffers %u", rbo);
    SCN("glBindRenderbuffer", 2u, (uint32_t)(0x8d41u), (uint32_t)(rbo));
    SCN("glRenderbufferStorage", 4u, (uint32_t)(0x8d41u), (uint32_t)(0x81a6u), (uint32_t)(64), (uint32_t)(32));
    for (i = 0u; i < 10u; ++i) {
        SCN("glBindRenderbuffer", 2u, (uint32_t)(0x8d41u), (uint32_t)(rbo));
        SCN("glGetRenderbufferParameteriv", 3u, (uint32_t)(0x8d41u), (uint32_t)(0x8d42u), (uint32_t)(A(AR_OUT + 4u)));
        SCN("glGetRenderbufferParameteriv", 3u, (uint32_t)(0x8d41u), (uint32_t)(0x8d43u), (uint32_t)(A(AR_OUT + 8u)));
        rr("ret rbo %u %u", ((uint32_t *)(s_arena + AR_OUT))[1], ((uint32_t *)(s_arena + AR_OUT))[2]);
    }
    ((uint32_t *)(s_arena + AR_DELETES))[0] = rbo;
    SCN("glDeleteRenderbuffers", 2u, (uint32_t)(1), (uint32_t)(A(AR_DELETES)));
    POISON(AR_DELETES, 4u);

    /* Synchronous reads. */
    memset(s_arena + AR_OUT, 0, 32u);
    SCN("glGetIntegerv", 2u, (uint32_t)(0x0ba2u), (uint32_t)(A(AR_OUT)));
    rr("ret viewport h=%08x", rt_hash(s_arena + AR_OUT, 16u));
    SCN("glReadPixels", 7u, (uint32_t)(1), (uint32_t)(2), (uint32_t)(1), (uint32_t)(1), (uint32_t)(ORACLE_GL_RGBA), (uint32_t)(ORACLE_GL_UNSIGNED_BYTE), (uint32_t)(A(AR_OUT + 16u)));
    rr("ret pixel h=%08x", rt_hash(s_arena + AR_OUT + 16u, 4u));

    /* Relink fence, then a location that must be asked again. */
    SCN("glLinkProgram", 1u, (uint32_t)(program));
    put_name(AR_NAMES, "Position");
    rr("ret attrib after relink %d", SCN("glGetAttribLocation", 2u, (uint32_t)(program), (uint32_t)(A(AR_NAMES))));
    POISON(AR_NAMES, 16u);

    /* Overflow: more than one ring of distinct uniform payloads. */
    for (i = 0u; i < 9000u; ++i) {
        uint32_t k;
        for (k = 0u; k < 16u; ++k) ((float *)(s_arena + AR_VALUES))[k] = (float)(i + k);
        SCN("glUniformMatrix4fv", 4u, (uint32_t)(location), (uint32_t)(1), (uint32_t)(0u), (uint32_t)(A(AR_VALUES)));
        POISON(AR_VALUES, 64u);
    }
    /* Census-shaped presents: 10 render-target switches
     * (bind FBO + RB, W/H queries, attach, status, clear), 20 location
     * lookups, 30 draws with uniforms/textures, a 1x1 readback every 6th. */
    {
        uint32_t rbo2, present, k, syncs_before = 0u;
#if defined(ISAAC_VITA_GL_THREAD_RECORD) && ISAAC_VITA_GL_THREAD_RECORD
        uint32_t st[8];
#endif
        SCN("glGenRenderbuffers", 2u, (uint32_t)(1), (uint32_t)(A(AR_OUT)));
        rbo2 = ((uint32_t *)(s_arena + AR_OUT))[0];
        SCN("glBindRenderbuffer", 2u, (uint32_t)(0x8d41u), (uint32_t)(rbo2));
        SCN("glRenderbufferStorage", 4u, (uint32_t)(0x8d41u), (uint32_t)(0x81a6u), (uint32_t)(1024), (uint32_t)(1024));
        PRESENT();
        for (present = 0u; present < 60u; ++present) {
#if defined(ISAAC_VITA_GL_THREAD_RECORD) && ISAAC_VITA_GL_THREAD_RECORD
            if (present == 1u) {   /* present 0 warms the post-relink memo */
                isaac_gl_record_oracle_stats(st);
                syncs_before = st[3];
            }
#endif
            for (k = 0u; k < 10u; ++k) {
                SCN("glBindFramebuffer", 2u, (uint32_t)(ORACLE_GL_FRAMEBUFFER), (uint32_t)(9u));
                SCN("glBindRenderbuffer", 2u, (uint32_t)(0x8d41u), (uint32_t)(rbo2));
                SCN("glGetRenderbufferParameteriv", 3u, (uint32_t)(0x8d41u), (uint32_t)(0x8d42u), (uint32_t)(A(AR_OUT + 4u)));
                SCN("glGetRenderbufferParameteriv", 3u, (uint32_t)(0x8d41u), (uint32_t)(0x8d43u), (uint32_t)(A(AR_OUT + 8u)));
                SCN("glFramebufferTexture2D", 5u, (uint32_t)(ORACLE_GL_FRAMEBUFFER), (uint32_t)(0x8ce0u), (uint32_t)(ORACLE_GL_TEXTURE_2D), (uint32_t)(5u), (uint32_t)(0));
                rr("ret census status %x rbo %u %u",
                   SCN("glCheckFramebufferStatus", 1u, (uint32_t)(ORACLE_GL_FRAMEBUFFER)),
                   ((uint32_t *)(s_arena + AR_OUT))[1], ((uint32_t *)(s_arena + AR_OUT))[2]);
                SCN("glClear", 1u, (uint32_t)(0x4100u));
            }
            SCN("glBindFramebuffer", 2u, (uint32_t)(ORACLE_GL_FRAMEBUFFER), (uint32_t)(0u));
            for (k = 0u; k < 20u; ++k) {
                put_name(AR_NAMES, attribs[k % 3u]);
                rr("ret census attrib %d", SCN("glGetAttribLocation", 2u, (uint32_t)(program), (uint32_t)(A(AR_NAMES))));
                POISON(AR_NAMES, 16u);
            }
            for (k = 0u; k < 30u; ++k) {
                uint32_t j;
                for (j = 0u; j < 16u; ++j) ((float *)(s_arena + AR_VALUES))[j] = (float)(present * 30u + k + j);
                SCN("glUniformMatrix4fv", 4u, (uint32_t)(location), (uint32_t)(1), (uint32_t)(0u), (uint32_t)(A(AR_VALUES)));
                POISON(AR_VALUES, 64u);
                SCN("glBindTexture", 2u, (uint32_t)(ORACLE_GL_TEXTURE_2D), (uint32_t)(5u + (k & 1u)));
                memcpy(s_arena + AR_INDICES, quad, sizeof quad);
                fill_vertices(present * 7u + k, 4u * 20u);
                SCN("glDrawElements", 4u, (uint32_t)(ORACLE_GL_TRIANGLES), (uint32_t)(6),
                    (uint32_t)(ORACLE_GL_UNSIGNED_SHORT), (uint32_t)(A(AR_INDICES)));
                fill_vertices(present * 13u + k + 99u, 4u * 20u); /* KAGE reuse */
                POISON(AR_INDICES, 12u);
            }
            if (present % 6u == 0u) {
                SCN("glReadPixels", 7u, (uint32_t)(1), (uint32_t)(2), (uint32_t)(1), (uint32_t)(1),
                    (uint32_t)(ORACLE_GL_RGBA), (uint32_t)(ORACLE_GL_UNSIGNED_BYTE), (uint32_t)(A(AR_OUT + 16u)));
                rr("ret census pixel h=%08x", rt_hash(s_arena + AR_OUT + 16u, 4u));
            }
            PRESENT();
        }
#if defined(ISAAC_VITA_GL_THREAD_RECORD) && ISAAC_VITA_GL_THREAD_RECORD
        isaac_gl_record_oracle_stats(st);
        printf("census steady presents 1..59: %u synchronous waits = %.3f/present (readbacks: 9)\n",
               st[3] - syncs_before, (double)(st[3] - syncs_before) / 59.0);
#else
        (void)syncs_before;
#endif
    }
    PRESENT();
}

#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
static int s_identity_inside;
static void scn_identity_probe(void *unused)
{
    (void)unused;
    s_identity_inside = isaac_gl_record_on_render_thread() &&
                        GetCurrentThreadId() == s_vgl_thread;
}
static uint8_t *s_typed;
static size_t s_typed_bytes;
static void scn_read_typed(void *unused)
{ (void)unused; s_typed_bytes = gl_vita_backend_oracle_typed_state(s_typed, 512u); }
static void scn_uninstall(void *unused) { (void)unused; gl_vita_backend_uninstall(); }
# if defined(ISAAC_GL_RECORD_OPCODE_CENSUS)
/* ISAAC_VITA_GL_RECORD_OPCODE_CENSUS: the census of the window `w` closed
 * must account for every record of it (gl_record_backend.h), and the next
 * windows must restart: a NULL take discards, a closure's own record lands
 * in the window after its take.  Takes post closures only (no GL call), so
 * the trace stays identical to the other variants.  Prints its coverage. */
#  if defined(ISAAC_VITA_GL_RECORD_ATTRIB_BATCH)
/* REC_WORKER_CALL, before the two S4 batch opcodes (gl_record_backend.c). */
#   define SCN_CLOSURE_SLOT (ISAAC_GL_RECORD_ATTRIBS_ENABLE_OPCODE - 1u)
#  else
#   define SCN_CLOSURE_SLOT (ISAAC_GL_RECORD_CENSUS_OPS - 1u) /* REC_WORKER_CALL */
#  endif
static int scn_check_census(const IsaacGlRecordWorkerWindow *w)
{
    const IsaacGlRecordOpcodeCensus *c = isaac_gl_record_opcode_census();
    uint32_t op, n = 0u, b = 0u, e = 0u, rs = 0u, ps = 0u, pst = 0u, excess = 0u;
    uint32_t sy = 0u, sw = 0u, swus = 0u, active = 0u, bad = 0u;
    const IsaacGlRecordOpcodeStats *k = &c->op[SCN_CLOSURE_SLOT];
    IsaacGlRecordWorkerWindow w2;

    for (op = 0u; op < ISAAC_GL_RECORD_CENSUS_OPS; ++op) {
        const IsaacGlRecordOpcodeStats *o = &c->op[op];
        uint32_t exact = op == SCN_CLOSURE_SLOT ? o->n :
            (o->n < ISAAC_GL_RECORD_CENSUS_EXACT ? o->n : ISAAC_GL_RECORD_CENSUS_EXACT);
        n += o->n; b += o->b; e += o->e; rs += o->rs; ps += o->ps; pst += o->pst;
        excess += o->n - exact;
        if (op != SCN_CLOSURE_SLOT) { sy += o->sy; sw += o->sw; swus += o->swus; }
        if (o->n) ++active;
        /* exact = the first 32 of each opcode and every closure; random
         * timings only among the rest; stamped and blocked are subsets; a
         * sampled append or an S wait is replayed in the same window. */
        if (o->e != exact || o->e + o->rs > o->n || o->pst > o->ps || o->sw > o->sy ||
                ((o->ps || o->sy) && !o->n))
            ++bad;
    }
    printf("census coverage: ops=%u n=%u b=%u exact=%u random=%u excess=%u "
           "sampled(ps,stamped)=%u,%u S(sy,sw,us)=%u,%u,%u closures(n,sy,sw)=%u,%u,%u "
           "slot0=%u bad=%u\n", active, n, b, e, rs, excess, ps, pst, sy, sw, swus,
           k->n, k->sy, k->sw, c->op[0].n | c->op[0].ps | c->op[0].sy, bad);
    if (bad || c->op[0].n || c->op[0].ps || c->op[0].sy || n != w->records ||
            b < w->copied_bytes || b - w->copied_bytes > 7u * n || ps != w->ap_samples ||
            sy != w->syncs || sw != w->sync_waits || swus != w->sync_wait_us ||
            k->sw > w->call_waits || k->swus > w->call_wait_us || !k->n || !k->sy ||
            !pst || !sy || (excess >= 640u && !rs))
        return 1;
    /* The first take's closure record was counted after its reset; a NULL
     * take discards it, and the second take sees only the NULL take's own
     * closure (one record, exactly timed, one closure call). */
    isaac_gl_record_worker_take(NULL);
    isaac_gl_record_worker_take(&w2);
    for (op = 0u; op < ISAAC_GL_RECORD_CENSUS_OPS; ++op) {
        const IsaacGlRecordOpcodeStats *o = &c->op[op];
        if (op == SCN_CLOSURE_SLOT ?
                (o->n != 1u || o->e != 1u || o->sy != 1u || o->ps || o->rs) :
                (o->n | o->b | o->e | o->rs | o->ps | o->sy) != 0u)
            return 2;
    }
    printf("census window restart: records=%u closure(n,e,sy)=%u,%u,%u\n",
           w2.records, k->n, k->e, k->sy);
    return w2.records == 1u ? 0 : 3;
}
# endif
#endif

int main(int argc, char **argv)
{
    uint32_t stack[96];
    uint8_t typed[512];
    size_t typed_bytes, i;
    CPU cpu;

    s_arena = (uint8_t *)VirtualAlloc((void *)(uintptr_t)ARENA_ADDRESS, ARENA_BYTES,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if ((uintptr_t)s_arena != ARENA_ADDRESS) return 4;
    if (argc != 3 || !(s_trace = fopen(argv[1], "w"))) return 2;
    s_poison = argv[2][0] == '1';
    memset(&cpu, 0, sizeof cpu);
#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
    if (!isaac_gl_record_worker_start(scn_vgl_init)) return 5;
    /* The device bug: a shared "on render thread" flag made the producer run
     * closures inline.  Identity must differ between the two threads. */
    isaac_gl_record_worker_call(scn_identity_probe, NULL);
    if (isaac_gl_record_on_render_thread() || !s_identity_inside) return 6;
#else
    scn_vgl_init();
#endif
    run(&cpu, (uint32_t)(uintptr_t)&stack[96]);
#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
    {
        /* ph120.rt source: one take for the whole run (it also quiesces). */
        IsaacGlRecordWorkerWindow w;
        isaac_gl_record_worker_take(&w);
        printf("render thread window: wall=%u busy=%u f(n,o)=%u,%u fr(50,95,max)=%u,%u,%u "
               "hw=%u r=%u s(n,w,us)=%u,%u,%u h(n,us)=%u,%u c(n,us)=%u,%u rf=%u\n",
               w.wall_us, w.busy_us, w.frames, w.frame_overflow, w.frame_p50_us,
               w.frame_p95_us, w.frame_max_us, w.ring_high_water, w.records,
               w.syncs, w.sync_waits, w.sync_wait_us, w.held, w.held_us,
               w.call_waits, w.call_wait_us, w.ring_full_waits);
        printf("render thread wake: wk=%u sl(n,t)=%u,%u mb(n,s,us)=%u,%u,%u ap(s,us)=%u,%u cb=%u mq=%u\n",
               w.wakes, w.consumer_sleeps, w.consumer_timeouts, w.mb_calls,
               w.mb_samples, w.mb_us, w.ap_samples, w.ap_us, w.copied_bytes, w.mb_queries);
        /* Host sleeps time out after 1 s: a timeout that found work means a
         * wait/PRESENT/ring-full path published without kicking. */
        if (w.consumer_timeouts || !w.mb_calls || !w.ap_samples || !w.copied_bytes ||
                !w.mb_queries || w.mb_queries > w.mb_calls)
            return 8;
        if (w.frames != s_scn_presents || w.frame_overflow || !w.records ||
                w.busy_us > w.wall_us || w.frame_p50_us > w.frame_p95_us ||
                w.frame_p95_us > w.frame_max_us || !w.ring_high_water ||
                w.sync_waits > w.syncs)
            return 7;
# if defined(ISAAC_GL_RECORD_OPCODE_CENSUS)
        if (scn_check_census(&w)) return 9;
# endif
    }
#endif
    fputs("results\n", s_trace);
    fwrite(s_results, 1u, s_results_len, s_trace);
#if defined(ISAAC_VITA_PHASE_PROFILE)
    {
        const uint8_t *phase = (const uint8_t *)&g_isaac_vita_gl_phase_profile_counters;
        fputs("phase ", s_trace);
        for (i = 0u; i < sizeof g_isaac_vita_gl_phase_profile_counters; ++i) fprintf(s_trace, "%02x", phase[i]);
        fputc('\n', s_trace);
    }
#endif
#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
    s_typed = typed;
    isaac_gl_record_worker_call(scn_read_typed, NULL);
    typed_bytes = s_typed_bytes;
#else
    typed_bytes = gl_vita_backend_oracle_typed_state(typed, 512u);
#endif
    fputs("typed ", s_trace);
    for (i = 0u; i < typed_bytes; ++i) fprintf(s_trace, "%02x", typed[i]);
    fputc('\n', s_trace);
#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
    isaac_gl_record_worker_call(scn_uninstall, NULL);
    printf("worker producer waits (ring full): %u\n", isaac_gl_record_worker_producer_waits());
    isaac_gl_record_worker_stop();
#else
    gl_vita_backend_uninstall();
#endif
    fclose(s_trace);
#if defined(ISAAC_VITA_GL_THREAD_RECORD) && ISAAC_VITA_GL_THREAD_RECORD
    {
        uint32_t st[8];
        isaac_gl_record_oracle_stats(st);
        printf("scenario record poison=%d records=%u copied=%u syncs=%u cold_loc=%u draws(snap,sync)=%u,%u\n",
               s_poison, st[0], st[1], st[3], st[4], st[6], st[7]);
    }
# if defined(SCN_BATCH)
    {
        /* Every batch the script issued was published as one record, and
         * (worker builds) some of them started right after a wrap marker. */
        uint32_t bs[4];
        isaac_gl_record_oracle_batch_stats(bs);
        printf("scenario batch records(en,di)=%u,%u attributes=%u after-wrap=%u issued(en,di)=%u,%u\n",
               bs[0], bs[1], bs[2], bs[3], s_scn_batches[0], s_scn_batches[1]);
        if (bs[0] != s_scn_batches[0] || bs[1] != s_scn_batches[1] || !bs[2])
            return 12;
#  if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
        if (!bs[3])
            return 13;
#  endif
    }
# endif
#else
    printf("scenario direct poison=%d\n", s_poison);
#endif
    return 0;
}
