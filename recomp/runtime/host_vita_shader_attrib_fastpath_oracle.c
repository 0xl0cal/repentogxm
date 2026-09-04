/* Differential host oracle for ISAAC_VITA_SHADER_ATTRIB_FASTPATH
 * (wf/opt-attrib; driven by recomp/test_vita_shader_attrib_fastpath.py).
 *
 * The production dispatcher (guest.c, included), gl_bridge.c and
 * host_vita_gl.c are built with ISAAC_VITA_GL_SHIM_FASTDISPATCH and the
 * attribute option.  The two translated bodies of the frozen corpus unit
 * (sub_005672d0 Shader::EnableAttribs, sub_005673f0 Shader::DisableAttribs,
 * extracted verbatim by the test) are linked twice: once with their host
 * seam compiled in (the production shape: `if (isaac_vita_shader_attribs_
 * *_try(c)) return;` at the root) and once without it (the reference: the
 * exact translated loop dispatching each GL call through guest_call), in
 * both GPR spellings (GUEST_GPR_LOCAL=1 production, 0 memory mode).
 *
 * Every case runs the reference and the seamed body on byte-identical
 * synthetic state (Shader object, attribute table, guest stack, register
 * file, the relocated format jump tables and the four function-pointer slot
 * words at their real image addresses) and requires the recorded backend
 * call stream (function, arguments, order), the CPU after the return
 * (eight registers, fault, stop kind), the caller's frame words and the
 * Shader object bytes to be identical.  The censuses are checked against
 * the design: a handled replay leaves g_host_dynamic_calls and the guest_call
 * census untouched while the reference advances both by one per GL call;
 * a declined replay leaves every census equal to the reference.  Hostile
 * inputs (foreign vtable, unknown formats, null program/name, count above
 * the bound, every slot word corrupted - zero, a near token, a foreign
 * registered token - an import table that reaches the token family, a stack
 * too shallow for the translated frame, a frame straddling the ceiling, ESP
 * below the floor, re-entry from a GL callback, a foreign CPU) must decline
 * before the first wrapper runs; inputs the translated body cannot
 * execute at all (null attribute table, a count of 2^32-1) are probed
 * against the fast path alone and must leave the CPU untouched.
 *
 * Executed on a 32-bit identity-mapped host (MSVC x86, /LARGEADDRESSAWARE so
 * the 0x98xxxxxx image words can be committed at their real addresses). */
#include "guest.c"

#include <stdarg.h>

#include "gl_bridge.h"
#include "host_vita_gl.h"
#include "host_vita_import_id.h"

/* The fast path itself, with its two entry points renamed so the oracle can
 * wrap them and count verdicts; the seamed bodies call the public names. */
#define isaac_vita_shader_attribs_enable_try oracle_attrib_enable_impl
#define isaac_vita_shader_attribs_disable_try oracle_attrib_disable_impl
#include "host_vita_shader_attrib_fastpath.c"
#undef isaac_vita_shader_attribs_enable_try
#undef isaac_vita_shader_attribs_disable_try

#include <windows.h>

#if UINTPTR_MAX != UINT32_MAX
# error host_vita_shader_attrib_fastpath_oracle requires a 32-bit host
#endif
#if !defined(ISAAC_VITA_IMPORT_ID_DISPATCH) || \
    !defined(ISAAC_VITA_GUEST_LOOKUP_CACHE) || \
    !defined(ISAAC_VITA_PHASE_PROFILE) || GUEST_STACK_REQUIRED != 1 || \
    !defined(ISAAC_VITA_GL_SHIM_FASTDISPATCH) || \
    !defined(ISAAC_VITA_SHADER_ATTRIB_FASTPATH)
# error build this oracle with the production guest.c configuration
#endif
#if GUEST_IMAGE_BASE != 0x98000000u
# error the frozen corpus text is rebased to 0x98000000
#endif

/* ---- the translated bodies (four link-time variants) ------------------ */

void sub_005672d0(CPU *__restrict c);          /* seam, GPR locals */
void sub_005673f0(CPU *__restrict c);
void ref_sub_005672d0(CPU *__restrict c);      /* reference, GPR locals */
void ref_sub_005673f0(CPU *__restrict c);
void mem_sub_005672d0(CPU *__restrict c);      /* seam, memory mode */
void mem_sub_005673f0(CPU *__restrict c);
void mem_ref_sub_005672d0(CPU *__restrict c);  /* reference, memory mode */
void mem_ref_sub_005673f0(CPU *__restrict c);

/* ---- stubs owned by other Vita runtime units ------------------------ */

unsigned g_host_import_calls;
unsigned g_host_dynamic_calls;
static unsigned s_generic_import_calls;

int guest_host_import_ids_register(const guest_import *imports, uint32_t count)
{
    (void)imports;
    (void)count;
    return 1;
}

int guest_host_import_id(CPU *__restrict c, uint32_t import_id)
{
    ++s_generic_import_calls;
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

    if (family != UINT32_C(0x7d000000) &&
        family != UINT32_C(0x7e000000))
        return 0;
    return isaac_vita_gl_dynamic_counted(c, token, &g_host_dynamic_calls);
}

/* ---- trace ------------------------------------------------------------ */

static char s_trace[1u << 17];
static size_t s_trace_len;
static int s_trace_overflow;
static unsigned s_backend_calls;
static unsigned s_log_calls;
static unsigned s_handled;
static unsigned s_rejected;

static void trace(const char *format, ...)
{
    va_list ap;
    int n;

    if (s_trace_len >= sizeof s_trace - 1u) {
        s_trace_overflow = 1;
        return;
    }
    va_start(ap, format);
    n = vsnprintf(s_trace + s_trace_len, sizeof s_trace - s_trace_len,
                  format, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof s_trace - s_trace_len) {
        s_trace_overflow = 1;
        s_trace_len = sizeof s_trace - 1u;
        return;
    }
    s_trace_len += (size_t)n;
}

static void trace_reset(void)
{
    s_trace_len = 0u;
    s_trace[0] = '\0';
    s_trace_overflow = 0;
}

/* ---- verdict-counting wrappers around the fast path ------------------- */

int isaac_vita_shader_attribs_enable_try(CPU *__restrict c)
{
    int handled = oracle_attrib_enable_impl(c);
    if (handled)
        ++s_handled;
    else
        ++s_rejected;
    trace("fastpath enable %s\n", handled ? "HANDLED" : "REJECTED");
    return handled;
}

int isaac_vita_shader_attribs_disable_try(CPU *__restrict c)
{
    int handled = oracle_attrib_disable_impl(c);
    if (handled)
        ++s_handled;
    else
        ++s_rejected;
    trace("fastpath disable %s\n", handled ? "HANDLED" : "REJECTED");
    return handled;
}

/* ---- the translated log callee of the unknown-format branch ---------- */

/* sub_0055e330 is `push fmt; push 3; call; add esp, 8` (cdecl, two words).
 * The stub records the call, clobbers the caller-saved registers the way a
 * real callee may and consumes only its return word. */
void sub_0055e330(CPU *__restrict c)
{
    uint32_t level = ld32(c->esp + 4u);
    uint32_t text = ld32(c->esp + 8u);

    ++s_log_calls;
    trace("log level=%u text=%08x\n", level, text);
    c->eax = UINT32_C(0x11111111);
    c->ecx = UINT32_C(0x22222222);
    c->edx = UINT32_C(0x33333333);
    (void)gpop(c);
}

/* ---- oracle state ---------------------------------------------------- */

#define ORACLE_STACK_WORDS 256u
#define ORACLE_STACK_GUARD_WORDS 64u
#define ORACLE_RETURN_RVA UINT32_C(0x0056d5a0)
#define ORACLE_UNUSED_ARG UINT32_C(0xa5a50000)
#define ORACLE_MAX_ATTRIBS 17u

/* Guard words below the floor absorb the raw pushes of a translated body on
 * a stack too shallow for its frame (the adapters then fault on the checked
 * argument reads), so the stub storage next to the stack stays intact. */
static uint32_t s_stack_storage[ORACLE_STACK_GUARD_WORDS + ORACLE_STACK_WORDS];
static uint32_t s_foreign_storage[ORACLE_STACK_GUARD_WORDS + ORACLE_STACK_WORDS];

typedef struct shader_object {
    uint32_t vtable;    /* +0x00 */
    uint32_t pad04;
    uint32_t attribs;   /* +0x08 */
    uint32_t count;     /* +0x0c */
    uint32_t pad10, pad14, pad18, pad1c, pad20, pad24;
    uint32_t program;   /* +0x28 */
    uint32_t pad2c;
} shader_object;

typedef struct attrib_pair {
    uint32_t name;
    uint32_t format;
} attrib_pair;

static shader_object s_object;
static attrib_pair s_pairs[ORACLE_MAX_ATTRIBS];
static CPU s_cpu;
static CPU s_foreign;
static guest_fn s_body;
static guest_fn s_nested_body;
static uint32_t s_nested_base;
static uint32_t s_nested_stride;

static const char *const s_names[] = {
    "aPosition", "aTexCoord", "aColor", "aColorOffset", "aRenderData",
    "aScale", "aExtra0", "aExtra1", "aExtra2", "aExtra3", "aExtra4",
    "aExtra5", "aExtra6", "aExtra7", "aExtra8", "aExtra9", "aExtra10"
};
/* Names starting with 'x' answer -1 from the recording glGetAttribLocation. */
static const char *const s_missing_names[] = {
    "xMissing0", "xMissing1", "xMissing2"
};

/* ---- recording backend ------------------------------------------------ */

static guest_gl_int rec_glGetAttribLocation(guest_gl_uint program,
                                            guest_gl_addr name)
{
    const char *text = (const char *)(uintptr_t)name;
    uint32_t hash = program * 33u;
    const char *p;

    ++s_backend_calls;
    if (!text) {
        trace("be glGetAttribLocation(%u,%08x,<null>)\n", program, name);
        return -1;
    }
    for (p = text; *p; ++p)
        hash = hash * 31u + (unsigned char)*p;
    trace("be glGetAttribLocation(%u,%08x,\"%s\")\n", program, name, text);
    if (text[0] == 'x')
        return -1;
    return (guest_gl_int)(hash % 13u);
}

static void rec_glEnableVertexAttribArray(guest_gl_uint index)
{
    ++s_backend_calls;
    trace("be glEnableVertexAttribArray(%08x)\n", index);
    if (s_nested_body) {
        /* Re-entry from inside a GL callback: a nested thiscall frame on the
         * same CPU.  Both the reference and the seamed body must raise the
         * established "nested or concurrent guest GL dispatch" fault. */
        guest_fn body = s_nested_body;
        s_nested_body = NULL;
        trace("nested enter\n");
        gpush(&s_cpu, s_nested_stride);
        gpush(&s_cpu, ORACLE_UNUSED_ARG);
        gpush(&s_cpu, s_nested_base);
        gpush(&s_cpu, ORACLE_RETURN_RVA + 0x10u);
        s_cpu.ecx = (uint32_t)(uintptr_t)&s_object;
        body(&s_cpu);
        trace("nested returned\n");
    }
}

static void rec_glDisableVertexAttribArray(guest_gl_uint index)
{
    ++s_backend_calls;
    trace("be glDisableVertexAttribArray(%08x)\n", index);
}

static void rec_glVertexAttribPointer(guest_gl_uint index, guest_gl_int size,
                                      guest_gl_enum type,
                                      guest_gl_boolean normalized,
                                      guest_gl_sizei stride,
                                      guest_gl_addr pointer)
{
    ++s_backend_calls;
    trace("be glVertexAttribPointer(%08x,%d,%08x,%u,%d,%08x)\n", index, size,
          type, normalized, stride, pointer);
}

static void rec_glCullFace(guest_gl_enum mode)
{
    ++s_backend_calls;
    trace("be glCullFace(%08x)\n", mode);
}

static void install_recording_backend(int with_pointer)
{
    guest_gl_backend backend;

    memset(&backend, 0, sizeof backend);
    backend.glGetAttribLocation = rec_glGetAttribLocation;
    backend.glEnableVertexAttribArray = rec_glEnableVertexAttribArray;
    backend.glDisableVertexAttribArray = rec_glDisableVertexAttribArray;
    if (with_pointer)
        backend.glVertexAttribPointer = rec_glVertexAttribPointer;
    backend.glCullFace = rec_glCullFace;
    guest_gl_install_backend(&backend);
}

/* ---- image words at their real addresses ------------------------------ */

#define ORACLE_TABLE_PAGE UINT32_C(0x98567000)
#define ORACLE_TABLE_BYTES UINT32_C(0x1000)
#define ORACLE_SLOT_PAGE UINT32_C(0x987bf000)
#define ORACLE_SLOT_BYTES UINT32_C(0x4000)
#define ORACLE_TABLES_RVA UINT32_C(0x005673ac)
#define ORACLE_PE_BASE UINT32_C(0x00400000)

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
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* argv[1]: the 64 frozen jump-table bytes at RVA 0x5673ac as 128 hex
 * digits, exactly as the PE stores them (image base 0x400000); relocated to
 * GUEST_IMAGE_BASE the way the loader applies the HIGHLOW fixups. */
static int install_tables(const char *hex)
{
    uint32_t i;

    if (strlen(hex) != 128u)
        return 0;
    for (i = 0u; i < 16u; ++i) {
        uint32_t word = 0u;
        uint32_t k;
        for (k = 0u; k < 4u; ++k) {
            int hi = hex_nibble(hex[i * 8u + k * 2u]);
            int lo = hex_nibble(hex[i * 8u + k * 2u + 1u]);
            if (hi < 0 || lo < 0)
                return 0;
            word |= (uint32_t)((hi << 4) | lo) << (k * 8u);
        }
        if (word - ORACLE_PE_BASE > UINT32_C(0x0085f000))
            return 0;
        st32(GUEST_IMAGE_BASE + ORACLE_TABLES_RVA + i * 4u,
             word - ORACLE_PE_BASE + GUEST_IMAGE_BASE);
    }
    return 1;
}

/* ---- one case ---------------------------------------------------------- */

typedef struct attrib_case {
    const char *name;
    int kind;                 /* 0 EnableAttribs, 1 DisableAttribs */
    uint32_t count;
    uint32_t formats[ORACLE_MAX_ATTRIBS];
    const char *names[ORACLE_MAX_ATTRIBS];
    uint32_t program;
    uint32_t base;
    uint32_t stride;
    uint32_t vtable;          /* 0: the frozen Shader vtable */
    int attribs_null;
    int slot_get_bad;         /* 1: zero, 2: near token, 3: glCullFace token */
    int slot_second_bad;      /* second slot (enable/disable), same coding */
    int slot_pointer_bad;     /* glVertexAttribPointer slot, same coding */
    int hostile_imports;
    uint32_t esp_offset;      /* 0: frame at the top; else floor + offset */
    int esp_below_floor;      /* frame word at floor - 4 (ESP below the floor) */
    int nested;
    int foreign;
    int missing_pointer;      /* backend without glVertexAttribPointer */
    int expect_handled;       /* verdicts of the seamed run */
    int expect_rejected;
    int fault_mid_replay;     /* compare trace + fault only */
    int expect_dyn_set;       /* pin the dyn census of both runs */
    unsigned expect_ref_dyn;
    unsigned expect_seam_dyn;
} attrib_case;

typedef struct census {
    unsigned dyn, calls, imports, backend, log, handled, rejected;
} census;

static void census_take(census *out)
{
    out->dyn = g_host_dynamic_calls;
    out->calls = g_guest_phase_profile_counters.guest_calls;
    out->imports = g_host_import_calls;
    out->backend = s_backend_calls;
    out->log = s_log_calls;
    out->handled = s_handled;
    out->rejected = s_rejected;
}

static void census_delta(const census *before, census *after)
{
    after->dyn -= before->dyn;
    after->calls -= before->calls;
    after->imports -= before->imports;
    after->backend -= before->backend;
    after->log -= before->log;
    after->handled -= before->handled;
    after->rejected -= before->rejected;
}

static void oracle_entry(CPU *__restrict c)
{
    s_body(c);
}

/* A translated run that faults inside a GL adapter (a checked stack access)
 * leaves gl_bridge's owner word holding the CPU: fatal in production, so
 * nothing there ever resets it.  Between oracle runs an unregistered-token
 * dispatch under its own run scope normalises the word (owner -> FAULTED
 * sentinel with the established nested fault, FAULTED -> IDLE, IDLE -> IDLE)
 * so every case starts from the same state. */
static void scrub_entry(CPU *__restrict c)
{
    (void)guest_gl_dispatch(c, UINT32_C(0x7e000001));
}

static void scrub_owner_word(CPU *c)
{
    (void)guest_run_until_stop(c, scrub_entry);
    (void)guest_run_until_stop(c, scrub_entry);
    c->fault = NULL;
    c->fault_addr = 0u;
}

static uint32_t slot_value(uint32_t good, int bad)
{
    switch (bad) {
    case 1: return 0u;
    case 2: return good + 1u;
    case 3: return guest_gl_resolve("glCullFace");
    default: return good;
    }
}

static void prepare(const attrib_case *k, CPU *c, uint32_t *storage,
                    uint32_t *entry_esp_out)
{
    uint32_t floor = (uint32_t)(uintptr_t)&storage[ORACLE_STACK_GUARD_WORDS];
    uint32_t ceiling = (uint32_t)(uintptr_t)
        &storage[ORACLE_STACK_GUARD_WORDS + ORACLE_STACK_WORDS];
    uint32_t esp = k->esp_offset ? floor + k->esp_offset : ceiling - 16u;
    uint32_t word;
    uint32_t i;
    uint32_t t_get = guest_gl_resolve("glGetAttribLocation");
    uint32_t t_enable = guest_gl_resolve("glEnableVertexAttribArray");
    uint32_t t_pointer = guest_gl_resolve("glVertexAttribPointer");
    uint32_t t_disable = guest_gl_resolve("glDisableVertexAttribArray");

    if (k->esp_below_floor)
        esp = floor - 4u;
    memset(storage, 0xcd,
           (ORACLE_STACK_GUARD_WORDS + ORACLE_STACK_WORDS) * sizeof storage[0]);
    memset(&s_object, 0, sizeof s_object);
    s_object.vtable = k->vtable ? k->vtable : ATTRIB_VTABLE;
    s_object.pad04 = 0x0badf00du;
    s_object.attribs = k->attribs_null ? 0u : (uint32_t)(uintptr_t)s_pairs;
    s_object.count = k->count;
    s_object.pad10 = 0x10101010u;
    s_object.pad24 = 0x24242424u;
    s_object.program = k->program;
    s_object.pad2c = 0x2c2c2c2cu;
    for (i = 0u; i < ORACLE_MAX_ATTRIBS; ++i) {
        s_pairs[i].name = (uint32_t)(uintptr_t)k->names[i];
        s_pairs[i].format = k->formats[i];
    }
    /* The four function-pointer slot words as wglGetProcAddress filled them
     * (or as a hostile case corrupts them). */
    st32(ATTRIB_SLOT_GET, slot_value(t_get, k->slot_get_bad));
    st32(ATTRIB_SLOT_ENABLE, slot_value(t_enable, k->slot_second_bad));
    st32(ATTRIB_SLOT_POINTER, slot_value(t_pointer, k->slot_pointer_bad));
    st32(ATTRIB_SLOT_DISABLE, slot_value(t_disable, k->slot_second_bad));

    c->eax = 0x11110000u;
    c->ecx = (uint32_t)(uintptr_t)&s_object;
    c->edx = 0x33330000u;
    c->ebx = 0x44440000u;
    c->esp = esp;
    c->ebp = 0x55550000u;
    c->esi = 0x66660000u;
    c->edi = 0x77770000u;
    c->fault = NULL;
    c->fault_addr = 0u;
    c->exit_api = NULL;
    c->exit_code = 0;
    c->stop_kind = 0;
    /* A frame that straddles the ceiling is only written inside the bound
     * stack (the storage past the ceiling belongs to other statics). */
    for (word = 0u; word < 4u; ++word) {
        uint32_t value = word == 0u ? ORACLE_RETURN_RVA :
                         word == 1u ? k->base :
                         word == 2u ? ORACLE_UNUSED_ARG : k->stride;
        if (esp + word * 4u + 4u <= ceiling)
            st32(esp + word * 4u, value);
    }
    *entry_esp_out = esp;
}

static void describe(char *out, size_t cap, const CPU *c, uint32_t entry_esp,
                     int stop, int registers)
{
    size_t n = 0u;
    const unsigned char *bytes;
    size_t i;

    if (registers) {
        n += (size_t)snprintf(out + n, cap - n,
            "eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%+d ebp=%08x esi=%08x "
            "edi=%08x stop=%d ",
            c->eax, c->ecx, c->edx, c->ebx,
            (int)(c->esp - entry_esp), c->ebp, c->esi, c->edi, stop);
    }
    n += (size_t)snprintf(out + n, cap - n, "fault=%s addr=%08x ",
                          c->fault ? c->fault : "-", c->fault_addr);
    if (registers) {
        n += (size_t)snprintf(out + n, cap - n, "frame=");
        for (i = 0u; i < 4u; ++i) {
            uint32_t at = entry_esp + (uint32_t)i * 4u;
            if (at + 4u <= c->stack_ceiling)
                n += (size_t)snprintf(out + n, cap - n, "%08x,", ld32(at));
            else
                n += (size_t)snprintf(out + n, cap - n, "<above>,");
        }
        n += (size_t)snprintf(out + n, cap - n, " obj=");
        bytes = (const unsigned char *)&s_object;
        for (i = 0u; i < sizeof s_object && n + 3u < cap; ++i)
            n += (size_t)snprintf(out + n, cap - n, "%02x", bytes[i]);
        n += (size_t)snprintf(out + n, cap - n, " pairs=");
        bytes = (const unsigned char *)s_pairs;
        for (i = 0u; i < sizeof s_pairs && n + 3u < cap; ++i)
            n += (size_t)snprintf(out + n, cap - n, "%02x", bytes[i]);
    }
}

static int run_variant(const attrib_case *k, guest_fn body, guest_fn nested,
                       char *trace_out, size_t trace_cap, char *state_out,
                       size_t state_cap, census *delta)
{
    CPU *c = k->foreign ? &s_foreign : &s_cpu;
    uint32_t *storage = k->foreign ? s_foreign_storage : s_stack_storage;
    uint32_t entry_esp;
    census before;
    int stop;

    prepare(k, c, storage, &entry_esp);
    install_recording_backend(!k->missing_pointer);
    s_nested_body = k->nested ? nested : NULL;
    s_nested_base = k->base;
    s_nested_stride = k->stride;
    trace_reset();
    census_take(&before);
    s_body = body;
    stop = guest_run_until_stop(c, oracle_entry);
    census_take(delta);
    census_delta(&before, delta);
    s_nested_body = NULL;
    describe(state_out, state_cap, c, entry_esp, stop, !k->fault_mid_replay);
    scrub_owner_word(c);
    if (s_trace_overflow || s_trace_len + 1u > trace_cap)
        return 0;
    memcpy(trace_out, s_trace, s_trace_len + 1u);
    return 1;
}

static char s_ref_trace[sizeof s_trace];
static char s_seam_trace[sizeof s_trace];
static char s_ref_state[4096];
static char s_seam_state[4096];

static unsigned s_cases;
static unsigned s_failures;
static unsigned s_total_handled;
static unsigned s_total_rejected;
static unsigned s_total_backend;

static void fail(const char *name, const char *tag, const char *why)
{
    ++s_failures;
    printf("FAIL %s [%s]: %s\n", name, tag, why);
}

static void compare(const attrib_case *k, const char *tag, guest_fn ref,
                    guest_fn seam)
{
    census ref_census, seam_census;
    unsigned crossings;

    if (!run_variant(k, ref, ref, s_ref_trace, sizeof s_ref_trace,
                     s_ref_state, sizeof s_ref_state, &ref_census)) {
        fail(k->name, tag, "reference trace overflow");
        return;
    }
    if (ref_census.handled || ref_census.rejected) {
        fail(k->name, tag, "reference body reached the fast path");
        return;
    }
    if (!run_variant(k, seam, seam, s_seam_trace, sizeof s_seam_trace,
                     s_seam_state, sizeof s_seam_state, &seam_census)) {
        fail(k->name, tag, "seamed trace overflow");
        return;
    }
    ++s_cases;
    s_total_handled += seam_census.handled;
    s_total_rejected += seam_census.rejected;
    s_total_backend += ref_census.backend;
    printf("case %-26s %-4s verdict=%u/%u be=%u dyn=%u,%u calls=%u,%u "
           "imp=%u,%u log=%u\n",
           k->name, tag, seam_census.handled, seam_census.rejected,
           ref_census.backend, ref_census.dyn, seam_census.dyn,
           ref_census.calls, seam_census.calls, ref_census.imports,
           seam_census.imports, ref_census.log);
    {
        /* The verdict trace lines exist only in the seamed run; strip them
         * before comparing the backend streams. */
        char *p = s_seam_trace;
        char *out = s_seam_trace;
        while (*p) {
            char *end = strchr(p, '\n');
            size_t len = end ? (size_t)(end - p) + 1u : strlen(p);
            if (strncmp(p, "fastpath ", 9) != 0) {
                memmove(out, p, len);
                out += len;
            }
            p += len;
        }
        *out = '\0';
    }
    if (strcmp(s_ref_trace, s_seam_trace) != 0) {
        fail(k->name, tag, "backend call stream differs");
        printf("--- reference\n%s--- seamed\n%s", s_ref_trace, s_seam_trace);
    }
    if (strcmp(s_ref_state, s_seam_state) != 0) {
        fail(k->name, tag, "CPU/frame/object state differs");
        printf("--- reference\n%s\n--- seamed\n%s\n", s_ref_state,
               s_seam_state);
    }
    if (seam_census.handled != (unsigned)k->expect_handled ||
        seam_census.rejected != (unsigned)k->expect_rejected)
        fail(k->name, tag, "fast-path verdicts differ from the expectation");
    if (seam_census.backend != ref_census.backend ||
        seam_census.log != ref_census.log ||
        seam_census.imports != ref_census.imports)
        fail(k->name, tag, "backend/log/import censuses differ");
    /* Census design: the reference dispatches every GL call through
     * guest_call (one guest_calls and one dyn increment each, unless a
     * hostile import table owns the token); a handled replay skips exactly
     * those increments and nothing else. */
    crossings = ref_census.dyn;
    if (k->expect_dyn_set &&
        (ref_census.dyn != k->expect_ref_dyn ||
         seam_census.dyn != k->expect_seam_dyn))
        fail(k->name, tag, "dyn census differs from the pinned expectation");
    if (k->expect_handled && !k->nested) {
        if (seam_census.dyn != 0u || seam_census.calls != 0u)
            fail(k->name, tag, "handled replay still advanced a census");
        if (!k->hostile_imports && crossings != ref_census.backend)
            fail(k->name, tag, "reference crossings != backend calls");
    } else if (!k->expect_handled && !k->nested) {
        if (seam_census.dyn != ref_census.dyn ||
            seam_census.calls != ref_census.calls)
            fail(k->name, tag, "declined replay changed a census");
    }
}

static void compare_both(const attrib_case *k)
{
    guest_fn ref = k->kind ? ref_sub_005673f0 : ref_sub_005672d0;
    guest_fn seam = k->kind ? sub_005673f0 : sub_005672d0;
    guest_fn mem_ref = k->kind ? mem_ref_sub_005673f0 : mem_ref_sub_005672d0;
    guest_fn mem_seam = k->kind ? mem_sub_005673f0 : mem_sub_005672d0;

    compare(k, "gpr", ref, seam);
    compare(k, "mem", mem_ref, mem_seam);
}

static void case_init(attrib_case *k, const char *name, int kind,
                      uint32_t count)
{
    uint32_t i;

    memset(k, 0, sizeof *k);
    k->name = name;
    k->kind = kind;
    k->count = count;
    k->program = 7u;
    k->base = 0x98800000u;
    k->stride = 48u;
    for (i = 0u; i < ORACLE_MAX_ATTRIBS; ++i) {
        k->formats[i] = (i % 8u) + 1u;
        k->names[i] = s_names[i];
    }
    k->expect_handled = 1;
}

/* Fast-path-only probe for inputs the translated body cannot execute. */
static unsigned s_probes;

static void probe_rejects(const attrib_case *k)
{
    CPU before;
    uint32_t entry_esp;
    int rc;
    census a, b;

    prepare(k, &s_cpu, s_stack_storage, &entry_esp);
    install_recording_backend(1);
    trace_reset();
    memcpy(&before, &s_cpu, sizeof before);
    census_take(&a);
    rc = k->kind ? oracle_attrib_disable_impl(&s_cpu)
                 : oracle_attrib_enable_impl(&s_cpu);
    census_take(&b);
    census_delta(&a, &b);
    ++s_probes;
    printf("probe %-26s rc=%d\n", k->name, rc);
    if (rc != 0 || memcmp(&before, &s_cpu, sizeof before) != 0 ||
        s_trace_len != 0u || b.dyn || b.calls || b.backend)
        fail(k->name, "probe", "fast path did not decline untouched");
}

int main(int argc, char **argv)
{
    static guest_import imports[4];
    static guest_import hostile[5];
    attrib_case k;
    uint32_t i;
    uint32_t stack_floor, stack_ceiling;

    if (argc != 2) {
        fprintf(stderr, "usage: oracle <128 hex digits of the jump tables>\n");
        return 2;
    }
    if (!commit_image_page(ORACLE_TABLE_PAGE, ORACLE_TABLE_BYTES) ||
        !commit_image_page(ORACLE_SLOT_PAGE, ORACLE_SLOT_BYTES)) {
        fprintf(stderr, "could not commit the image words at their "
                        "addresses (build with /LARGEADDRESSAWARE)\n");
        return 2;
    }
    if (!install_tables(argv[1])) {
        fprintf(stderr, "bad jump-table argument\n");
        return 2;
    }
    if (!guest_gl_resolve("glGetAttribLocation") ||
        !guest_gl_resolve("glEnableVertexAttribArray") ||
        !guest_gl_resolve("glVertexAttribPointer") ||
        !guest_gl_resolve("glDisableVertexAttribArray") ||
        !guest_gl_resolve("glCullFace")) {
        fprintf(stderr, "registry drifted\n");
        return 2;
    }

    guest_cpu_init(&s_cpu);
    guest_cpu_init(&s_foreign);
    stack_floor = (uint32_t)(uintptr_t)&s_stack_storage[ORACLE_STACK_GUARD_WORDS];
    stack_ceiling = (uint32_t)(uintptr_t)
        &s_stack_storage[ORACLE_STACK_GUARD_WORDS + ORACLE_STACK_WORDS];
    if (guest_stack_bind(&s_cpu, stack_floor, stack_ceiling) ||
        guest_stack_bind(&s_foreign,
            (uint32_t)(uintptr_t)&s_foreign_storage[ORACLE_STACK_GUARD_WORDS],
            (uint32_t)(uintptr_t)&s_foreign_storage[
                ORACLE_STACK_GUARD_WORDS + ORACLE_STACK_WORDS])) {
        fprintf(stderr, "stack bind failed\n");
        return 2;
    }
    /* Production-shaped import table (slot RVAs far below the token
     * families): g_gl_dynamic_first holds. */
    imports[0].slot_rva = 0x00606104u; imports[0].name = "KERNEL32.dll!FreeLibrary";
    imports[1].slot_rva = 0x00606118u; imports[1].name = "KERNEL32.dll!GetProcAddress";
    imports[2].slot_rva = 0x00606124u; imports[2].name = "KERNEL32.dll!LoadLibraryA";
    imports[3].slot_rva = 0x006064c0u; imports[3].name = "WINMM.dll!timeGetTime";
    guest_register_imports(imports, 4u);
    if (!guest_gl_dynamic_first_holds()) {
        fprintf(stderr, "0x7e-first startup fact does not hold\n");
        return 2;
    }

    printf("=== enable: counts 0..8, formats cycling 1..8 ===\n");
    for (i = 0u; i <= 8u; ++i) {
        static char names[9][16];
        snprintf(names[i], sizeof names[i], "enable-count-%u", i);
        case_init(&k, names[i], 0, i);
        compare_both(&k);
    }
    printf("=== enable: each format alone (count 3) ===\n");
    for (i = 1u; i <= 8u; ++i) {
        static char names[9][16];
        uint32_t j;
        snprintf(names[i], sizeof names[i], "enable-format-%u", i);
        case_init(&k, names[i], 0, 3u);
        for (j = 0u; j < ORACLE_MAX_ATTRIBS; ++j)
            k.formats[j] = i;
        compare_both(&k);
    }
    printf("=== enable: locations of -1, argument variety ===\n");
    case_init(&k, "enable-missing-first", 0, 5u);
    k.names[0] = s_missing_names[0];
    compare_both(&k);
    case_init(&k, "enable-missing-mixed", 0, 7u);
    k.names[2] = s_missing_names[1];
    k.names[6] = s_missing_names[2];
    compare_both(&k);
    case_init(&k, "enable-all-missing", 0, 3u);
    k.names[0] = s_missing_names[0];
    k.names[1] = s_missing_names[1];
    k.names[2] = s_missing_names[2];
    compare_both(&k);
    case_init(&k, "enable-stride0-base0", 0, 4u);
    k.stride = 0u;
    k.base = 0u;
    compare_both(&k);
    case_init(&k, "enable-negative-stride", 0, 4u);
    k.stride = 0xfffffff0u;
    k.base = 0xfffffff8u;
    k.program = 0x7fffffffu;
    compare_both(&k);
    case_init(&k, "enable-count-16", 0, 16u);
    compare_both(&k);
    case_init(&k, "enable-game-shape", 0, 7u);
    k.formats[0] = 4u; k.formats[1] = 2u; k.formats[2] = 4u; k.formats[3] = 4u;
    k.formats[4] = 4u; k.formats[5] = 2u; k.formats[6] = 1u;
    k.stride = 92u;
    compare_both(&k);

    printf("=== enable: hostile inputs decline to the translated body ===\n");
    case_init(&k, "enable-count-17", 0, 17u);
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    case_init(&k, "enable-foreign-vtable", 0, 3u);
    k.vtable = ATTRIB_VTABLE + 4u;
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    case_init(&k, "enable-vtable-zero", 0, 3u);
    k.vtable = 1u;   /* prepare() maps 0 to the frozen vtable */
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    case_init(&k, "enable-program-zero", 0, 3u);
    k.program = 0u;
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    case_init(&k, "enable-null-name", 0, 4u);
    k.names[2] = NULL;
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    case_init(&k, "enable-format-0", 0, 4u);
    k.formats[1] = 0u;
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    case_init(&k, "enable-format-9", 0, 4u);
    k.formats[3] = 9u;
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    case_init(&k, "enable-format-max", 0, 2u);
    k.formats[0] = 0xffffffffu;
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    case_init(&k, "enable-slot-get-zero", 0, 2u);
    k.slot_get_bad = 1;
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    case_init(&k, "enable-slot-get-near", 0, 2u);
    k.slot_get_bad = 2;
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    case_init(&k, "enable-slot-get-cull", 0, 1u);
    k.slot_get_bad = 3;
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    case_init(&k, "enable-slot-enable-zero", 0, 2u);
    k.slot_second_bad = 1;
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    case_init(&k, "enable-no-pointer-backend", 0, 2u);
    k.missing_pointer = 1;
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    case_init(&k, "enable-stack-43", 0, 3u);
    k.esp_offset = 43u * 0u + 40u;   /* below the 44-byte translated depth */
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    case_init(&k, "enable-stack-44", 0, 3u);
    k.esp_offset = 44u;              /* exactly the translated depth */
    compare_both(&k);
    /* Reviewer cases (wf/opt-attrib review): the third slot word is the one
     * the original oracle never corrupted; a decline must happen before the
     * first wrapper runs, so the reference's two dispatched crossings and
     * its fault on the zero slot are reproduced exactly by the seamed body. */
    case_init(&k, "enable-slot-pointer-zero", 0, 2u);
    k.slot_pointer_bad = 1;
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    case_init(&k, "enable-slot-pointer-near", 0, 2u);
    k.slot_pointer_bad = 2;
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    /* A registered but foreign token in the pointer slot: the translated
     * body runs glCullFace against the six-word frame (stdcall pops one
     * word; the epilogue then pops argument words) - the replay must not
     * "repair" that, it must decline and let the body do exactly that. */
    case_init(&k, "enable-slot-pointer-cull", 0, 1u);
    k.slot_pointer_bad = 3;
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    case_init(&k, "enable-slot-enable-cull", 0, 2u);
    k.slot_second_bad = 3;
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    /* Frame straddling the stack ceiling: the raw translated `ret 0xc`
     * leaves ESP above the ceiling; the replay's frame check declines. */
    case_init(&k, "enable-frame-above-ceiling", 0, 0u);
    k.esp_offset = ORACLE_STACK_WORDS * 4u - 12u;
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    /* ESP below the floor: the reference's first checked adapter read
     * raises the stack fault before any wrapper runs; the replay's frame
     * check (wrapped offset) declines without reading the caller's words. */
    case_init(&k, "enable-esp-below-floor", 0, 1u);
    k.esp_below_floor = 1;
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    /* Re-entry from the glEnableVertexAttribArray callback.  Reference: two
     * dispatched crossings, then the nested body's first guest_call raises
     * the nested fault (dyn 3).  Seamed: the outer replay issues both
     * wrappers without guest_call, the nested replay declines (owner word
     * held by the outer one) and the nested translated body raises the same
     * fault on its first guest_call (dyn 1).  The fault unwinds through the
     * outer replay before its verdict is counted, so only the nested
     * rejection is visible; the dyn census pins the rest. */
    case_init(&k, "enable-nested-callback", 0, 2u);
    k.nested = 1;
    k.expect_handled = 0; k.expect_rejected = 1;
    k.fault_mid_replay = 1;
    k.expect_dyn_set = 1; k.expect_ref_dyn = 3u; k.expect_seam_dyn = 1u;
    compare_both(&k);
    case_init(&k, "enable-hostile-imports", 0, 2u);
    k.hostile_imports = 1;
    k.expect_handled = 0; k.expect_rejected = 1;
    memcpy(hostile, imports, sizeof imports);
    hostile[4].slot_rva = guest_gl_resolve("glGetAttribLocation");
    hostile[4].name = "HOSTILE.dll!ShadowsGetAttribLocation";
    guest_register_imports(hostile, 5u);
    if (guest_gl_dynamic_first_holds()) {
        fprintf(stderr, "hostile import table left the startup fact\n");
        return 2;
    }
    compare_both(&k);
    guest_register_imports(imports, 4u);
    if (!guest_gl_dynamic_first_holds())
        return 2;
    case_init(&k, "enable-after-restore", 0, 2u);
    compare_both(&k);

    printf("=== disable: counts 0..8, -1 locations, hostile inputs ===\n");
    for (i = 0u; i <= 8u; ++i) {
        static char names[9][18];
        snprintf(names[i], sizeof names[i], "disable-count-%u", i);
        case_init(&k, names[i], 1, i);
        compare_both(&k);
    }
    case_init(&k, "disable-missing-mixed", 1, 7u);
    k.names[0] = s_missing_names[0];
    k.names[5] = s_missing_names[2];
    compare_both(&k);
    case_init(&k, "disable-count-16", 1, 16u);
    compare_both(&k);
    case_init(&k, "disable-formats-ignored", 1, 4u);
    k.formats[0] = 0u; k.formats[1] = 99u; k.formats[2] = 0xffffffffu;
    compare_both(&k);
    case_init(&k, "disable-count-17", 1, 17u);
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    case_init(&k, "disable-foreign-vtable", 1, 2u);
    k.vtable = ATTRIB_VTABLE - 0x14u;
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    case_init(&k, "disable-program-zero", 1, 2u);
    k.program = 0u;
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    case_init(&k, "disable-null-name", 1, 3u);
    k.names[0] = NULL;
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    case_init(&k, "disable-slot-get-zero", 1, 2u);
    k.slot_get_bad = 1;
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    case_init(&k, "disable-slot-disable-zero", 1, 2u);
    k.slot_second_bad = 1;
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    case_init(&k, "disable-stack-16", 1, 3u);
    k.esp_offset = 16u;              /* below the 20-byte translated depth */
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    case_init(&k, "disable-stack-20", 1, 3u);
    k.esp_offset = 20u;
    compare_both(&k);
    case_init(&k, "disable-slot-disable-cull", 1, 2u);
    k.slot_second_bad = 3;
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    case_init(&k, "disable-slot-get-cull", 1, 2u);
    k.slot_get_bad = 3;
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    case_init(&k, "disable-frame-above-ceiling", 1, 0u);
    k.esp_offset = ORACLE_STACK_WORDS * 4u - 8u;
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    case_init(&k, "disable-esp-below-floor", 1, 2u);
    k.esp_below_floor = 1;
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);

    printf("=== foreign CPU after the owner pin ===\n");
    case_init(&k, "enable-foreign-cpu", 0, 2u);
    k.foreign = 1;
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    case_init(&k, "disable-foreign-cpu", 1, 2u);
    k.foreign = 1;
    k.expect_handled = 0; k.expect_rejected = 1;
    compare_both(&k);
    case_init(&k, "enable-owner-after-foreign", 0, 3u);
    compare_both(&k);

    printf("=== fast-path-only probes (translated body cannot run these) ===\n");
    case_init(&k, "probe-enable-count-max", 0, 0xffffffffu);
    probe_rejects(&k);
    case_init(&k, "probe-disable-count-max", 1, 0xffffffffu);
    probe_rejects(&k);
    case_init(&k, "probe-enable-null-attribs", 0, 3u);
    k.attribs_null = 1;
    probe_rejects(&k);
    case_init(&k, "probe-disable-null-attribs", 1, 3u);
    k.attribs_null = 1;
    probe_rejects(&k);
    case_init(&k, "probe-enable-unbound-stack", 0, 3u);
    {
        CPU unbound;
        CPU snapshot;
        uint32_t entry_esp;
        guest_cpu_init(&unbound);
        prepare(&k, &s_cpu, s_stack_storage, &entry_esp);
        unbound.ecx = s_cpu.ecx;
        unbound.esp = s_cpu.esp;
        memcpy(&snapshot, &unbound, sizeof snapshot);
        ++s_probes;
        printf("probe %-26s rc=%d\n", k.name,
               oracle_attrib_enable_impl(&unbound));
        if (memcmp(&snapshot, &unbound, sizeof snapshot) != 0)
            fail(k.name, "probe", "unbound CPU was modified");
    }

    if (s_failures) {
        printf("Vita shader attrib fast path oracle: FAIL; failures=%u\n",
               s_failures);
        return 1;
    }
    printf("Vita shader attrib fast path oracle: PASS; cases=%u; "
           "handled=%u; rejected=%u; backend=%u; probes=%u\n",
           s_cases, s_total_handled, s_total_rejected, s_total_backend,
           s_probes);
    return 0;
}
