/* Differential host oracle for ISAAC_VITA_KAGE_BIND_PACKET (RENDER60 W-pkt;
 * driven by recomp/test_vita_kage_bind_packet.py).
 *
 * The production dispatcher (guest.c, included), gl_bridge.c, host_vita_gl.c
 * and the real typed GL recorder (gl_record_backend.c with the knob,
 * gl_record_ring.c, gl_record_locations.c) with its consumer thread are
 * built exactly as recipes A/B install them: the recording backend below is
 * wrapped and installed ON the consumer inside a synchronous worker closure.
 * The translated bodies of the frozen corpus (Shader::Bind 005671d0,
 * Shader::SetUniformInt 00567430, RenderManager::Transform 00565240,
 * Image::IsEmpty 0055f900 and the eight trivial target getters, extracted
 * verbatim by the test) are linked in both GPR spellings (GPR locals, the
 * production shape, and memory mode under a mem_ prefix); the seam TU
 * (host_vita_kage_bind_packet.c) is linked twice, once per spelling, its
 * fallback bound to that spelling's Bind.
 *
 * A case is a chain of 1..8 Bind calls on one install (so the producer
 * location cache, the seam's name memo and the epoch carry from call to call
 * exactly as in a frame).  The chain runs once with the translated Bind (the
 * reference) and once with the --wrap entry (the seam), each from a fresh
 * install with the same fake-backend epoch, and every step must give:
 *   - the same GL call stream on the render thread: function, arguments,
 *     the name bytes, the 16 matrix words, and the return RVA each call
 *     sees (isaac_gl_record_return_rva), in the same order;
 *   - the same CPU at the `ret 4`: eight registers, xmm0..7 bit for bit, the
 *     lazy flag fields, x87 state, fault, stop kind, the stack low water;
 *   - the same guest words from the entry ESP up to the stack ceiling and
 *     the same Shader / Image / target / RenderManager / slot bytes;
 *   - the same number of synchronous lookups (cold producer-cache misses),
 *     i.e. identical producer location-cache transitions;
 *   - the same body-entry census (ISAAC_VITA_PROFILE_FUNCTION_ENTRIES
 *     flavour) and the same final sampler word (GUEST_SAMPLER flavour);
 * and the verdict the case expects.  A handled seam makes no guest_call
 * (guest_calls and dyn deltas zero; the reference makes one per crossing)
 * and appends exactly packets + cold-lookup ring records; a declined seam
 * leaves every census equal to the reference.
 *
 * Executed on a 32-bit identity-mapped host (MSVC x86, /LARGEADDRESSAWARE,
 * the 0x98xxxxxx image words committed at their real addresses). */
#include "guest.c"

#include <stdarg.h>

#include "gl_bridge.h"
#include "gl_record_backend.h"
#include "host_vita_gl.h"
#include "host_vita_import_id.h"
#include "host_vita_kage_bind_packet.h"

#include <windows.h>

#if UINTPTR_MAX != UINT32_MAX
# error host_vita_kage_bind_packet_oracle requires a 32-bit host
#endif
#if !defined(ISAAC_VITA_IMPORT_ID_DISPATCH) || \
    !defined(ISAAC_VITA_GUEST_LOOKUP_CACHE) || \
    !defined(ISAAC_VITA_PHASE_PROFILE) || GUEST_STACK_REQUIRED != 1 || \
    !defined(ISAAC_VITA_GL_SHIM_FASTDISPATCH) || \
    !defined(ISAAC_VITA_SHADER_ATTRIB_FASTPATH) || \
    !defined(ISAAC_VITA_GL_THREAD_RECORD) || \
    !defined(ISAAC_VITA_GL_THREAD_WORKER) || \
    !defined(ISAAC_VITA_KAGE_BIND_PACKET)
# error build this oracle with the production guest.c + recorder configuration
#endif
#if GUEST_IMAGE_BASE != 0x98000000u
# error the frozen corpus text is rebased to 0x98000000
#endif

#define VA(rva) ((uint32_t)(GUEST_IMAGE_BASE + (uint32_t)(rva)))

/* ---- the translated bodies (two spellings) --------------------------- */

#define ORACLE_BODY_LIST(X) \
    X(sub_005671d0) X(sub_00567430) X(sub_00565240) X(sub_0055f900) \
    X(sub_005a0b90) X(sub_005a0ba0) X(sub_005a0dd0) X(sub_005a0de0) \
    X(sub_00032ed0) X(sub_005a1a70) X(sub_005a1ef0) X(sub_005a1f00)
#define ORACLE_DECLARE(name) \
    void name(CPU *__restrict c); void mem_##name(CPU *__restrict c);
ORACLE_BODY_LIST(ORACLE_DECLARE)
#undef ORACLE_DECLARE

/* The seam, once per spelling (the test renames the second copy). */
void mem_wrap_sub_005671d0(CPU *__restrict c);
void mem_isaac_vita_kage_bind_packet_counts(uint32_t out[3]);
void mem_isaac_vita_kage_bind_packet_declines(uint32_t out[8]);

/* The seam's decline reasons, in isaac_vita_kage_bind_packet_declines
 * order. */
enum {
    R_FRAME, R_OBJECT, R_RECORDS, R_IMAGE, R_TARGET, R_TOKENS, R_RECORDER,
    R_OWNER, R_COUNT
};
static const char *const s_reason_name[R_COUNT] = {
    "frame", "object", "records", "image", "target", "tokens", "recorder",
    "owner",
};

/* Transform calls the cookie check 005eacd7 (always with
 * GUEST_LEAF_INLINE=0, on a mismatch otherwise; extracted too).  Its
 * failure path continues into the report body 005eb36d, whose only callee
 * outside the extracted set is 005eb345.  The cookie never mismatches here,
 * so that callee is a loud stub. */
void sub_005eb345(CPU *__restrict c);
void mem_sub_005eb345(CPU *__restrict c);
void sub_005eb345(CPU *__restrict c)
{
    guest_fault(c, 0x005eb345u, "oracle: cookie report path reached");
}
void mem_sub_005eb345(CPU *__restrict c)
{
    guest_fault(c, 0x005eb345u, "oracle: cookie report path reached");
}

/* ---- stubs owned by other Vita runtime units ------------------------ */

unsigned g_host_import_calls;
unsigned g_host_dynamic_calls;

int guest_host_import_ids_register(const guest_import *imports, uint32_t count)
{
    (void)imports;
    (void)count;
    return 1;
}

int guest_host_import_id(CPU *__restrict c, uint32_t import_id)
{
    ++g_host_import_calls;
    c->eax = UINT32_C(0x60000000) | import_id;
    (void)gpop(c);
    return 1;
}

const char *guest_host_import_id_error(void)
{
    return "oracle generic import-ID error";
}

int guest_host_dynamic(CPU *__restrict c, uint32_t token)
{
    uint32_t family = token & UINT32_C(0xff000000);

    if (family != UINT32_C(0x7d000000) && family != UINT32_C(0x7e000000))
        return 0;
    return isaac_vita_gl_dynamic_counted(c, token, &g_host_dynamic_calls);
}

/* The seam's cumulative log line (game thread). */
static char s_log[4096];
static size_t s_log_len;
static unsigned s_log_lines;

void isaac_vita_log(const char *format, ...)
{
    va_list ap;
    int n;

    ++s_log_lines;
    if (s_log_len >= sizeof s_log - 1u)
        return;
    va_start(ap, format);
    n = vsnprintf(s_log + s_log_len, sizeof s_log - s_log_len, format, ap);
    va_end(ap);
    if (n > 0)
        s_log_len += (size_t)n < sizeof s_log - s_log_len ?
            (size_t)n : sizeof s_log - 1u - s_log_len;
}

/* ---- trace: game-thread lines and render-thread (consumer) lines ------ */

static char s_trace[1u << 16];
static size_t s_trace_len;
static int s_trace_overflow;
static char s_ctrace[1u << 16];
static size_t s_ctrace_len;
static int s_ctrace_overflow;
static unsigned s_backend_calls;    /* consumer (or game thread, unrecorded) */

static void trace(const char *format, ...)
{
    char *buffer = s_trace;
    size_t cap = sizeof s_trace;
    size_t *len = &s_trace_len;
    int *overflow = &s_trace_overflow;
    va_list ap;
    int n;

    if (isaac_gl_record_on_render_thread()) {
        buffer = s_ctrace;
        cap = sizeof s_ctrace;
        len = &s_ctrace_len;
        overflow = &s_ctrace_overflow;
    }
    if (*len >= cap - 1u) {
        *overflow = 1;
        return;
    }
    va_start(ap, format);
    n = vsnprintf(buffer + *len, cap - *len, format, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *len) {
        *overflow = 1;
        *len = cap - 1u;
        return;
    }
    *len += (size_t)n;
}

static void trace_reset(void)
{
    s_trace_len = 0u;
    s_trace[0] = '\0';
    s_trace_overflow = 0;
    s_ctrace_len = 0u;
    s_ctrace[0] = '\0';
    s_ctrace_overflow = 0;
}

/* ---- recording backend ------------------------------------------------ */

/* Link epoch of the fake GL: every program-state mutator advances it and
 * every glGetUniformLocation answer carries it (answer / 64), so a location
 * remembered across a mutation reaches glUniform1i / glUniformMatrix4fv as a
 * different argument than the reference's fresh lookup. */
static uint32_t s_link_epoch;

static uint32_t be_rva(void)
{
    uint32_t rva = 0u;
    (void)isaac_gl_record_return_rva(&rva);
    return rva;
}

static void be_glUseProgram(guest_gl_uint program)
{
    ++s_backend_calls;
    trace("be glUseProgram(%u) rva=%06x\n", program, be_rva());
}

static void be_glActiveTexture(guest_gl_enum texture)
{
    ++s_backend_calls;
    trace("be glActiveTexture(%08x) rva=%06x\n", texture, be_rva());
}

static void be_glBindTexture(guest_gl_enum target, guest_gl_uint texture)
{
    ++s_backend_calls;
    trace("be glBindTexture(%08x,%08x) rva=%06x\n", target, texture, be_rva());
}

static guest_gl_int be_glGetUniformLocation(guest_gl_uint program,
                                            guest_gl_addr name)
{
    const char *text = (const char *)(uintptr_t)name;
    uint32_t hash = program * 33u;
    const char *p;

    ++s_backend_calls;
    if (!text) {
        trace("be glGetUniformLocation(%u,<null>) rva=%06x\n", program, be_rva());
        return -1;
    }
    for (p = text; *p; ++p)
        hash = hash * 31u + (unsigned char)*p;
    /* The name may be the ring copy: print the bytes, not the address. */
    trace("be glGetUniformLocation(%u,\"%s\") rva=%06x\n", program, text, be_rva());
    if (text[0] == 'x')
        return -1;
    return (guest_gl_int)(hash % 61u + 64u * s_link_epoch);
}

static void be_glUniform1i(guest_gl_int location, guest_gl_int v0)
{
    ++s_backend_calls;
    trace("be glUniform1i(%d,%d) rva=%06x\n", location, v0, be_rva());
}

static void be_glUniformMatrix4fv(guest_gl_int location, guest_gl_sizei count,
                                  guest_gl_boolean transpose, guest_gl_addr value)
{
    uint32_t i;

    ++s_backend_calls;
    trace("be glUniformMatrix4fv(%d,%d,%u) rva=%06x m=", location, count,
          (unsigned)transpose, be_rva());
    for (i = 0u; i < 16u; ++i)
        trace("%08x%s", value ? ld32(value + i * 4u) : 0u, i < 15u ? "," : "\n");
}

static void be_glCullFace(guest_gl_enum mode)
{
    ++s_backend_calls;
    trace("be glCullFace(%08x) rva=%06x\n", mode, be_rva());
}

/* Registered foreign tokens of the same arity as the slot they corrupt
 * (a translated call through a slot of another arity unbalances the guest
 * stack and the reference itself crashes the host). */
static void be_glClampColorARB(guest_gl_enum target, guest_gl_enum clamp)
{
    ++s_backend_calls;
    trace("be glClampColorARB(%08x,%08x) rva=%06x\n", target, clamp, be_rva());
}

static guest_gl_int be_glGetAttribLocation(guest_gl_uint program,
                                           guest_gl_addr name)
{
    ++s_backend_calls;
    trace("be glGetAttribLocation(%u,\"%s\") rva=%06x\n", program,
          name ? (const char *)(uintptr_t)name : "<null>", be_rva());
    return 5;
}

static void be_glUniformMatrix3fv(guest_gl_int location, guest_gl_sizei count,
                                  guest_gl_boolean transpose, guest_gl_addr value)
{
    uint32_t i;

    ++s_backend_calls;
    trace("be glUniformMatrix3fv(%d,%d,%u) rva=%06x m=", location, count,
          (unsigned)transpose, be_rva());
    for (i = 0u; i < 9u; ++i)
        trace("%08x%s", value ? ld32(value + i * 4u) : 0u, i < 8u ? "," : "\n");
}

/* The seven program-state mutators (the recorder's FENCES, each of which
 * resets the producer location cache): a new link epoch. */
#define ORACLE_CREATE_NAME 8u
static unsigned s_mutator_calls;

static void be_mutate(const char *what)
{
    ++s_backend_calls;
    ++s_mutator_calls;
    ++s_link_epoch;
    trace("be %s -> epoch %u\n", what, s_link_epoch);
}
static void be_glAttachShader(guest_gl_uint p, guest_gl_uint s)
{ (void)p; (void)s; be_mutate("glAttachShader"); }
static void be_glCompileShader(guest_gl_uint s)
{ (void)s; be_mutate("glCompileShader"); }
static guest_gl_uint be_glCreateProgram(void)
{ be_mutate("glCreateProgram"); return ORACLE_CREATE_NAME; }
static void be_glDeleteProgram(guest_gl_uint p)
{ (void)p; be_mutate("glDeleteProgram"); }
static void be_glDeleteShader(guest_gl_uint s)
{ (void)s; be_mutate("glDeleteShader"); }
static void be_glLinkProgram(guest_gl_uint p)
{ (void)p; be_mutate("glLinkProgram"); }
static void be_glShaderSource(guest_gl_uint s, guest_gl_sizei n,
                              guest_gl_addr t, guest_gl_addr l)
{ (void)s; (void)n; (void)t; (void)l; be_mutate("glShaderSource"); }

static void fill_backend(guest_gl_backend *backend)
{
    memset(backend, 0, sizeof *backend);
    backend->glUseProgram = be_glUseProgram;
    backend->glActiveTexture = be_glActiveTexture;
    backend->glBindTexture = be_glBindTexture;
    backend->glGetUniformLocation = be_glGetUniformLocation;
    backend->glUniform1i = be_glUniform1i;
    backend->glUniformMatrix4fv = be_glUniformMatrix4fv;
    backend->glCullFace = be_glCullFace;
    backend->glClampColorARB = be_glClampColorARB;
    backend->glGetAttribLocation = be_glGetAttribLocation;
    backend->glUniformMatrix3fv = be_glUniformMatrix3fv;
    backend->glAttachShader = be_glAttachShader;
    backend->glCompileShader = be_glCompileShader;
    backend->glCreateProgram = be_glCreateProgram;
    backend->glDeleteProgram = be_glDeleteProgram;
    backend->glDeleteShader = be_glDeleteShader;
    backend->glLinkProgram = be_glLinkProgram;
    backend->glShaderSource = be_glShaderSource;
}

/* gl_vita_backend_install's order on the consumer: stop, wrap (which resets
 * the producer location cache and so the seam's memo epoch), install.  An
 * unrecorded install (the recorder bypassed: the seam must decline) installs
 * the fake table itself. */
static IsaacGlRecordRboNative s_rbo;

typedef struct install_args {
    int recorded;
    uint32_t epoch;
} install_args;

static void install_closure(void *context)
{
    const install_args *args = (const install_args *)context;
    guest_gl_backend backend;

    isaac_gl_record_stop();
    s_link_epoch = args->epoch;
    fill_backend(&backend);
    if (args->recorded)
        isaac_gl_record_wrap(&backend, &s_rbo);
    guest_gl_install_backend(&backend);
}

static void install(int recorded, uint32_t epoch)
{
    install_args args;

    args.recorded = recorded;
    args.epoch = epoch;
    isaac_gl_record_worker_call(install_closure, &args);
}

/* ---- image words at their real addresses ------------------------------ */

/* VirtualAlloc reserves in 64 KiB granules: one commit per granule. */
#define ORACLE_PAGE 0x10000u
static const uint32_t s_image_pages[] = {
    0x00740000u,   /* "Transform" 744ac0 */
    0x00750000u,   /* "Texture1" 75e03c, "Texture0" 75e098, Shader vtable 75e0a8 */
    0x00760000u,   /* 2.0f 76a7b0, u32 bias 76c940, sign mask 76c9b0 */
    0x007a0000u,   /* cookie 7aa3b4, default extent 7aa92c/7aa930 */
    0x007b0000u,   /* GL slots 7bf96c/7bf994/7bf9bc/7bf9d0 */
    0x007c0000u,   /* GL slots 7c293c/7c2978, RenderManager 7c7a30, image 7c7af4 */
    0x00800000u,   /* an image-backed synthetic stack (decline case) */
};

static int commit_image_page(uint32_t base, uint32_t bytes)
{
    void *result = VirtualAlloc((LPVOID)(uintptr_t)base, bytes,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!result || (uintptr_t)result > base)
        return 0;
    st32(base, 0x11223344u);
    st32(base + bytes - 4u, 0x55667788u);
    return ld32(base) == 0x11223344u && ld32(base + bytes - 4u) == 0x55667788u;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* argv[1..]: "rva:hex" spans the test read from the frozen PE (the Shader
 * vtable already relocated to GUEST_IMAGE_BASE). */
static int install_span(const char *arg)
{
    const char *colon = strchr(arg, ':');
    uint32_t rva = 0u, i, bytes;
    const char *p;

    if (!colon || colon == arg)
        return 0;
    for (p = arg; p < colon; ++p) {
        int v = hex_nibble(*p);
        if (v < 0)
            return 0;
        rva = rva * 16u + (uint32_t)v;
    }
    bytes = (uint32_t)strlen(colon + 1) / 2u;
    if (!bytes || strlen(colon + 1) != bytes * 2u || rva < 0x00740000u ||
            rva + bytes > 0x007d0000u)
        return 0;
    for (i = 0u; i < bytes; ++i) {
        int hi = hex_nibble(colon[1 + i * 2u]);
        int lo = hex_nibble(colon[2 + i * 2u]);
        if (hi < 0 || lo < 0)
            return 0;
        st8(VA(rva) + i, (uint8_t)((hi << 4) | lo));
    }
    return 1;
}

/* Frozen addresses (copied from the corpus lines the seam cites). */
#define O_NAME_TEXTURE0 VA(0x0075e098u)
#define O_NAME_TEXTURE1 VA(0x0075e03cu)
#define O_NAME_TRANSFORM VA(0x00744ac0u)
#define O_SHADER_VTABLE VA(0x0075e0a8u)
#define O_COOKIE VA(0x007aa3b4u)
#define O_DEFAULT_W VA(0x007aa92cu)
#define O_DEFAULT_H VA(0x007aa930u)
#define O_MANAGER VA(0x007c7a30u)
#define O_IMAGE_PTR VA(0x007c7af4u)
#define O_IMAGE_STACK VA(0x00800000u)
static const uint32_t s_slot_va[6] = {
    VA(0x007bf994u), VA(0x007c293cu), VA(0x007c2978u),
    VA(0x007bf9bcu), VA(0x007bf96cu), VA(0x007bf9d0u),
};
static const char *const s_slot_fn[6] = {
    "glUseProgram", "glActiveTexture", "glBindTexture",
    "glGetUniformLocation", "glUniform1i", "glUniformMatrix4fv",
};
static uint32_t s_slot_token[6];

#if defined(ORACLE_STAGE_WORDS)
/* Sampler flavour: the seam TU is compiled with its six stage calls renamed
 * to these, which check that the sampler word holds the GL slot's token
 * while the call is staged - the value guest_call publishes for the
 * translated call it stands for. */
static unsigned s_stage_checks, s_stage_mismatches;
static void stage_word(uint32_t index)
{
    ++s_stage_checks;
    if (g_kage_guest_last_indirect_target != s_slot_token[index])
        ++s_stage_mismatches;
}
void oracle_bp_use_program(uint32_t rva, guest_gl_uint program)
{
    stage_word(0u);
    isaac_gl_record_bind_packet_use_program(rva, program);
}
void oracle_bp_active_texture(uint32_t rva, guest_gl_enum texture)
{
    stage_word(1u);
    isaac_gl_record_bind_packet_active_texture(rva, texture);
}
void oracle_bp_bind_texture(uint32_t rva, guest_gl_enum target,
                            guest_gl_uint texture)
{
    stage_word(2u);
    isaac_gl_record_bind_packet_bind_texture(rva, target, texture);
}
int oracle_bp_uniform_location(uint32_t rva, uint32_t slot,
                               guest_gl_uint program, guest_gl_addr name,
                               guest_gl_int *location)
{
    stage_word(3u);
    return isaac_gl_record_bind_packet_uniform_location(rva, slot, program,
                                                        name, location);
}
void oracle_bp_uniform1i(uint32_t rva, guest_gl_int location, guest_gl_int v0)
{
    stage_word(4u);
    isaac_gl_record_bind_packet_uniform1i(rva, location, v0);
}
void oracle_bp_uniform_matrix4fv(uint32_t rva, guest_gl_int location,
                                 guest_gl_sizei count,
                                 guest_gl_boolean transpose,
                                 guest_gl_addr value, const uint32_t matrix[16])
{
    stage_word(5u);
    isaac_gl_record_bind_packet_uniform_matrix4fv(rva, location, count,
                                                  transpose, value, matrix);
}
#endif

/* ---- registered functions (sorted VA keys, image_rva is the identity) -- */

#define O_FOREIGN_SUI VA(0x00600100u)
#define O_FOREIGN_EMPTY VA(0x00600110u)
#define O_FOREIGN_GETTER VA(0x00600120u)
#define O_GETTER_COUNT 8u
static const uint32_t s_getter_va[O_GETTER_COUNT] = {
    VA(0x005a0b90u), VA(0x005a0ba0u), VA(0x005a0dd0u), VA(0x005a0de0u),
    VA(0x00032ed0u), VA(0x005a1a70u), VA(0x005a1ef0u), VA(0x005a1f00u),
};
/* Field offsets the eight getters read (dword unless halfword). */
static const uint32_t s_getter_off[O_GETTER_COUNT] = {
    0x100u, 0x104u, 0x80u, 0x82u, 0x80u, 0x84u, 0xc0u, 0xc4u,
};
static const int s_getter_half[O_GETTER_COUNT] = { 0, 0, 1, 1, 0, 0, 0, 0 };

static void oracle_foreign_sui(CPU *__restrict c)
{
    trace("foreign SetUniformInt(%08x,%u,%08x)\n", ld32(c->esp + 4u),
          ld32(c->esp + 8u), ld32(c->esp + 12u));
    c->eax = 0x0f0f0f0fu;
    (void)gpop(c);
    c->esp += 12u;
}

static void oracle_foreign_empty(CPU *__restrict c)
{
    trace("foreign IsEmpty(%08x)\n", c->ecx);
    c->eax = (c->eax & 0xffffff00u) | 1u;
    (void)gpop(c);
}

static void oracle_foreign_getter(CPU *__restrict c)
{
    trace("foreign getter(%08x)\n", c->ecx);
    c->eax = 77u;
    (void)gpop(c);
}

static const uint32_t s_reg_addrs[] = {
    VA(0x00032ed0u), VA(0x0055f900u), VA(0x00567430u), VA(0x005a0b90u),
    VA(0x005a0ba0u), VA(0x005a0dd0u), VA(0x005a0de0u), VA(0x005a1a70u),
    VA(0x005a1ef0u), VA(0x005a1f00u), O_FOREIGN_SUI, O_FOREIGN_EMPTY,
    O_FOREIGN_GETTER,
};
#define O_REG_COUNT (sizeof s_reg_addrs / sizeof s_reg_addrs[0])
static const guest_fn s_reg_gpr[O_REG_COUNT] = {
    sub_00032ed0, sub_0055f900, sub_00567430, sub_005a0b90, sub_005a0ba0,
    sub_005a0dd0, sub_005a0de0, sub_005a1a70, sub_005a1ef0, sub_005a1f00,
    oracle_foreign_sui, oracle_foreign_empty, oracle_foreign_getter,
};
static const guest_fn s_reg_mem[O_REG_COUNT] = {
    mem_sub_00032ed0, mem_sub_0055f900, mem_sub_00567430, mem_sub_005a0b90,
    mem_sub_005a0ba0, mem_sub_005a0dd0, mem_sub_005a0de0, mem_sub_005a1a70,
    mem_sub_005a1ef0, mem_sub_005a1f00,
    oracle_foreign_sui, oracle_foreign_empty, oracle_foreign_getter,
};

/* ---- guest objects ------------------------------------------------------ */

/* The bound stack has guard words on both sides (a shallow frame writes
 * below the floor, a frame at the ceiling reads and writes above it), so
 * no run touches another static. */
#define ORACLE_STACK_WORDS 512u
#define ORACLE_STACK_GUARD_WORDS 64u
#define ORACLE_STORAGE_WORDS (2u * ORACLE_STACK_GUARD_WORDS + ORACLE_STACK_WORDS)
#define ORACLE_ABOVE_ENTRY 0x400u          /* default frame: ceiling - 1 KiB */
#define ORACLE_STACK_OBJECT 0x80u          /* stack objects at entry + 0x80 */
#define ORACLE_RETURN_RVA UINT32_C(0x0055d4a7)

static uint32_t s_stack_storage[ORACLE_STORAGE_WORDS];
static uint32_t s_foreign_storage[ORACLE_STORAGE_WORDS];
static CPU s_cpu, s_foreign;

/* Every object is a word array addressed by byte offset. */
static uint32_t s_shader[16];        /* the bound Shader (Bind's ECX) */
static uint32_t s_cur_shader[16];    /* RenderManager+0x14 */
static uint32_t s_image[40];         /* RenderManager image 7c7af4 */
static uint32_t s_image_vt[32];
static uint32_t s_target[72];        /* render target, +0x108 */
static uint32_t s_target_vt[16];
static uint32_t s_alt_list[4];       /* the explicit record list argument */
static uint32_t s_records[4];        /* record-index vector storage */
static uint32_t s_descriptors[12];   /* one uniform descriptor (0x18 bytes) */

#define W(obj, off) ((obj)[(off) / 4u])
#define ADDR(obj) ((uint32_t)(uintptr_t)(obj))

/* ---- one case ---------------------------------------------------------- */

enum {
    ACT_NONE = 0,
    ACT_MUTATE_FIRST = 1,         /* 1..7: one program-state mutator */
    ACT_MUTATE_LAST = 7,
    ACT_RENAME = 8,               /* "Texture0" -> "Texturf0" (same address) */
    ACT_UNRENAME = 9,
    ACT_PROGRAM = 10,             /* the Shader's program += 1 */
    ACT_PRIME = 11,               /* cold lookups of the three names first */
    ACT_TRANSFORM_NAME = 12,      /* "Transform" -> "xransform" (answer -1) */
};

typedef struct bind_case {
    const char *name;
    int expect_handled;
    int reason;                   /* the check that must decline (R_*) */
    int steps;
    int act[8];                   /* before step i */
    /* Shader */
    uint32_t vtable;              /* 0: frozen */
    uint32_t sui_slot;            /* non-zero: vtable+0x1c patched to this */
    uint32_t program;
    int list_mode;                /* 0: arg 0, 1: arg -> s_alt_list, 2: arg -> stack */
    uint32_t records;             /* entries in the list used */
    int self_on_stack;
    /* Image */
    int image;
    uint32_t image_handle, image_kind, tex0, tex1;
    uint32_t image_empty_fn;      /* 0: IsEmpty */
    int image_on_stack;
    /* RenderManager */
    int target;
    uint32_t wget, hget;          /* getter VAs */
    uint32_t wval, hval;
    uint32_t target_noise;
    uint8_t flag;
    uint32_t flag_word_noise;     /* the three bytes beside the flag byte */
    int target_on_stack;
    int shader;
    uint32_t cur_program;
    int shader_on_stack;
    uint32_t cookie;              /* 0: the PE value */
    int alt_defaults;
    /* environment */
    int slot_bad[6];              /* 1 zero, 2 token+1, 3 glCullFace token */
    uint32_t esp_offset;          /* 0: frame at the top (ceiling - 16) */
    int esp_top;                  /* ESP = ceiling - 4: no `ret 4` window */
    int foreign;
    int unrecorded;
    int image_stack;
} bind_case;

typedef struct census {
    unsigned dyn, calls, backend, handled, declined, cold;
    unsigned packets, staged, memo, cache, miss;
    uint32_t entries;
    uint32_t why[R_COUNT];
} census;

static uint32_t s_pe_cookie;
static uint32_t s_pe_default_w, s_pe_default_h;
static guest_fn s_body;
static int s_variant;             /* 0 GPR locals, 1 memory mode */

static void seam_counts(uint32_t out[3])
{
    if (s_variant)
        mem_isaac_vita_kage_bind_packet_counts(out);
    else
        isaac_vita_kage_bind_packet_counts(out);
}

static void census_take(census *out)
{
    uint32_t counts[3], stats[5];

    seam_counts(counts);
    if (s_variant)
        mem_isaac_vita_kage_bind_packet_declines(out->why);
    else
        isaac_vita_kage_bind_packet_declines(out->why);
    isaac_gl_record_bind_packet_stats(stats);
    out->dyn = g_host_dynamic_calls;
    out->calls = g_guest_phase_profile_counters.guest_calls;
    out->backend = s_backend_calls;
    out->handled = counts[0];
    out->declined = counts[1];
    out->cold = counts[2];
    out->packets = stats[0];
    out->staged = stats[1];
    out->memo = stats[2];
    out->cache = stats[3];
    out->miss = stats[4];
#if defined(ISAAC_VITA_PROFILE_FUNCTION_ENTRIES)
    out->entries = g_guest_function_entries;
#else
    out->entries = 0u;
#endif
}

static void census_delta(const census *b, census *a)
{
    a->dyn -= b->dyn; a->calls -= b->calls; a->backend -= b->backend;
    a->handled -= b->handled; a->declined -= b->declined; a->cold -= b->cold;
    a->packets -= b->packets; a->staged -= b->staged; a->memo -= b->memo;
    a->cache -= b->cache; a->miss -= b->miss; a->entries -= b->entries;
    for (int i = 0; i < R_COUNT; ++i)
        a->why[i] -= b->why[i];
}

static void oracle_entry(CPU *__restrict c)
{
    s_body(c);
}

static void scrub_entry(CPU *__restrict c)
{
    (void)guest_gl_dispatch(c, UINT32_C(0x7e000001));
}

/* A translated run that faults inside a GL adapter leaves gl_bridge's owner
 * word holding the CPU (fatal in production); normalise it between runs. */
static void scrub_owner_word(CPU *c)
{
    (void)guest_run_until_stop(c, scrub_entry);
    (void)guest_run_until_stop(c, scrub_entry);
    c->fault = NULL;
    c->fault_addr = 0u;
}

static const char *const s_slot_foreign[6] = {
    "glCullFace", "glCullFace", "glClampColorARB", "glGetAttribLocation",
    "glClampColorARB", "glUniformMatrix3fv",
};

/* 1: zero, 2: the next token (a near miss), 3: a registered foreign token
 * of the same arity. */
static uint32_t slot_value(uint32_t index, uint32_t good, int bad)
{
    switch (bad) {
    case 1: return 0u;
    case 2: return good + 1u;
    case 3: return guest_gl_resolve(s_slot_foreign[index]);
    default: return good;
    }
}

static uint32_t s_lcg = 0x2545f491u;
static uint32_t next_random(void)
{
    s_lcg = s_lcg * 1103515245u + 12345u;
    return (s_lcg >> 8) ^ (s_lcg << 20);
}

/* The objects every step starts from (both chains build them identically). */
static void build_objects(const bind_case *k, CPU *c, uint32_t entry)
{
    uint32_t i, noise = k->target_noise;
    uint32_t stack_obj = entry + ORACLE_STACK_OBJECT;   /* below the ceiling */

    memset(s_shader, 0, sizeof s_shader);
    W(s_shader, 0x00) = k->vtable ? k->vtable : O_SHADER_VTABLE;
    W(s_shader, 0x04) = 0x04040404u;
    W(s_shader, 0x10) = ADDR(s_descriptors);
    W(s_shader, 0x1c) = ADDR(s_records);
    W(s_shader, 0x20) = ADDR(s_records) + (k->list_mode ? 0u : k->records * 4u);
    W(s_shader, 0x24) = ADDR(s_records) + 16u;
    W(s_shader, 0x28) = k->program;
    W(s_shader, 0x2c) = 0x2c2c2c2cu;
    memset(s_alt_list, 0, sizeof s_alt_list);
    s_alt_list[0] = ADDR(s_records);
    s_alt_list[1] = ADDR(s_records) + (k->list_mode ? k->records * 4u : 0u);
    for (i = 0u; i < 4u; ++i)
        s_records[i] = 0u;               /* descriptor index 0 */
    memset(s_descriptors, 0, sizeof s_descriptors);
    W(s_descriptors, 0x00) = O_NAME_TEXTURE0;
    W(s_descriptors, 0x08) = 0x15u;      /* an int uniform type */
    W(s_descriptors, 0x0c) = ADDR(s_records);   /* empty value vector: */
    W(s_descriptors, 0x10) = ADDR(s_records);   /* the loop calls nothing */

    memset(s_image_vt, 0, sizeof s_image_vt);
    for (i = 0u; i < 32u; ++i)
        s_image_vt[i] = 0xdead0000u | i;
    W(s_image_vt, 0x48) = k->image_empty_fn ? k->image_empty_fn : VA(0x0055f900u);
    memset(s_image, 0, sizeof s_image);
    for (i = 0u; i < 40u; ++i)
        s_image[i] = 0x1a000000u | i;
    W(s_image, 0x00) = ADDR(s_image_vt);
    W(s_image, 0x18) = k->image_handle;
    W(s_image, 0x20) = k->image_kind;
    W(s_image, 0x78) = k->tex0;
    W(s_image, 0x7c) = k->tex1;

    for (i = 0u; i < 16u; ++i)
        s_target_vt[i] = 0xbeef0000u | i;
    W(s_target_vt, 0x28) = k->wget;
    W(s_target_vt, 0x2c) = k->hget;
    for (i = 0u; i < 72u; ++i) {
        noise = noise * 1664525u + 1013904223u;
        s_target[i] = noise;
    }
    W(s_target, 0x00) = ADDR(s_target_vt);
    for (i = 0u; i < O_GETTER_COUNT; ++i) {
        uint32_t value;
        if (s_getter_va[i] != k->wget && s_getter_va[i] != k->hget)
            continue;
        value = s_getter_va[i] == k->wget ? k->wval : k->hval;
        if (s_getter_half[i])
            st16(ADDR(s_target) + s_getter_off[i], (uint16_t)value);
        else
            st32(ADDR(s_target) + s_getter_off[i], value);
    }
    memset(s_cur_shader, 0, sizeof s_cur_shader);
    W(s_cur_shader, 0x00) = O_SHADER_VTABLE;
    W(s_cur_shader, 0x28) = k->cur_program;

    /* Objects placed on the synthetic stack (above the frame) instead. */
    if (k->self_on_stack) {
        for (i = 0u; i < 16u; ++i) st32(stack_obj + i * 4u, s_shader[i]);
        c->ecx = stack_obj;
    } else {
        c->ecx = ADDR(s_shader);
    }
    st32(O_IMAGE_PTR, !k->image ? 0u :
         k->image_on_stack ? stack_obj : ADDR(s_image));
    if (k->image_on_stack)
        for (i = 0u; i < 40u; ++i) st32(stack_obj + i * 4u, s_image[i]);
    st32(O_MANAGER + 0x14u, !k->shader ? 0u :
         k->shader_on_stack ? stack_obj : ADDR(s_cur_shader));
    if (k->shader_on_stack)
        for (i = 0u; i < 16u; ++i) st32(stack_obj + i * 4u, s_cur_shader[i]);
    st32(O_MANAGER + 0x18u, !k->target ? 0u :
         k->target_on_stack ? stack_obj : ADDR(s_target));
    if (k->target_on_stack)
        for (i = 0u; i < 72u; ++i) st32(stack_obj + i * 4u, s_target[i]);
    st32(O_MANAGER + 0x1cu, (k->flag_word_noise & 0xffffff00u) | k->flag);
    st32(O_MANAGER + 0x10u, 0x10101010u);
    st32(O_MANAGER + 0x20u, 0x20202020u);
    st32(O_COOKIE, k->cookie ? k->cookie : s_pe_cookie);
    st32(O_DEFAULT_W, k->alt_defaults ? 0x44700000u : s_pe_default_w); /* 960.0 */
    st32(O_DEFAULT_H, k->alt_defaults ? 0x44080000u : s_pe_default_h); /* 544.0 */
    st32(O_SHADER_VTABLE + 0x1cu, k->sui_slot ? k->sui_slot : VA(0x00567430u));
    for (i = 0u; i < 6u; ++i)
        st32(s_slot_va[i], slot_value(i, s_slot_token[i], k->slot_bad[i]));
}

static void prepare_cpu(const bind_case *k, CPU *c, uint32_t *entry_out)
{
    uint32_t floor = c->stack_floor, ceiling = c->stack_ceiling;
    uint32_t esp = k->esp_offset ? floor + k->esp_offset :
                   k->esp_top ? ceiling - 4u : ceiling - ORACLE_ABOVE_ENTRY;
    uint32_t i, j;

    /* Below the ceiling a pattern; above it zero (a frame at the ceiling
     * reads its argument there: 0 selects the Shader's own list). */
    for (i = floor - ORACLE_STACK_GUARD_WORDS * 4u; i < ceiling; i += 4u)
        st32(i, 0xcdcd0000u | (i & 0xffffu));
    for (i = ceiling; i < ceiling + ORACLE_STACK_GUARD_WORDS * 4u; i += 4u)
        st32(i, 0u);
    c->eax = 0x11110000u;
    c->edx = 0x33330000u;
    c->ebx = 0x44440000u;
    c->esp = esp;
    c->ebp = 0x55550000u;
    c->esi = 0x66660000u;
    c->edi = 0x77770000u;
    for (i = 0u; i < 8u; ++i)
        for (j = 0u; j < 4u; ++j)
            c->x[i].u32[j] = 0xa0000000u | (i << 8) | j;
    for (i = 0u; i < 8u; ++i)
        c->st[i] = 0.5 + (double)i;
    c->st_top = 0;
    c->fsw = 0x0100u;
    c->f_a = 0x0f0f0f0fu;
    c->f_b = 0x01010101u;
    c->f_r = 0x10101010u;
    c->f_op = 3u;
    c->f_sz = 4u;
    c->f_cf = 1u;
    c->f_of = 0u;
    c->f_zf = 1u;
    c->f_sf = 0u;
    c->f_pf = 1u;
    c->df = 0u;
    c->fault = NULL;
    c->fault_addr = 0u;
    c->exit_api = NULL;
    c->exit_code = 0;
    c->stop_kind = 0;
    c->stack_low_water = ceiling;
    st32(esp, ORACLE_RETURN_RVA);
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    /* The enclosing indirect target (Bind is reached through the vtable). */
    g_kage_guest_last_indirect_target = VA(0x005671d0u);
#endif
    if (esp + 8u <= ceiling)
        st32(esp + 4u, k->list_mode == 1 ? ADDR(s_alt_list) :
                       k->list_mode == 2 ? esp + 32u : 0u);
    if (k->list_mode == 2 && esp + 40u <= ceiling) {
        st32(esp + 32u, ADDR(s_records));
        st32(esp + 36u, ADDR(s_records));
    }
    *entry_out = esp;
}

/* Everything the two runs must agree on after the step. */
static void describe(char *out, size_t cap, const CPU *c, uint32_t entry,
                     int stop)
{
    size_t n = 0u;
    uint32_t i, at;
    uint64_t bits;

#define ODESC(...) (n += (size_t)snprintf(out + n, n < cap ? cap - n : 0u, __VA_ARGS__))
    ODESC("eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%+d ebp=%08x esi=%08x "
        "edi=%08x stop=%d fault=%s addr=%08x low=%+d\n",
        c->eax, c->ecx, c->edx, c->ebx, (int)(c->esp - entry), c->ebp,
        c->esi, c->edi, stop, c->fault ? c->fault : "-", c->fault_addr,
        (int)(c->stack_low_water - entry));
    for (i = 0u; i < 8u; ++i)
        ODESC("x%u=%08x.%08x.%08x.%08x%s", i, c->x[i].u32[3], c->x[i].u32[2],
            c->x[i].u32[1], c->x[i].u32[0], i == 3u || i == 7u ? "\n" : " ");
    ODESC("fl=%08x,%08x,%08x,%u,%u,%u,%u,%u,%u,%u df=%u fsw=%04x top=%d st=",
        c->f_a, c->f_b, c->f_r, c->f_op, c->f_sz, c->f_cf, c->f_of,
        c->f_zf, c->f_sf, c->f_pf, c->df, c->fsw, c->st_top);
    for (i = 0u; i < 8u; ++i) {
        memcpy(&bits, &c->st[i], sizeof bits);
        ODESC("%016llx%s", (unsigned long long)bits, i < 7u ? "," : "\n");
    }
    /* From the entry ESP (the return word) up through the guard above the
     * ceiling; the dead stack below the final ESP is not compared. */
    ODESC("frame=");
    for (at = entry; at < c->stack_ceiling + ORACLE_STACK_GUARD_WORDS * 4u;
            at += 4u)
        ODESC("%08x,", ld32(at));
    ODESC("\nobjects=");
    for (i = 0u; i < 16u; ++i) ODESC("%08x", s_shader[i]);
    for (i = 0u; i < 40u; ++i) ODESC("%08x", s_image[i]);
    for (i = 0u; i < 72u; ++i) ODESC("%08x", s_target[i]);
    for (i = 0u; i < 4u; ++i) ODESC("%08x", s_alt_list[i]);
    for (i = 0u; i < 0x30u; i += 4u) ODESC("%08x", ld32(O_MANAGER + i));
    ODESC("%08x%08x", ld32(O_IMAGE_PTR), ld32(O_COOKIE));
    for (i = 0u; i < 6u; ++i) ODESC("%08x", ld32(s_slot_va[i]));
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    ODESC("\nsampler=%08x", g_kage_guest_last_indirect_target);
#endif
    ODESC("\n");
#undef ODESC
}

static void act_rename(int on)
{
    st8(O_NAME_TEXTURE0 + 6u, on ? (uint8_t)'f' : (uint8_t)'e');
}

static void act_transform_name(int on)
{
    st8(O_NAME_TRANSFORM, on ? (uint8_t)'x' : (uint8_t)'T');
}

/* Issue program-state mutator `which` (1..7) through the installed
 * (recorded) table: a FENCE record that resets the producer location cache
 * and waits for the consumer, which advances the fake GL's link epoch. */
static void act_mutate(int which, uint32_t program)
{
    const guest_gl_backend *b = guest_gl_installed_backend();

    switch (which) {
    case 1: b->glAttachShader(program, 3u); break;
    case 2: b->glCompileShader(3u); break;
    case 3: (void)b->glCreateProgram(); break;
    case 4: b->glDeleteProgram(program); break;
    case 5: b->glDeleteShader(3u); break;
    case 6: b->glLinkProgram(program); break;
    default: b->glShaderSource(3u, 0, 0u, 0u); break;
    }
}

static void act_prime(const bind_case *k)
{
    const guest_gl_backend *b = guest_gl_installed_backend();

    (void)b->glGetUniformLocation(W(s_shader, 0x28), O_NAME_TEXTURE0);
    (void)b->glGetUniformLocation(W(s_shader, 0x28), O_NAME_TEXTURE1);
    (void)b->glGetUniformLocation(k->cur_program, O_NAME_TRANSFORM);
}

#define ORACLE_MAX_STEPS 8
typedef struct step_result {
    char trace[1u << 15];
    char state[1u << 14];
    census delta;
    unsigned records, syncs;
    unsigned packet_replays;      /* opcode census: REC_KAGE_BIND_PACKET */
    int stop;
} step_result;

static step_result s_ref[ORACLE_MAX_STEPS];
static step_result s_seam[ORACLE_MAX_STEPS];
static uint32_t s_chain_epoch = 1u;

/* One chain: install, then each step's action and Bind.  Returns 0 on a
 * trace overflow. */
static int run_chain(const bind_case *k, guest_fn body, step_result *out)
{
    CPU *c = k->foreign ? &s_foreign : &s_cpu;
    uint32_t saved_floor = c->stack_floor, saved_ceiling = c->stack_ceiling;
    int step, renamed = 0, tnamed = 0, ok = 1;

    /* An image-backed stack on the owner CPU (so only the frame check can
     * decline it): the bounds move into the committed image page. */
    if (k->image_stack) {
        c->stack_floor = O_IMAGE_STACK + ORACLE_STACK_GUARD_WORDS * 4u;
        c->stack_ceiling = c->stack_floor + ORACLE_STACK_WORDS * 4u;
    }
    install(!k->unrecorded, s_chain_epoch);
    for (step = 0; step < k->steps; ++step) {
        step_result *r = &out[step];
        IsaacGlRecordWorkerWindow window;
        census before;
        uint32_t entry;
        int act = k->act[step];

        prepare_cpu(k, c, &entry);
        build_objects(k, c, entry);
        if (renamed) act_rename(1);
        if (tnamed) act_transform_name(1);
        if (act == ACT_PROGRAM)
            W(s_shader, 0x28) = k->program + 1u + (uint32_t)step;
        if (act >= ACT_MUTATE_FIRST && act <= ACT_MUTATE_LAST)
            act_mutate(act, k->program);
        if (act == ACT_RENAME) { act_rename(1); renamed = 1; }
        if (act == ACT_UNRENAME) { act_rename(0); renamed = 0; }
        if (act == ACT_TRANSFORM_NAME) { act_transform_name(1); tnamed = 1; }
        if (act == ACT_PRIME)
            act_prime(k);
        guest_register(s_reg_addrs, s_variant ? s_reg_mem : s_reg_gpr,
                       (uint32_t)O_REG_COUNT);
        isaac_gl_record_drain();
        isaac_gl_record_worker_take(NULL);
        trace_reset();
        census_take(&before);
        s_body = body;
        r->stop = guest_run_until_stop(c, oracle_entry);
        isaac_gl_record_drain();
        isaac_gl_record_worker_take(&window);
        r->records = window.records - 1u;
        r->syncs = window.syncs;
#if defined(ISAAC_GL_RECORD_OPCODE_CENSUS)
        /* REC_KAGE_BIND_PACKET's slot: the last one (static assert in
         * gl_record_backend.c), unless RENDER60 lane C appends
         * REC_DRAW_PACKET behind it and names this slot (gl_record_backend.h). */
# if defined(ISAAC_GL_RECORD_KAGE_BIND_PACKET_OPCODE)
        r->packet_replays = isaac_gl_record_opcode_census()->op[
            ISAAC_GL_RECORD_KAGE_BIND_PACKET_OPCODE].n;
# else
        r->packet_replays = isaac_gl_record_opcode_census()->op[
            ISAAC_GL_RECORD_CENSUS_OPS - 1u].n;
# endif
#else
        r->packet_replays = 0u;
#endif
        census_take(&r->delta);
        census_delta(&before, &r->delta);
        describe(r->state, sizeof r->state, c, entry, r->stop);
        scrub_owner_word(c);
        if (s_trace_overflow || s_ctrace_overflow ||
                s_trace_len + s_ctrace_len + 1u > sizeof r->trace) {
            ok = 0;
            break;
        }
        memcpy(r->trace, s_trace, s_trace_len);
        memcpy(r->trace + s_trace_len, s_ctrace, s_ctrace_len + 1u);
    }
    act_rename(0);
    act_transform_name(0);
    c->stack_floor = saved_floor;
    c->stack_ceiling = saved_ceiling;
    return ok;
}

static unsigned s_cases, s_steps, s_failures;
static unsigned s_handled_steps, s_declined_steps, s_cold_steps;
static unsigned s_memo_hits, s_cache_hits, s_ref_records, s_seam_records;
static unsigned s_reason_hits[R_COUNT];

static void fail(const bind_case *k, const char *tag, int step, const char *why)
{
    ++s_failures;
    printf("FAIL %s [%s] step %d: %s\n", k->name, tag, step, why);
}

static void compare_variant(const bind_case *k, int variant)
{
    const char *tag = variant ? "mem" : "gpr";
    guest_fn ref = variant ? mem_sub_005671d0 : sub_005671d0;
    guest_fn seam = variant ? mem_wrap_sub_005671d0 : __wrap_sub_005671d0;
    int step;

    s_variant = variant;
    if (!run_chain(k, ref, s_ref) || !run_chain(k, seam, s_seam)) {
        fail(k, tag, -1, "trace overflow");
        return;
    }
    s_chain_epoch += 3u;
    ++s_cases;
    for (step = 0; step < k->steps; ++step) {
        const step_result *r = &s_ref[step];
        const step_result *s = &s_seam[step];
        unsigned verdicts = s->delta.handled + s->delta.declined;

        ++s_steps;
        printf("case %-28s %s #%d verdict=%u/%u calls=%u,%u dyn=%u,%u be=%u,%u "
               "rec=%u,%u sync=%u,%u cold=%u memo=%u cache=%u pk=%u ent=%u,%u\n",
               k->name, tag, step, s->delta.handled, s->delta.declined,
               r->delta.calls, s->delta.calls, r->delta.dyn, s->delta.dyn,
               r->delta.backend, s->delta.backend, r->records, s->records,
               r->syncs, s->syncs, s->delta.cold, s->delta.memo,
               s->delta.cache, s->delta.packets, r->delta.entries,
               s->delta.entries);
        if (r->delta.handled || r->delta.declined)
            fail(k, tag, step, "the reference reached the seam");
        if (verdicts != 1u || (int)s->delta.handled != k->expect_handled)
            fail(k, tag, step, "seam verdict differs from the expectation");
        if (strcmp(r->trace, s->trace) != 0) {
            fail(k, tag, step, "GL call stream differs");
            printf("--- reference\n%s--- seam\n%s", r->trace, s->trace);
        }
        if (strcmp(r->state, s->state) != 0) {
            fail(k, tag, step, "CPU/stack/object state differs");
            printf("--- reference\n%s--- seam\n%s", r->state, s->state);
        }
        if (r->stop != s->stop)
            fail(k, tag, step, "stop kind differs");
        if (r->delta.backend != s->delta.backend)
            fail(k, tag, step, "backend call count differs");
        if (r->delta.entries != s->delta.entries)
            fail(k, tag, step, "body-entry census differs");
        if (!k->unrecorded && r->syncs != s->syncs)
            fail(k, tag, step, "synchronous lookups differ (location-cache "
                               "transitions)");
        if (s->delta.handled) {
            ++s_handled_steps;
            s_cold_steps += s->delta.cold;
            s_memo_hits += s->delta.memo;
            s_cache_hits += s->delta.cache;
            s_ref_records += r->records;
            s_seam_records += s->records;
            if (s->delta.calls || s->delta.dyn)
                fail(k, tag, step, "handled seam advanced a dispatch census");
            if (s->records != s->delta.packets + s->delta.cold)
                fail(k, tag, step, "handled seam ring records != packets + cold");
            if (s->delta.cold != s->delta.miss || s->delta.cold != s->syncs)
                fail(k, tag, step, "cold lookups != stage misses != syncs");
            if (r->records != r->delta.backend)
                fail(k, tag, step, "reference ring records != GL calls");
            if (s->delta.staged + s->delta.cold != r->delta.backend)
                fail(k, tag, step, "staged + cold != reference GL calls");
            if (r->delta.dyn != r->delta.backend)
                fail(k, tag, step, "reference crossings != GL calls");
#if defined(ISAAC_GL_RECORD_OPCODE_CENSUS)
            if (s->packet_replays != s->delta.packets || r->packet_replays)
                fail(k, tag, step, "opcode census: packet replays != packets");
#endif
        } else {
            int why, hits = 0, first = -1;

            ++s_declined_steps;
            for (why = 0; why < R_COUNT; ++why)
                if (s->delta.why[why]) {
                    hits += (int)s->delta.why[why];
                    if (first < 0)
                        first = why;
                }
            if (hits != 1 || first != k->reason) {
                fail(k, tag, step, "declined by another check");
                printf("    expected %s, got %s (%d)\n",
                       k->reason >= 0 ? s_reason_name[k->reason] : "-",
                       first >= 0 ? s_reason_name[first] : "-", hits);
            } else {
                ++s_reason_hits[first];
            }
            if (s->delta.calls != r->delta.calls || s->delta.dyn != r->delta.dyn)
                fail(k, tag, step, "declined seam changed a census");
            if (s->records != r->records || s->delta.packets || s->delta.staged)
                fail(k, tag, step, "declined seam staged a packet");
        }
    }
}

static void compare_both(const bind_case *k)
{
    compare_variant(k, 0);
    compare_variant(k, 1);
}

/* The game shape: Texture0 image (handle 0, so IsEmpty binds Texture1 too),
 * a render target with the most common getter pair, a current shader. */
static void case_init(bind_case *k, const char *name)
{
    memset(k, 0, sizeof *k);
    k->name = name;
    k->expect_handled = 1;
    k->reason = -1;
    k->steps = 1;
    k->program = 5u;
    k->image = 1;
    k->image_handle = 0u;
    k->image_kind = 1u;
    k->tex0 = 0x1000u;
    k->tex1 = 0x2000u;
    k->target = 1;
    k->wget = VA(0x005a0b90u);
    k->hget = VA(0x005a0ba0u);
    k->wval = 960u;
    k->hval = 544u;
    k->target_noise = 0x5eed1234u;
    k->flag = 0u;
    k->flag_word_noise = 0xa5a5a500u;
    k->shader = 1;
    k->cur_program = 7u;
}

int main(int argc, char **argv)
{
    static guest_import imports[4];
    static char names[256][48];
    unsigned name_index = 0u;
    bind_case k;
    uint32_t i, j, floor, ceiling;
    int a;

#define NAME(...) \
    (snprintf(names[name_index], sizeof names[0], __VA_ARGS__), \
     names[name_index++])

    /* A crash must not eat the buffered case lines. */
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: oracle rva:hex ... (frozen PE spans)\n");
        return 2;
    }
    for (i = 0u; i < sizeof s_image_pages / sizeof s_image_pages[0]; ++i)
        if (!commit_image_page(VA(s_image_pages[i]), ORACLE_PAGE)) {
            fprintf(stderr, "could not commit image page %08x (build with "
                            "/LARGEADDRESSAWARE)\n", VA(s_image_pages[i]));
            return 2;
        }
    for (a = 1; a < argc; ++a)
        if (!install_span(argv[a])) {
            fprintf(stderr, "bad span argument %s\n", argv[a]);
            return 2;
        }
    if (strcmp((const char *)(uintptr_t)O_NAME_TEXTURE0, "Texture0") != 0 ||
            strcmp((const char *)(uintptr_t)O_NAME_TEXTURE1, "Texture1") != 0 ||
            strcmp((const char *)(uintptr_t)O_NAME_TRANSFORM, "Transform") != 0 ||
            ld32(O_SHADER_VTABLE + 0x10u) != VA(0x005671d0u) ||
            ld32(O_SHADER_VTABLE + 0x1cu) != VA(0x00567430u)) {
        fprintf(stderr, "frozen spans missing or drifted\n");
        return 2;
    }
    s_pe_cookie = ld32(O_COOKIE);
    s_pe_default_w = ld32(O_DEFAULT_W);
    s_pe_default_h = ld32(O_DEFAULT_H);
    for (i = 0u; i < 6u; ++i) {
        s_slot_token[i] = guest_gl_resolve(s_slot_fn[i]);
        if (!s_slot_token[i] || !guest_gl_resolve(s_slot_foreign[i]) ||
                guest_gl_resolve(s_slot_foreign[i]) == s_slot_token[i]) {
            fprintf(stderr, "registry drifted: %s / %s\n", s_slot_fn[i],
                    s_slot_foreign[i]);
            return 2;
        }
    }

    guest_cpu_init(&s_cpu);
    guest_cpu_init(&s_foreign);
    floor = ADDR(&s_stack_storage[ORACLE_STACK_GUARD_WORDS]);
    ceiling = ADDR(&s_stack_storage[ORACLE_STACK_GUARD_WORDS + ORACLE_STACK_WORDS]);
    if (guest_stack_bind(&s_cpu, floor, ceiling) ||
            guest_stack_bind(&s_foreign,
                ADDR(&s_foreign_storage[ORACLE_STACK_GUARD_WORDS]),
                ADDR(&s_foreign_storage[ORACLE_STACK_GUARD_WORDS +
                                        ORACLE_STACK_WORDS]))) {
        fprintf(stderr, "stack bind failed\n");
        return 2;
    }
    /* Production-shaped import table: the 0x7e-first fact holds. */
    imports[0].slot_rva = 0x00606104u; imports[0].name = "KERNEL32.dll!FreeLibrary";
    imports[1].slot_rva = 0x00606118u; imports[1].name = "KERNEL32.dll!GetProcAddress";
    imports[2].slot_rva = 0x00606124u; imports[2].name = "KERNEL32.dll!LoadLibraryA";
    imports[3].slot_rva = 0x006064c0u; imports[3].name = "WINMM.dll!timeGetTime";
    guest_register_imports(imports, 4u);
    if (!guest_gl_dynamic_first_holds()) {
        fprintf(stderr, "0x7e-first startup fact does not hold\n");
        return 2;
    }
    if (!isaac_gl_record_worker_start(NULL) || !isaac_gl_record_worker_active()) {
        fprintf(stderr, "GL recorder worker did not start\n");
        return 2;
    }
    /* The generated-unit spellings (stack guard, leaf inline) are the
     * bodies' and the seam's, set by the test per flavour. */
    printf("=== real typed recorder + consumer thread (worker mode %s) ===\n",
           ISAAC_GL_RECORD_WORKER_MODE_NAME);

    printf("=== game shapes ===\n");
    case_init(&k, "game-shape");
    compare_both(&k);
    case_init(&k, "image-not-empty");
    k.image_handle = 0x77u;
    compare_both(&k);
    case_init(&k, "no-image");
    k.image = 0;
    compare_both(&k);
    case_init(&k, "image-kind2");
    k.image_kind = 2u;
    compare_both(&k);
    case_init(&k, "image-kind2-not-empty");
    k.image_kind = 2u;
    k.image_handle = 1u;
    compare_both(&k);
    case_init(&k, "no-target-defaults");
    k.target = 0;
    compare_both(&k);
    case_init(&k, "flag-set-defaults");
    k.flag = 1u;
    compare_both(&k);
    case_init(&k, "flag-0x80-defaults");
    k.flag = 0x80u;
    k.flag_word_noise = 0u;
    compare_both(&k);
    case_init(&k, "alt-defaults-no-target");
    k.target = 0;
    k.alt_defaults = 1;
    compare_both(&k);
    case_init(&k, "no-shader");
    k.shader = 0;
    compare_both(&k);
    case_init(&k, "no-shader-no-image");
    k.shader = 0;
    k.image = 0;
    compare_both(&k);
    case_init(&k, "no-shader-no-target");
    k.shader = 0;
    k.target = 0;
    compare_both(&k);
    case_init(&k, "list-argument");
    k.list_mode = 1;
    compare_both(&k);
    case_init(&k, "other-cookie");
    k.cookie = 0x12345678u;
    compare_both(&k);
    case_init(&k, "program-0");
    k.program = 0u;
    compare_both(&k);
    case_init(&k, "program-big");
    k.program = 0xfffffff0u;
    k.cur_program = 0x80000000u;
    compare_both(&k);
    case_init(&k, "transform-missing-uniform");
    k.steps = 2;
    k.act[0] = ACT_TRANSFORM_NAME;
    compare_both(&k);

    printf("=== target getters: every pair, value sweep ===\n");
    for (i = 0u; i < O_GETTER_COUNT; ++i)
        for (j = 0u; j < O_GETTER_COUNT; ++j) {
            case_init(&k, NAME("getters-%u-%u", i, j));
            k.wget = s_getter_va[i];
            k.hget = s_getter_va[j];
            k.wval = 640u + i * 17u;
            k.hval = 360u + j * 13u;
            k.target_noise = 0x1000u + i * 8u + j;
            compare_both(&k);
        }
    {
        static const uint32_t values[] = {
            0u, 1u, 2u, 480u, 960u, 4096u, 65535u, 65536u, 0x7fffffffu,
            0x80000000u, 0x80000001u, 0xfffffffeu, 0xffffffffu, 0x01000001u,
        };
        const uint32_t nv = (uint32_t)(sizeof values / sizeof values[0]);
        for (i = 0u; i < nv; ++i) {
            case_init(&k, NAME("values-w%08x", values[i]));
            k.wval = values[i];
            k.hval = values[(i + 5u) % nv];
            compare_both(&k);
        }
        for (i = 0u; i < nv; ++i) {
            case_init(&k, NAME("values-half-%08x", values[i]));
            k.wget = VA(0x005a0dd0u);
            k.hget = VA(0x005a0de0u);
            k.wval = values[i];
            k.hval = values[(i + 3u) % nv];
            compare_both(&k);
        }
    }
    for (i = 0u; i < 48u; ++i) {
        case_init(&k, NAME("random-%u", i));
        k.wget = s_getter_va[next_random() % O_GETTER_COUNT];
        k.hget = s_getter_va[next_random() % O_GETTER_COUNT];
        k.wval = next_random();
        k.hval = next_random();
        k.target_noise = next_random();
        k.image = (int)(next_random() % 4u != 0u);
        k.image_handle = next_random() % 3u;
        k.image_kind = next_random() % 4u;
        k.tex0 = next_random();
        k.tex1 = next_random();
        k.target = (int)(next_random() % 5u != 0u);
        k.flag = (uint8_t)(next_random() % 3u == 0u ? next_random() : 0u);
        k.shader = (int)(next_random() % 6u != 0u);
        k.program = next_random() % 64u;
        k.cur_program = next_random() % 64u;
        k.alt_defaults = (int)(next_random() & 1u);
        compare_both(&k);
    }

    printf("=== chains: producer location cache, name memo, epochs ===\n");
    case_init(&k, "chain-cold-cache-memo");
    k.steps = 4;
    compare_both(&k);
    case_init(&k, "chain-primed");
    k.steps = 3;
    k.act[0] = ACT_PRIME;
    compare_both(&k);
    for (a = ACT_MUTATE_FIRST; a <= ACT_MUTATE_LAST; ++a) {
        case_init(&k, NAME("chain-mutator-%d", a));
        k.steps = 5;
        k.act[3] = a;
        compare_both(&k);
    }
    case_init(&k, "chain-rename-unrename");
    k.steps = 7;
    k.act[3] = ACT_RENAME;
    k.act[5] = ACT_UNRENAME;
    compare_both(&k);
    case_init(&k, "chain-program-change");
    k.steps = 6;
    k.act[3] = ACT_PROGRAM;
    k.act[4] = ACT_PROGRAM;
    compare_both(&k);
    case_init(&k, "chain-no-shader-memo");
    k.steps = 4;
    k.shader = 0;
    compare_both(&k);
    /* The shape the game binds: Texture0 only (A2S P2 MEASURED one
     * glActiveTexture and two uniform lookups per draw), whose steady state
     * is 7 -> 1;
     * and a Bind with no current texture. */
    case_init(&k, "chain-image-not-empty");
    k.steps = 4;
    k.image_handle = 0x77u;
    compare_both(&k);
    case_init(&k, "chain-no-image");
    k.steps = 4;
    k.image = 0;
    compare_both(&k);
    case_init(&k, "chain-mutate-then-prime");
    k.steps = 6;
    k.act[2] = 6;
    k.act[3] = ACT_PRIME;
    compare_both(&k);

    printf("=== declines (the translated body runs) ===\n");
    case_init(&k, "decline-foreign-vtable");
    k.expect_handled = 0;
    k.reason = R_OBJECT;
    k.vtable = O_SHADER_VTABLE + 4u;    /* never read: no Image, no SetUniformInt */
    k.image = 0;
    compare_both(&k);
    case_init(&k, "decline-foreign-setuniformint");
    k.expect_handled = 0;
    k.reason = R_OBJECT;
    k.sui_slot = O_FOREIGN_SUI;
    compare_both(&k);
    case_init(&k, "decline-record-list-1");
    k.expect_handled = 0;
    k.reason = R_RECORDS;
    k.records = 1u;
    compare_both(&k);
    case_init(&k, "decline-list-argument-1");
    k.expect_handled = 0;
    k.reason = R_RECORDS;
    k.list_mode = 1;
    k.records = 1u;
    compare_both(&k);
    case_init(&k, "decline-list-on-stack");
    k.expect_handled = 0;
    k.reason = R_OBJECT;
    k.list_mode = 2;
    compare_both(&k);
    case_init(&k, "decline-self-on-stack");
    k.expect_handled = 0;
    k.reason = R_OBJECT;
    k.self_on_stack = 1;
    k.image = 0;
    compare_both(&k);
    case_init(&k, "decline-foreign-isempty");
    k.expect_handled = 0;
    k.reason = R_IMAGE;
    k.image_empty_fn = O_FOREIGN_EMPTY;
    compare_both(&k);
    case_init(&k, "decline-image-on-stack");
    k.expect_handled = 0;
    k.reason = R_IMAGE;
    k.image_on_stack = 1;
    compare_both(&k);
    case_init(&k, "decline-foreign-width-getter");
    k.expect_handled = 0;
    k.reason = R_TARGET;
    k.wget = O_FOREIGN_GETTER;
    compare_both(&k);
    case_init(&k, "decline-foreign-height-getter");
    k.expect_handled = 0;
    k.reason = R_TARGET;
    k.hget = O_FOREIGN_GETTER;
    compare_both(&k);
    case_init(&k, "foreign-getter-unused-flag");
    k.wget = O_FOREIGN_GETTER;          /* the flag skips the getters */
    k.flag = 1u;
    compare_both(&k);
    case_init(&k, "decline-target-on-stack");
    k.expect_handled = 0;
    k.reason = R_TARGET;
    k.target_on_stack = 1;
    compare_both(&k);
    case_init(&k, "decline-shader-on-stack");
    k.expect_handled = 0;
    k.reason = R_TARGET;
    k.shader_on_stack = 1;
    compare_both(&k);
    for (i = 0u; i < 6u; ++i)
        for (a = 1; a <= 3; ++a) {
            case_init(&k, NAME("decline-slot-%s-%d", s_slot_fn[i] + 2, a));
            k.expect_handled = 0;
            k.reason = R_TOKENS;
            k.slot_bad[i] = a;
            compare_both(&k);
        }
    case_init(&k, "decline-shallow-stack-100");
    k.expect_handled = 0;
    k.reason = R_FRAME;
    k.esp_offset = 100u;
    compare_both(&k);
    case_init(&k, "decline-shallow-no-shader");
    k.expect_handled = 0;
    k.reason = R_FRAME;
    k.esp_offset = 104u;
    k.shader = 0;
    compare_both(&k);
    case_init(&k, "exact-depth-116");
    k.esp_offset = 116u;
    compare_both(&k);
    case_init(&k, "decline-no-ret-window");
    k.expect_handled = 0;
    k.reason = R_FRAME;
    k.esp_top = 1;
    k.image = 0;
    k.shader = 0;
    compare_both(&k);
    case_init(&k, "decline-unrecorded-backend");
    k.expect_handled = 0;
    k.reason = R_RECORDER;
    k.unrecorded = 1;
    compare_both(&k);
    case_init(&k, "decline-image-backed-stack");
    k.expect_handled = 0;
    k.reason = R_FRAME;
    k.image_stack = 1;
    compare_both(&k);
    case_init(&k, "decline-foreign-cpu");
    k.expect_handled = 0;
    k.reason = R_OWNER;
    k.foreign = 1;
    compare_both(&k);

    printf("log lines=%u\n%s", s_log_lines, s_log);
#if defined(ORACLE_STAGE_WORDS)
    printf("stage words: checks=%u mismatches=%u\n", s_stage_checks,
           s_stage_mismatches);
    if (!s_stage_checks || s_stage_mismatches) {
        ++s_failures;
        printf("FAIL stage words: the sampler word is not the slot token "
               "while a call is staged\n");
    }
#endif
    if (s_failures) {
        printf("Vita KAGE bind packet oracle: FAIL; failures=%u\n", s_failures);
        return 1;
    }
    printf("declines by reason:");
    for (i = 0u; i < R_COUNT; ++i)
        printf(" %s=%u", s_reason_name[i], s_reason_hits[i]);
    printf("\n");
    printf("Vita KAGE bind packet oracle: PASS; cases=%u; steps=%u; "
           "handled=%u; declined=%u; cold=%u; memo=%u; cache=%u; "
           "records(ref,seam)=%u,%u\n",
           s_cases, s_steps, s_handled_steps, s_declined_steps, s_cold_steps,
           s_memo_hits, s_cache_hits, s_ref_records, s_seam_records);
    return 0;
#undef NAME
}
