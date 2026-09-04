/* Differential host oracle for ISAAC_VITA_GL_SHIM_FASTDISPATCH.
 *
 * recomp/test_gl_shim_fastdispatch.py builds this file twice with the real
 * guest.c (included, as vita_sync_import_fastpath_oracle.c does), gl_bridge.c
 * and host_vita_gl.c: once without the option (the legacy path: cache probe,
 * IAT search, guest_host_dynamic, linear token scan, compare/exchange owner
 * word, 73-case switch) and once with it.  Every line printed before the
 * "=== build-specific ===" marker must be byte-identical between the two
 * binaries: the guest CPU after each call, the stdcall cleanup, EAX, the
 * fault text, the dynamic/import/phase-profile censuses and the recording
 * backend's call stream, plus direct guest_gl_dispatch calls (the PC
 * callers that bypass guest_call) with known and unknown tokens.  The section
 * after the marker holds the exhaustive membership sweep and the fail-closed
 * foreign-CPU trap that only the fast build has, including the proof that an
 * unknown token from a foreign CPU neither dispatches nor moves the pin.
 *
 * Executed on a 32-bit identity-mapped host (MSVC x86); guest addresses are
 * host pointers to static storage in this file. */
#include "guest.c"

#include "gl_bridge.h"
#include "host_vita_gl.h"
#include "host_vita_import_id.h"

#if UINTPTR_MAX != UINT32_MAX
# error gl_shim_fastdispatch_oracle requires a 32-bit identity-mapped host
#endif
#if !defined(ISAAC_VITA_IMPORT_ID_DISPATCH) || \
    !defined(ISAAC_VITA_GUEST_LOOKUP_CACHE) || \
    !defined(ISAAC_VITA_PHASE_PROFILE) || GUEST_STACK_REQUIRED != 1
# error build this oracle with the production guest.c configuration
#endif

/* The dispatch-table legs (ISAAC_VITA_GUEST_DISPATCH_TABLE, with and without
 * ISAAC_VITA_GL_SHIM_TABLE_TOKENS) count guest_call entries in
 * dispatch_calls/dispatch_slow/dispatch_gl instead of guest_calls; print the
 * derived g(c)/g(l) the profiler prints (kage_vita_phase_profile.c) so every
 * leg's trace spells the same census. */
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
# if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
#  define ORACLE_PP_GL (g_guest_phase_profile_counters.dispatch_gl)
# else
#  define ORACLE_PP_GL 0u
# endif
# define ORACLE_PP_CALLS (g_guest_phase_profile_counters.guest_calls + \
                          g_guest_phase_profile_counters.dispatch_calls)
# define ORACLE_PP_LOOKUPS (g_guest_phase_profile_counters.guest_lookups + \
                            g_guest_phase_profile_counters.dispatch_calls - \
                            g_guest_phase_profile_counters.dispatch_slow - \
                            ORACLE_PP_GL)
#else
# define ORACLE_PP_CALLS (g_guest_phase_profile_counters.guest_calls)
# define ORACLE_PP_LOOKUPS (g_guest_phase_profile_counters.guest_lookups)
#endif

/* gl_bridge.c, ISAAC_GL_SHIM_FASTDISPATCH_ORACLE only. */
void guest_gl_oracle_reset_active(void);
#if defined(ISAAC_VITA_GL_SHIM_RAW_ARGS)
extern unsigned g_guest_gl_oracle_raw_frames;
#endif

/* ---- stubs owned by other Vita runtime units ------------------------ */

unsigned g_host_import_calls;
unsigned g_host_dynamic_calls;
static unsigned s_generic_import_calls;
static unsigned s_translated_calls;

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
    /* stdcall with no arguments: consume the return word only. */
    (void)gpop(c);
    return 1;
}

const char *guest_host_import_id_error(void)
{
    return "oracle generic import-ID error";
}

/* Exact mirror of host_vita_first_fault.c guest_host_dynamic without the
 * XInput delegate (ISAAC_VITA_XINPUT owns only 0x7d2xxxxx tokens and never
 * claims the 0x7e family, by its static assertions). */
int guest_host_dynamic(CPU *__restrict c, uint32_t token)
{
    uint32_t family = token & UINT32_C(0xff000000);

    if (family != UINT32_C(0x7d000000) &&
        family != UINT32_C(0x7e000000))
        return 0;
    return isaac_vita_gl_dynamic_counted(c, token, &g_host_dynamic_calls);
}

/* ---- oracle state ---------------------------------------------------- */

#define ORACLE_STACK_WORDS 256u
#define ORACLE_RETURN_RVA  UINT32_C(0x0056734f)

static uint32_t s_stack[ORACLE_STACK_WORDS];
static uint32_t s_stack_ceiling;
static uint32_t s_stack_floor;
static uint32_t s_pending_token;
static uint32_t s_pending_args[9];
static uint32_t s_pending_count;
static CPU *s_cpu;
static unsigned s_backend_calls;
static uint32_t s_nested_token;

/* ---- phase 10: hostile frames and hostile backends ---------------------
 * The recording glCullFace / glViewport callbacks perform one queued action
 * on the dispatching CPU while the adapter is between its argument decode
 * and its stdcall retirement: move esp (two pushes left on the stack, esp
 * parked at the ceiling or one word under it) or rebind the stack (a tight
 * binding that still holds the frame; a binding that excludes the return
 * word).  Production backends (vitaGL wrappers) never see the CPU; these
 * are the hostile shapes the retirement of every build must agree on. */
enum {
    HOSTILE_NONE = 0,
    HOSTILE_PUSH2,
    HOSTILE_ESP_CEILING,
    HOSTILE_ESP_CEILING_M4,
    HOSTILE_REBIND_TIGHT,
    HOSTILE_REBIND_EXCLUDE_RETURN
};
static int s_hostile_action;
static uint32_t s_hostile_words;
static uint32_t s_hostile_host_words[8];
static void rebind(CPU *c, uint32_t floor, uint32_t ceiling);

static void hostile_backend_action(void)
{
    CPU *c = s_cpu;
    uint32_t esp = c->esp;
    int action = s_hostile_action;

    s_hostile_action = HOSTILE_NONE;
    switch (action) {
    case HOSTILE_PUSH2:
        gpush(c, 0x11u);
        gpush(c, 0x22u);
        break;
    case HOSTILE_ESP_CEILING:
        c->esp = c->stack_ceiling;
        break;
    case HOSTILE_ESP_CEILING_M4:
        c->esp = c->stack_ceiling - 4u;
        break;
    case HOSTILE_REBIND_TIGHT:
        /* guest_stack_bind parks esp at the new ceiling; the frame stays
         * where the adapter left it. */
        rebind(c, esp, esp + 4u + 4u * s_hostile_words);
        c->esp = esp;
        break;
    case HOSTILE_REBIND_EXCLUDE_RETURN:
        rebind(c, esp + 4u, c->stack_ceiling);
        c->esp = esp;
        break;
    default:
        return;
    }
    printf("  be hostile action %d esp=%+d floor=%+d ceiling=%+d\n", action,
           (int)(c->esp - esp), (int)(c->stack_floor - esp),
           (int)(c->stack_ceiling - esp));
}

static uint32_t rel(uint32_t address)
{
    return s_stack_ceiling - address;
}

static const char *token_name(uint32_t token)
{
    size_t count;
    size_t i;
    const guest_gl_symbol *symbols = guest_gl_symbols(&count);

    for (i = 0u; i < count; ++i)
        if (symbols[i].token == token)
            return symbols[i].name;
    return "-";
}

/* ---- recording backend ------------------------------------------------ */

static uint32_t rec_return_rva(void)
{
    return guest_gl_backend_return_rva();
}

static void rec_glActiveTexture(guest_gl_enum texture)
{
    ++s_backend_calls;
    printf("  be glActiveTexture(%08x)\n", texture);
}
static void rec_glBindFramebuffer(guest_gl_enum target, guest_gl_uint fb)
{
    ++s_backend_calls;
    printf("  be glBindFramebuffer(%08x,%u)\n", target, fb);
}
static void rec_glBindTexture(guest_gl_enum target, guest_gl_uint texture)
{
    ++s_backend_calls;
    printf("  be glBindTexture(%08x,%u)\n", target, texture);
}
static void rec_glBlendFuncSeparate(guest_gl_enum a, guest_gl_enum b,
                                    guest_gl_enum c, guest_gl_enum d)
{
    ++s_backend_calls;
    printf("  be glBlendFuncSeparate(%08x,%08x,%08x,%08x)\n", a, b, c, d);
}
static guest_gl_enum rec_glCheckFramebufferStatus(guest_gl_enum target)
{
    ++s_backend_calls;
    printf("  be glCheckFramebufferStatus(%08x)\n", target);
    return UINT32_C(0x8cd5);
}
static void rec_glClear(guest_gl_bitfield mask)
{
    ++s_backend_calls;
    printf("  be glClear(%08x) rva=%08x\n", mask, rec_return_rva());
    if (s_nested_token) {
        /* Nested dispatch from inside a backend callback: must fault. */
        uint32_t token = s_nested_token;
        s_nested_token = 0u;
        gpush(s_cpu, 0x1u);
        gpush(s_cpu, ORACLE_RETURN_RVA + 0x10u);
        guest_call(s_cpu, token);
        printf("  be nested dispatch returned\n");
    }
}
static void rec_glClearColor(guest_gl_float r, guest_gl_float g,
                             guest_gl_float b, guest_gl_float a)
{
    ++s_backend_calls;
    printf("  be glClearColor(%.3f,%.3f,%.3f,%.3f)\n", r, g, b, a);
}
static void rec_glClearDepth(guest_gl_double depth)
{
    ++s_backend_calls;
    printf("  be glClearDepth(%.6f)\n", depth);
}
static guest_gl_uint rec_glCreateProgram(void)
{
    ++s_backend_calls;
    printf("  be glCreateProgram()\n");
    return 7u;
}
static void rec_glCullFace(guest_gl_enum mode)
{
    ++s_backend_calls;
    printf("  be glCullFace(%08x)\n", mode);
    hostile_backend_action();
}
static void rec_glDisableVertexAttribArray(guest_gl_uint index)
{
    ++s_backend_calls;
    printf("  be glDisableVertexAttribArray(%u)\n", index);
}
static void rec_glDrawElements(guest_gl_enum mode, guest_gl_sizei count,
                               guest_gl_enum type, guest_gl_addr indices)
{
    ++s_backend_calls;
    printf("  be glDrawElements(%08x,%d,%08x,%s) rva=%08x\n", mode, count,
           type, indices ? "ptr" : "null", rec_return_rva());
}
static void rec_glEnableVertexAttribArray(guest_gl_uint index)
{
    ++s_backend_calls;
    printf("  be glEnableVertexAttribArray(%u)\n", index);
}
static guest_gl_int rec_location(const char *api, guest_gl_uint program,
                                 guest_gl_addr name)
{
    uint32_t hash = program * 33u;
    const char *text = (const char *)(uintptr_t)name;
    const char *p;

    ++s_backend_calls;
    for (p = text; *p; ++p)
        hash = hash * 31u + (unsigned char)*p;
    printf("  be %s(%u,\"%s\")\n", api, program, text);
    if (text[0] == 'x')
        return -1;
    return (guest_gl_int)(hash % 13u);
}
static guest_gl_int rec_glGetAttribLocation(guest_gl_uint program,
                                            guest_gl_addr name)
{
    return rec_location("glGetAttribLocation", program, name);
}
static guest_gl_int rec_glGetUniformLocation(guest_gl_uint program,
                                             guest_gl_addr name)
{
    return rec_location("glGetUniformLocation", program, name);
}
static guest_gl_addr rec_glGetString(guest_gl_enum name)
{
    ++s_backend_calls;
    printf("  be glGetString(%08x)\n", name);
    return name == 0x1f02u ? UINT32_C(0x98760000) : 0u;
}
static void rec_glUniform1i(guest_gl_int location, guest_gl_int v0)
{
    ++s_backend_calls;
    printf("  be glUniform1i(%d,%d)\n", location, v0);
}
static void rec_glUniformMatrix4fv(guest_gl_int location, guest_gl_sizei count,
                                   guest_gl_boolean transpose,
                                   guest_gl_addr value)
{
    ++s_backend_calls;
    printf("  be glUniformMatrix4fv(%d,%d,%u,%s)\n", location, count,
           transpose, value ? "ptr" : "null");
}
static void rec_glUseProgram(guest_gl_uint program)
{
    ++s_backend_calls;
    printf("  be glUseProgram(%u)\n", program);
}
static void rec_glVertexAttribPointer(guest_gl_uint index, guest_gl_int size,
                                      guest_gl_enum type,
                                      guest_gl_boolean normalized,
                                      guest_gl_sizei stride,
                                      guest_gl_addr pointer)
{
    ++s_backend_calls;
    printf("  be glVertexAttribPointer(%u,%d,%08x,%u,%d,%08x)\n", index, size,
           type, normalized, stride, pointer);
}
static void rec_glViewport(guest_gl_int x, guest_gl_int y,
                           guest_gl_sizei w, guest_gl_sizei h)
{
    ++s_backend_calls;
    printf("  be glViewport(%d,%d,%d,%d)\n", x, y, w, h);
    hostile_backend_action();
}

static void install_recording_backend(void)
{
    guest_gl_backend backend;

    memset(&backend, 0, sizeof backend);
    backend.glActiveTexture = rec_glActiveTexture;
    backend.glBindFramebuffer = rec_glBindFramebuffer;
    backend.glBindTexture = rec_glBindTexture;
    backend.glBlendFuncSeparate = rec_glBlendFuncSeparate;
    backend.glCheckFramebufferStatus = rec_glCheckFramebufferStatus;
    backend.glClear = rec_glClear;
    backend.glClearColor = rec_glClearColor;
    backend.glClearDepth = rec_glClearDepth;
    backend.glCreateProgram = rec_glCreateProgram;
    backend.glCullFace = rec_glCullFace;
    backend.glDisableVertexAttribArray = rec_glDisableVertexAttribArray;
    backend.glDrawElements = rec_glDrawElements;
    backend.glEnableVertexAttribArray = rec_glEnableVertexAttribArray;
    backend.glGetAttribLocation = rec_glGetAttribLocation;
    backend.glGetUniformLocation = rec_glGetUniformLocation;
    backend.glGetString = rec_glGetString;
    backend.glUniform1i = rec_glUniform1i;
    backend.glUniformMatrix4fv = rec_glUniformMatrix4fv;
    backend.glUseProgram = rec_glUseProgram;
    backend.glVertexAttribPointer = rec_glVertexAttribPointer;
    backend.glViewport = rec_glViewport;
    guest_gl_install_backend(&backend);
}

/* ---- one guest_call under the production run boundary ---------------- */

static void oracle_entry(CPU *__restrict c)
{
    uint32_t i;

    for (i = s_pending_count; i-- > 0u;)
        gpush(c, s_pending_args[i]);
    gpush(c, ORACLE_RETURN_RVA);
    guest_call(c, s_pending_token);
}

static void call(const char *label, uint32_t token,
                 const uint32_t *args, uint32_t count)
{
    CPU *c = s_cpu;
    uint32_t esp_before;
    int stop;

    c->esp = s_stack_ceiling;
    c->eax = UINT32_C(0xdeadbeef);
    esp_before = c->esp;
    s_pending_token = token;
    s_pending_count = count;
    memcpy(s_pending_args, args, count * sizeof args[0]);
    printf("call %s token=%08x name=%s argc=%u\n", label, token,
           token_name(token), count);
    stop = guest_run_until_stop(c, oracle_entry);
    printf("  stop=%d esp=%+d eax=%08x fault=%s addr=%08x dyn=%u imp=%u "
           "gen=%u tr=%u pp(c,l,i)=%u,%u,%u\n",
           stop, (int)rel(c->esp) - (int)rel(esp_before), c->eax,
           c->fault ? c->fault : "-", c->fault_addr, g_host_dynamic_calls,
           g_host_import_calls, s_generic_import_calls, s_translated_calls,
           ORACLE_PP_CALLS, ORACLE_PP_LOOKUPS,
           g_guest_phase_profile_counters.lookup_iterations);
    /* The pushed frame bytes (return word and arguments) as the callee left
     * them: identical stdcall consumption leaves identical bytes.  Printed
     * relative to what was pushed, because argument words that are host
     * string addresses differ between the two executables. */
    {
        uint32_t words = count + 1u;
        uint32_t i;
        printf("  frame");
        for (i = 0u; i < words; ++i) {
            uint32_t value = s_stack[ORACLE_STACK_WORDS - words + i];
            uint32_t pushed = i == 0u ? ORACLE_RETURN_RVA
                                      : s_pending_args[i - 1u];
            if (value == pushed)
                printf(" =");
            else
                printf(" %08x", value);
        }
        printf("\n");
    }
}

/* ---- one direct gl_bridge dispatch (PC callers bypass guest_call) ---- */

static int s_direct_rc;

static void direct_entry(CPU *__restrict c)
{
    uint32_t i;

    for (i = s_pending_count; i-- > 0u;)
        gpush(c, s_pending_args[i]);
    gpush(c, ORACLE_RETURN_RVA);
    s_direct_rc = guest_gl_dispatch(c, s_pending_token);
}

static void direct(const char *label, CPU *c, uint32_t token,
                   const uint32_t *args, uint32_t count)
{
    unsigned before = s_backend_calls;
    int stop;

    s_cpu = c;
    s_pending_token = token;
    s_pending_count = count;
    memcpy(s_pending_args, args, count * sizeof args[0]);
    s_direct_rc = -1;
    printf("direct %s token=%08x name=%s argc=%u\n", label, token,
           token_name(token), count);
    stop = guest_run_until_stop(c, direct_entry);
    printf("  stop=%d rc=%d fault=%s addr=%08x backend=%+d dyn=%u\n",
           stop, s_direct_rc, c->fault ? c->fault : "-", c->fault_addr,
           (int)(s_backend_calls - before), g_host_dynamic_calls);
}

/* ---- the KAGE per-draw shapes (research-gl-shim-path.md finding 1) ---- */

static const char *const s_attribute_names[7] = {
    "aPosition", "aTexCoord", "aColor", "aColorOffset", "aRenderData",
    "aScale", "xMissingAttribute"
};

static void kage_draw(uint32_t program, uint32_t texture, uint32_t quads,
                      uint32_t token_blend, uint32_t token_use,
                      uint32_t token_attrib, uint32_t token_enable,
                      uint32_t token_pointer, uint32_t token_active,
                      uint32_t token_bind, uint32_t token_uniform,
                      uint32_t token_uniform1i, uint32_t token_matrix,
                      uint32_t token_draw, uint32_t token_disable)
{
    static const char texture_uniform[] = "Texture0";
    static const char matrix_uniform[] = "Transform";
    static float matrix[16];
    static uint16_t indices[6];
    uint32_t args[6];
    uint32_t i;
    uint32_t location;

    /* sub_0056d500: blend + shader bind. */
    args[0] = 0x302u; args[1] = 0x303u; args[2] = 1u; args[3] = 0x303u;
    call("blend", token_blend, args, 4u);
    args[0] = program;
    call("use", token_use, args, 1u);
    /* sub_005672d0: per attribute glGetAttribLocation ->
     * glEnableVertexAttribArray -> glVertexAttribPointer(GL_FLOAT). */
    for (i = 0u; i < 7u; ++i) {
        args[0] = program;
        args[1] = (uint32_t)(uintptr_t)s_attribute_names[i];
        call("attrib", token_attrib, args, 2u);
        location = s_cpu->eax;
        args[0] = location;
        call("enable", token_enable, args, 1u);
        args[0] = location; args[1] = 2u + (i & 1u); args[2] = 0x1406u;
        args[3] = 0u; args[4] = 48u; args[5] = 0x98800000u + i * 8u;
        call("pointer", token_pointer, args, 6u);
    }
    /* sub_00567430: glActiveTexture, glBindTexture, glGetUniformLocation,
     * glUniform1i. */
    args[0] = 0x84c0u;
    call("active", token_active, args, 1u);
    args[0] = 0xde1u; args[1] = texture;
    call("bind", token_bind, args, 2u);
    args[0] = program; args[1] = (uint32_t)(uintptr_t)texture_uniform;
    call("uniform", token_uniform, args, 2u);
    args[0] = s_cpu->eax; args[1] = 0u;
    call("uniform1i", token_uniform1i, args, 2u);
    /* sub_00565240: glGetUniformLocation + glUniformMatrix4fv. */
    args[0] = program; args[1] = (uint32_t)(uintptr_t)matrix_uniform;
    call("uniform", token_uniform, args, 2u);
    args[0] = s_cpu->eax; args[1] = 1u; args[2] = 0u;
    args[3] = (uint32_t)(uintptr_t)matrix;
    call("matrix", token_matrix, args, 4u);
    /* glDrawElements(GL_TRIANGLES, 6 * quads, GL_UNSIGNED_SHORT, ptr). */
    args[0] = 4u; args[1] = 6u * quads; args[2] = 0x1403u;
    args[3] = (uint32_t)(uintptr_t)indices;
    call("draw", token_draw, args, 4u);
    /* sub_005673f0: per attribute glGetAttribLocation ->
     * glDisableVertexAttribArray. */
    for (i = 0u; i < 7u; ++i) {
        args[0] = program;
        args[1] = (uint32_t)(uintptr_t)s_attribute_names[i];
        call("attrib", token_attrib, args, 2u);
        args[0] = s_cpu->eax;
        call("disable", token_disable, args, 1u);
    }
}

/* ---- phase 9: short frames (every argument position of every token) --- */

static void short_entry(CPU *__restrict c)
{
    guest_call(c, s_pending_token);
}

static void rebind(CPU *c, uint32_t floor, uint32_t ceiling)
{
    c->stack_owner = NULL;
    c->stack_floor = 0u;
    c->stack_ceiling = 0u;
    c->stack_low_water = 0u;
    if (guest_stack_bind(c, floor, ceiling)) {
        fprintf(stderr, "rebind failed\n");
        exit(2);
    }
}

/* The frame (return word + `words` argument words) is written into the
 * middle of s_stack; the stack is then bound so that the return word and
 * `inside` argument words lie below the ceiling (inside < words cuts the
 * frame), or, with floor_cut, so that the floor runs through the return
 * word while every argument word is inside.  Printed: the outcome the
 * legacy per-word validators produce -- kind, frame-relative address and
 * size of the stack fault, the fault text, EAX, the ESP delta and the
 * backend/dynamic censuses -- which every build must reproduce
 * (ISAAC_VITA_GL_SHIM_RAW_ARGS validates the whole frame once and falls
 * back to the per-word readers for a rejected frame). */
static void short_frame(const char *label, uint32_t token, uint32_t words,
                        uint32_t inside, int floor_cut)
{
    CPU *c = s_cpu;
    uint32_t base = (uint32_t)(uintptr_t)&s_stack[ORACLE_STACK_WORDS / 2u];
    uint32_t floor = s_stack_floor;
    uint32_t ceiling = base + 4u + 4u * inside;
    unsigned before = s_backend_calls;
    uint32_t i;
    int stop;

    if (floor_cut) {
        floor = base + 2u;
        ceiling = base + 4u + 4u * words;
    }
    st32(base, ORACLE_RETURN_RVA);
    for (i = 0u; i < words; ++i)
        st32(base + 4u + 4u * i, 0x1000u + i);
    guest_gl_oracle_reset_active();
    rebind(c, floor, ceiling);
    c->esp = base;
    c->eax = UINT32_C(0xdeadbeef);
    s_pending_token = token;
    s_pending_count = 0u;
    stop = guest_run_until_stop(c, short_entry);
    printf("short %s token=%08x name=%s words=%u inside=%u floor_cut=%d "
           "stop=%d esp=%+d eax=%08x fault=%s addr=%08x "
           "sk=%u sa=%+d ss=%u spc=%u backend=%+d dyn=%u\n",
           label, token, token_name(token), words, inside, floor_cut, stop,
           (int)(c->esp - base), c->eax, c->fault ? c->fault : "-",
           c->fault_addr, c->stack_fault_kind,
           c->stack_fault_kind ? (int)(c->stack_fault_address - base) : 0,
           c->stack_fault_size, c->stack_fault_pc,
           (int)(s_backend_calls - before), g_host_dynamic_calls);
    rebind(c, s_stack_floor, s_stack_ceiling);
}

static uint32_t symbol_words(uint32_t token)
{
    size_t count;
    size_t i;
    const guest_gl_symbol *symbols = guest_gl_symbols(&count);

    for (i = 0u; i < count; ++i)
        if (symbols[i].token == token)
            return symbols[i].x86_stack_bytes / 4u;
    return 0u;
}

/* ---- phase 10 helpers ------------------------------------------------- */

/* One guest_call of `token` with esp, floor and ceiling chosen freely (esp
 * may lie outside the bound stack, in host statics that look like a valid
 * frame: no build may read them).  Frame words are written only where they
 * lie inside s_stack.  Everything printed is relative to esp, so the trace
 * is layout-independent. */
static void hostile_frame(const char *label, uint32_t token, uint32_t words,
                          uint32_t esp, uint32_t floor, uint32_t ceiling)
{
    CPU *c = s_cpu;
    unsigned before = s_backend_calls;
    uint32_t i;
    int stop;

    if (esp >= s_stack_floor && esp <= s_stack_ceiling &&
        4u + 4u * words <= s_stack_ceiling - esp) {
        st32(esp, ORACLE_RETURN_RVA);
        for (i = 0u; i < words; ++i)
            st32(esp + 4u + 4u * i, 0x2000u + i);
    }
    guest_gl_oracle_reset_active();
    rebind(c, floor, ceiling);
    c->esp = esp;
    c->eax = UINT32_C(0xdeadbeef);
    s_pending_token = token;
    s_pending_count = 0u;
    stop = guest_run_until_stop(c, short_entry);
    printf("hostile %s token=%08x name=%s words=%u floor=%+d ceiling=%+d "
           "stop=%d esp=%+d eax=%08x fault=%s addr=%08x "
           "sk=%u sa=%+d ss=%u backend=%+d dyn=%u\n",
           label, token, token_name(token), words, (int)(floor - esp),
           (int)(ceiling - esp), stop, (int)(c->esp - esp), c->eax,
           c->fault ? c->fault : "-", c->fault_addr, c->stack_fault_kind,
           c->stack_fault_kind ? (int)(c->stack_fault_address - esp) : 0,
           c->stack_fault_size, (int)(s_backend_calls - before),
           g_host_dynamic_calls);
    rebind(c, s_stack_floor, s_stack_ceiling);
}

/* A production-shaped call() whose recording backend performs `action` on
 * the CPU before returning; the stack binding and the ownership word are
 * restored afterwards so the next case starts clean. */
static uint32_t hostile_call(const char *label, uint32_t token,
                             const uint32_t *args, uint32_t count, int action)
{
    uint32_t kind;

    s_hostile_action = action;
    s_hostile_words = count;
    call(label, token, args, count);
    kind = s_cpu->stack_fault_kind;
    printf("  hostile outcome fault=%s sk=%u sa=%+d\n",
           s_cpu->fault ? s_cpu->fault : "-", kind,
           kind ? (int)(s_cpu->stack_fault_address - s_stack_ceiling) : 0);
    s_hostile_action = HOSTILE_NONE;
    guest_gl_oracle_reset_active();
    /* guest_stack_bind clears the stack-fault record: the kind is returned
     * for callers that pin it. */
    rebind(s_cpu, s_stack_floor, s_stack_ceiling);
    return kind;
}

/* ---- translated function and import table used for precedence cases -- */

static void translated_a(CPU *__restrict c)
{
    ++s_translated_calls;
    c->eax = UINT32_C(0x7a000001);
    (void)gpop(c);
}

static void translated_b(CPU *__restrict c)
{
    ++s_translated_calls;
    c->eax = UINT32_C(0x7a000002);
    (void)gpop(c);
}

static const uint32_t s_translated_addrs[2] = { 0x00401000u, 0x7e307ce3u };
static const guest_fn s_translated_fns[2] = { translated_a, translated_b };

static guest_import s_imports[4];
static guest_import s_hostile_imports[5];

static int linear_registered(uint32_t token)
{
    size_t count;
    size_t i;
    const guest_gl_symbol *symbols = guest_gl_symbols(&count);

    for (i = 0u; i < count; ++i)
        if (symbols[i].token == token)
            return 1;
    return 0;
}

int main(void)
{
    static CPU cpu;
    static CPU foreign;
    static uint32_t foreign_stack[64];
    uint32_t args[6];
    size_t count;
    size_t i;
    const guest_gl_symbol *symbols = guest_gl_symbols(&count);
    uint32_t t_blend = guest_gl_resolve("glBlendFuncSeparate");
    uint32_t t_use = guest_gl_resolve("glUseProgram");
    uint32_t t_attrib = guest_gl_resolve("glGetAttribLocation");
    uint32_t t_enable = guest_gl_resolve("glEnableVertexAttribArray");
    uint32_t t_pointer = guest_gl_resolve("glVertexAttribPointer");
    uint32_t t_active = guest_gl_resolve("glActiveTexture");
    uint32_t t_bind = guest_gl_resolve("glBindTexture");
    uint32_t t_uniform = guest_gl_resolve("glGetUniformLocation");
    uint32_t t_uniform1i = guest_gl_resolve("glUniform1i");
    uint32_t t_matrix = guest_gl_resolve("glUniformMatrix4fv");
    uint32_t t_draw = guest_gl_resolve("glDrawElements");
    uint32_t t_disable = guest_gl_resolve("glDisableVertexAttribArray");
    uint32_t t_clear = guest_gl_resolve("glClear");
    uint32_t t_clear_color = guest_gl_resolve("glClearColor");
    uint32_t t_clear_depth = guest_gl_resolve("glClearDepth");
    uint32_t t_fbo = guest_gl_resolve("glBindFramebuffer");
    uint32_t t_cull = guest_gl_resolve("glCullFace");
    uint32_t t_viewport = guest_gl_resolve("glViewport");
    uint32_t t_create = guest_gl_resolve("glCreateProgram");
    uint32_t t_string = guest_gl_resolve("glGetString");
    uint32_t t_status = guest_gl_resolve("glCheckFramebufferStatus");
    unsigned long sweep_registered = 0u;
    unsigned long sweep_mismatch = 0u;
    uint32_t token;

    if (count != 73u || !t_blend || !t_use || !t_attrib || !t_enable ||
        !t_pointer || !t_active || !t_bind || !t_uniform || !t_uniform1i ||
        !t_matrix || !t_draw || !t_disable || !t_clear || !t_clear_color ||
        !t_clear_depth || !t_fbo || !t_cull || !t_viewport || !t_create ||
        !t_string || !t_status) {
        fprintf(stderr, "registry drifted\n");
        return 2;
    }

    guest_cpu_init(&cpu);
    s_cpu = &cpu;
    s_stack_floor = (uint32_t)(uintptr_t)&s_stack[0];
    s_stack_ceiling = (uint32_t)(uintptr_t)&s_stack[ORACLE_STACK_WORDS];
    if (guest_stack_bind(&cpu, s_stack_floor, s_stack_ceiling)) {
        fprintf(stderr, "stack bind failed\n");
        return 2;
    }
    /* The device maps the image before anything dispatches; the dispatch
     * table legs need the window to build and every leg must classify the
     * same way, so map the frozen window at the device base everywhere. */
    g_image_base = GUEST_IMAGE_BASE;
    g_image_size = UINT32_C(0x0085f000);

    /* Production-shaped import table: sorted slot RVAs far below the token
     * families (guest_import_lookup bounds).  Slot 0x006060fc is the real
     * EnterCriticalSection key; the others are neighbours. */
    s_imports[0].slot_rva = 0x00606104u; s_imports[0].name = "KERNEL32.dll!FreeLibrary";
    s_imports[1].slot_rva = 0x00606118u; s_imports[1].name = "KERNEL32.dll!GetProcAddress";
    s_imports[2].slot_rva = 0x00606124u; s_imports[2].name = "KERNEL32.dll!LoadLibraryA";
    s_imports[3].slot_rva = 0x006064c0u; s_imports[3].name = "WINMM.dll!timeGetTime";
    guest_register(s_translated_addrs, s_translated_fns, 2u);
    guest_register_imports(s_imports, 4u);

    printf("=== phase 1: recording backend, KAGE draw shapes ===\n");
    install_recording_backend();
    kage_draw(7u, 1001u, 3u, t_blend, t_use, t_attrib, t_enable, t_pointer,
              t_active, t_bind, t_uniform, t_uniform1i, t_matrix, t_draw,
              t_disable);
    kage_draw(9u, 1002u, 1u, t_blend, t_use, t_attrib, t_enable, t_pointer,
              t_active, t_bind, t_uniform, t_uniform1i, t_matrix, t_draw,
              t_disable);
    /* sub_00564f50 clear/present hook and Manager::Render state. */
    args[0] = 0x3f800000u; args[1] = 0u; args[2] = 0x3f000000u; args[3] = 0x3f800000u;
    call("clearcolor", t_clear_color, args, 4u);
    args[0] = 0x4000u;
    call("clear", t_clear, args, 1u);
    args[0] = 0u; args[1] = 0x3ff00000u;   /* GLdouble 1.0 in two slots */
    call("cleardepth", t_clear_depth, args, 2u);
    args[0] = 0x8d40u; args[1] = 3u;
    call("fbo", t_fbo, args, 2u);
    args[0] = 0x405u;
    call("cull", t_cull, args, 1u);
    args[0] = 0u; args[1] = 0u; args[2] = 960u; args[3] = 540u;
    call("viewport", t_viewport, args, 4u);
    call("create", t_create, args, 0u);
    args[0] = 0x1f02u;
    call("string", t_string, args, 1u);
    args[0] = 0x8d40u;
    call("status", t_status, args, 1u);
    printf("backend calls=%u\n", s_backend_calls);

    printf("=== phase 2: nested dispatch from a backend callback ===\n");
    s_nested_token = t_cull;
    args[0] = 0x4000u;
    call("clear-nested", t_clear, args, 1u);
    args[0] = 0x4000u;
    call("clear-after-nested", t_clear, args, 1u);

    printf("=== phase 3: every registered token, missing backend ===\n");
    guest_gl_install_backend(NULL);
    for (i = 0u; i < count; ++i) {
        uint32_t words = symbols[i].x86_stack_bytes / 4u;
        uint32_t k;
        for (k = 0u; k < words && k < 6u; ++k)
            args[k] = 0x1000u + k;
        call("missing", symbols[i].token, args, words < 6u ? words : 6u);
    }

    printf("=== phase 4: tokens outside the registry ===\n");
    for (i = 0u; i < count; ++i) {
        args[0] = 1u;
        call("near-below", symbols[i].token - 1u, args, 1u);
        call("near-above", symbols[i].token + 1u, args, 1u);
        call("family-7d", (symbols[i].token & 0x00ffffffu) | 0x7d000000u,
             args, 1u);
        call("family-7f", (symbols[i].token & 0x00ffffffu) | 0x7f000000u,
             args, 1u);
        call("family-00", symbols[i].token & 0x00ffffffu, args, 1u);
    }
    args[0] = 1u;
    call("7e-zero", 0x7e000000u, args, 1u);
    call("7e-max", 0x7effffffu, args, 1u);
    call("wgl-token-no-backend", ISAAC_VITA_GL_WGL_GET_PROC_TOKEN, args, 1u);
    call("opengl32-module-token", ISAAC_VITA_GL_OPENGL32_TOKEN, args, 1u);
    call("xinput-shaped", 0x7d200001u, args, 1u);

    printf("=== phase 5: precedence of translated code and imports ===\n");
    install_recording_backend();
    /* A translated function registered under a registry token: the dynamic
     * family owns it in both builds (cache rejects the family; guest_lookup
     * is never consulted for a dispatched token). */
    args[0] = 7u; args[1] = (uint32_t)(uintptr_t)"aPosition";
    call("translated-shadow", 0x7e307ce3u, args, 2u);
    call("translated-plain", 0x00401000u, args, 0u);
    call("translated-plain-again", 0x00401000u, args, 0u);
    call("import-slot", 0x006060fcu + 0x98000000u - 0x98000000u + 8u, args, 0u);
    call("import-slot-va", 0x98000000u + 0x00606124u, args, 0u);
    call("import-slot-rva", 0x00606124u, args, 0u);
    /* wglGetProcAddress through the dynamic family with a real name. */
    args[0] = (uint32_t)(uintptr_t)"glGetString";
    call("wgl-resolve", ISAAC_VITA_GL_WGL_GET_PROC_TOKEN, args, 1u);
    args[0] = (uint32_t)(uintptr_t)"glNotInRegistry";
    call("wgl-resolve-unknown", ISAAC_VITA_GL_WGL_GET_PROC_TOKEN, args, 1u);

    printf("=== phase 6: hostile import table reaching the token family ===\n");
    /* A registered slot key at or above 0x7e000000 makes the IAT search the
     * owner of colliding tokens.  The fast build must recompute its flag and
     * fall back to the unchanged path for the whole family. */
    memcpy(s_hostile_imports, s_imports, sizeof s_imports);
    s_hostile_imports[4].slot_rva = 0x7e307ce3u;
    s_hostile_imports[4].name = "HOSTILE.dll!ShadowsGetAttribLocation";
    guest_register_imports(s_hostile_imports, 5u);
    args[0] = 7u; args[1] = (uint32_t)(uintptr_t)"aPosition";
    call("hostile-shadowed", 0x7e307ce3u, args, 2u);
    args[0] = 0x405u;
    call("hostile-other-token", t_cull, args, 1u);
    call("hostile-missing-token", 0x7e000001u, args, 1u);
    guest_register_imports(s_imports, 4u);
    args[0] = 7u; args[1] = (uint32_t)(uintptr_t)"aPosition";
    call("restored", 0x7e307ce3u, args, 2u);

    printf("=== phase 7: membership over the 0x7d/0x7e/0x7f families ===\n");
    for (token = 0x7d000000u; token <= 0x7fffffffu; ++token) {
        int fast = guest_gl_token_is_registered(token);
        int slow = linear_registered(token);
        if (fast)
            ++sweep_registered;
        if (fast != slow) {
            ++sweep_mismatch;
            if (sweep_mismatch < 8u)
                printf("  mismatch token=%08x fast=%d slow=%d\n", token,
                       fast, slow);
        }
    }
    printf("families registered=%lu mismatch=%lu\n", sweep_registered,
           sweep_mismatch);
    printf("backend calls=%u dyn=%u imp=%u\n", s_backend_calls,
           g_host_dynamic_calls, g_host_import_calls);

    printf("=== phase 8: direct gl_bridge dispatch (bypassing guest_call) ===\n");
    /* A known token runs its adapter; an unknown token of either family
     * dispatches nothing, faults nothing and returns zero in both builds
     * (legacy: acquire, empty switch default, release; fast: no pin taken). */
    s_cpu = &cpu;
    args[0] = 0x405u;
    cpu.esp = s_stack_ceiling;
    direct("known", &cpu, t_cull, args, 1u);
    cpu.esp = s_stack_ceiling;
    direct("unknown-7e", &cpu, 0x7e000001u, args, 1u);
    cpu.esp = s_stack_ceiling;
    direct("unknown-7f", &cpu, 0x7f000001u, args, 1u);
    printf("backend calls=%u dyn=%u imp=%u\n", s_backend_calls,
           g_host_dynamic_calls, g_host_import_calls);

    printf("=== phase 9: short frames at every argument position ===\n");
    /* No backend: a whole frame reaches the unsupported-symbol fault after
     * every word was read; a cut frame faults on its first outside word. */
    guest_gl_install_backend(NULL);
    for (i = 0u; i < count; ++i) {
        uint32_t words = symbols[i].x86_stack_bytes / 4u;
        uint32_t k;

        for (k = 0u; k <= words; ++k)
            short_frame("nobackend", symbols[i].token, words, k, 0);
    }
    /* Recording backend on the pointer-free symbols: the whole frame retires
     * (the ESP delta is the stdcall cleanup), a cut frame faults before the
     * backend runs, and a floor cut under the return word reaches the
     * backend and faults in the retirement (the legacy gpop). */
    install_recording_backend();
    {
        static const char *const pointer_free[] = {
            "glActiveTexture", "glBindFramebuffer", "glBindTexture",
            "glBlendFuncSeparate", "glCheckFramebufferStatus",
            "glClearColor", "glClearDepth", "glCreateProgram", "glCullFace",
            "glDisableVertexAttribArray", "glEnableVertexAttribArray",
            "glUniform1i", "glUseProgram", "glViewport"
        };

        for (i = 0u; i < sizeof pointer_free / sizeof pointer_free[0]; ++i) {
            uint32_t tk = guest_gl_resolve(pointer_free[i]);
            uint32_t words = symbol_words(tk);
            uint32_t k;

            for (k = 0u; k <= words; ++k)
                short_frame("backend", tk, words, k, 0);
            if (words)
                short_frame("floorcut", tk, words, words, 1);
        }
    }
    guest_gl_oracle_reset_active();
    printf("backend calls=%u dyn=%u imp=%u\n", s_backend_calls,
           g_host_dynamic_calls, g_host_import_calls);

    printf("=== phase 10: hostile frames, esp outside the stack, hostile "
           "backends, table off ===\n");
    install_recording_backend();
    {
        uint32_t base = (uint32_t)(uintptr_t)&s_stack[ORACLE_STACK_WORDS / 2u];
        uint32_t words = symbol_words(t_viewport);
        uint32_t host = (uint32_t)(uintptr_t)&s_hostile_host_words[0];
        uint32_t k;

        if (words != 4u || symbol_words(t_cull) != 1u ||
            symbol_words(t_create) != 0u)
            return 2;
        /* esp far outside the bound stack, pointing at host statics shaped
         * like a valid frame: the first word read must fault as a stack
         * access, before any backend call, and no build may read them. */
        s_hostile_host_words[0] = ORACLE_RETURN_RVA;
        for (k = 1u; k < 8u; ++k)
            s_hostile_host_words[k] = 0x77770000u + k;
        hostile_frame("host-esp-4arg", t_viewport, words, host,
                      s_stack_floor, s_stack_ceiling);
        hostile_frame("host-esp-1arg", t_cull, 1u, host,
                      s_stack_floor, s_stack_ceiling);
        hostile_frame("host-esp-0arg", t_create, 0u, host,
                      s_stack_floor, s_stack_ceiling);
        hostile_frame("esp-below-floor", t_viewport, words,
                      s_stack_floor - 64u, s_stack_floor, s_stack_ceiling);
        hostile_frame("esp-at-ceiling", t_viewport, words, s_stack_ceiling,
                      s_stack_floor, s_stack_ceiling);
        /* Cuts through the middle of a word (the aligned cuts are phase 9). */
        for (k = 0u; k < words; ++k)
            hostile_frame("midword-ceiling", t_viewport, words, base,
                          s_stack_floor, base + 4u + 4u * k + 2u);
        hostile_frame("midword-floor-arg0", t_viewport, words, base,
                      base + 6u, s_stack_ceiling);
        hostile_frame("one-byte-short", t_viewport, words, base,
                      s_stack_floor, base + 4u + 4u * words - 1u);
        hostile_frame("ret-straddles-ceiling-0arg", t_create, 0u,
                      s_stack_ceiling - 2u, s_stack_floor, s_stack_ceiling);
        hostile_frame("ret-straddles-floor-0arg", t_create, 0u, base,
                      base + 2u, s_stack_ceiling);
        hostile_frame("ret-straddles-ceiling-1arg", t_cull, 1u, base,
                      s_stack_floor, base + 2u);
        /* Whole frames that must retire: exactly filling the binding, and
         * an unaligned esp (no build has an alignment rule). */
        hostile_frame("exact-fit", t_viewport, words, base, base,
                      base + 4u + 4u * words);
        hostile_frame("exact-fit-0arg", t_create, 0u, base, base, base + 4u);
        hostile_frame("unaligned-whole", t_viewport, words, base + 2u,
                      s_stack_floor, s_stack_ceiling);
    }
    /* Hostile backends: esp moved under the adapter (the retirement must
     * consume the frame at the moved esp or fault there, as the legacy
     * gpop + adjust pair does) and a tight rebinding that still holds the
     * frame. */
    args[0] = 0x405u;
    hostile_call("push2-1arg", t_cull, args, 1u, HOSTILE_PUSH2);
    args[0] = 0u; args[1] = 0u; args[2] = 960u; args[3] = 540u;
    hostile_call("push2-4arg", t_viewport, args, 4u, HOSTILE_PUSH2);
    args[0] = 0x405u;
    hostile_call("esp-to-ceiling", t_cull, args, 1u, HOSTILE_ESP_CEILING);
    hostile_call("esp-to-ceiling-4", t_cull, args, 1u,
                 HOSTILE_ESP_CEILING_M4);
    args[0] = 0u; args[1] = 0u; args[2] = 960u; args[3] = 540u;
    hostile_call("rebind-tight", t_viewport, args, 4u, HOSTILE_REBIND_TIGHT);
    /* Dispatch table off (no registered functions: "no eligible functions"
     * clears it and prints nothing) while the flag still holds: every
     * token takes the slow block; re-registration brings the table (and,
     * with the option, the tokens) back. */
    guest_register(s_translated_addrs, s_translated_fns, 0u);
    args[0] = 0x405u;
    call("table-off-cull", t_cull, args, 1u);
    args[0] = 0u; args[1] = 0u; args[2] = 960u; args[3] = 540u;
    call("table-off-viewport", t_viewport, args, 4u);
    call("table-off-translated-plain", 0x00401000u, args, 0u);
    guest_register(s_translated_addrs, s_translated_fns, 2u);
    args[0] = 0x405u;
    call("table-on-cull", t_cull, args, 1u);
    call("table-on-translated-plain", 0x00401000u, args, 0u);
    printf("backend calls=%u dyn=%u imp=%u\n", s_backend_calls,
           g_host_dynamic_calls, g_host_import_calls);

    printf("=== build-specific ===\n");
#if defined(ISAAC_VITA_GL_SHIM_FASTDISPATCH)
    printf("fast build\n");
#if defined(ISAAC_VITA_GL_SHIM_RAW_ARGS)
    /* The whole-frame check accepted every intact frame of the common
     * section (KAGE draws, phase 3/9 whole frames); zero means the fast arm
     * never ran and the differential proved nothing about it. */
    printf("raw frames=%u\n", g_guest_gl_oracle_raw_frames);
    if (g_guest_gl_oracle_raw_frames < 200u)
        return 3;
#endif
#if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
    /* Every registered-token dispatch of the common section went through
     * the table except the six that cannot: three wglGetProcAddress calls
     * (0x7d token, phases 4 and 5), the glCullFace call of phase 6 while
     * the hostile import table held the flag down, and the two typed calls
     * of phase 10 while the table was off (no registered functions). */
    printf("table gl hits=%u dyn=%u\n",
           g_guest_phase_profile_counters.dispatch_gl, g_host_dynamic_calls);
    if (g_guest_phase_profile_counters.dispatch_gl + 6u !=
            g_host_dynamic_calls)
        return 3;
#endif
    /* Exhaustive: every 32-bit token agrees with the linear scan. */
    sweep_registered = 0u;
    sweep_mismatch = 0u;
    token = 0u;
    do {
        int fast = guest_gl_token_is_registered(token);
        if (fast) {
            ++sweep_registered;
            if (!linear_registered(token))
                ++sweep_mismatch;
        } else if ((token & 0xff000000u) == 0x7e000000u &&
                   linear_registered(token)) {
            ++sweep_mismatch;
        }
        ++token;
    } while (token != 0u);
    printf("full sweep registered=%lu mismatch=%lu\n", sweep_registered,
           sweep_mismatch);
    if (sweep_registered != 73u || sweep_mismatch != 0u)
        return 3;

    /* Fail-closed foreign-CPU trap: a second CPU (a second thread would own
     * its own CPU) entering the pinned GL owner faults instead of running. */
    guest_cpu_init(&foreign);
    if (guest_stack_bind(&foreign, (uint32_t)(uintptr_t)&foreign_stack[0],
                         (uint32_t)(uintptr_t)&foreign_stack[64]))
        return 3;
    {
        unsigned before = s_backend_calls;
        unsigned dyn_before = g_host_dynamic_calls;
        foreign.esp = (uint32_t)(uintptr_t)&foreign_stack[64];
        gpush(&foreign, 0x405u);
        gpush(&foreign, ORACLE_RETURN_RVA);
        s_cpu = &foreign;
        s_pending_token = t_cull;
        s_pending_count = 1u;
        s_pending_args[0] = 0x405u;
        (void)guest_run_until_stop(&foreign, oracle_entry);
        printf("foreign fault=%s addr=%08x backend=%u dyn=%u\n",
               foreign.fault ? foreign.fault : "-", foreign.fault_addr,
               s_backend_calls - before, g_host_dynamic_calls - dyn_before);
        if (!foreign.fault ||
            strcmp(foreign.fault,
                   "guest GL dispatch from a foreign CPU/thread") != 0 ||
            s_backend_calls != before ||
            g_host_dynamic_calls != dyn_before + 1u)
            return 3;
        /* The pinned owner keeps working after the trap. */
        s_cpu = &cpu;
        args[0] = 0x405u;
        call("owner-after-foreign", t_cull, args, 1u);
        if (cpu.fault || s_backend_calls != before + 1u)
            return 3;
        /* An unknown token from the foreign CPU dispatches nothing and takes
         * no pin (the legacy path only took and released the word): the
         * owner keeps working and the foreign CPU still faults on a real
         * token afterwards, so the pin did not move. */
        foreign.esp = (uint32_t)(uintptr_t)&foreign_stack[64];
        direct("foreign-unknown", &foreign, 0x7e000001u, args, 1u);
        if (foreign.fault || s_direct_rc != 0 ||
            s_backend_calls != before + 1u)
            return 3;
        s_cpu = &cpu;
        call("owner-after-foreign-unknown", t_cull, args, 1u);
        if (cpu.fault || s_backend_calls != before + 2u)
            return 3;
        s_cpu = &foreign;
        foreign.esp = (uint32_t)(uintptr_t)&foreign_stack[64];
        (void)guest_run_until_stop(&foreign, oracle_entry);
        printf("foreign after unknown fault=%s backend=%u\n",
               foreign.fault ? foreign.fault : "-",
               s_backend_calls - before);
        if (!foreign.fault ||
            strcmp(foreign.fault,
                   "guest GL dispatch from a foreign CPU/thread") != 0 ||
            s_backend_calls != before + 2u)
            return 3;
        /* Backend teardown is the only pin reset. */
        guest_gl_install_backend(NULL);
        install_recording_backend();
        s_cpu = &foreign;
        foreign.esp = (uint32_t)(uintptr_t)&foreign_stack[64];
        (void)guest_run_until_stop(&foreign, oracle_entry);
        printf("foreign after teardown fault=%s backend=%u\n",
               foreign.fault ? foreign.fault : "-",
               s_backend_calls - before);
        if (foreign.fault || s_backend_calls != before + 3u)
            return 3;
        s_cpu = &cpu;
    }
#else
    printf("legacy build\n");
#endif
    /* A backend that rebinds the stack so the binding excludes the return
     * word while esp stays put.  The legacy retirement (gpop + adjust) faults
     * as a stack pop; the ISAAC_VITA_GL_SHIM_RAW_ARGS retirement, which
     * re-checks only that esp is still the validated frame, retires without
     * reading anything and leaves esp inside the new binding -- the one
     * documented divergence, unreachable in production (backends never
     * receive the CPU and guest_stack_bind refuses a bound CPU).  Printed
     * after the marker; recomp/test_gl_shim_fastdispatch.py pins the
     * per-build outcome. */
    /* Backend teardown is the only pin reset: the foreign-CPU section above
     * left the foreign CPU pinned in the fast builds. */
    guest_gl_install_backend(NULL);
    install_recording_backend();
    s_cpu = &cpu;
    args[0] = 0x405u;
    {
        uint32_t kind = hostile_call("rebind-exclude-return-word", t_cull,
                                     args, 1u,
                                     HOSTILE_REBIND_EXCLUDE_RETURN);

#if defined(ISAAC_VITA_GL_SHIM_RAW_ARGS)
        printf("rebind-exclude expectation=raw-retires\n");
        if (cpu.fault || kind != GUEST_STACK_FAULT_NONE)
            return 3;
#else
        printf("rebind-exclude expectation=legacy-pop-fault\n");
        if (!cpu.fault || kind != GUEST_STACK_FAULT_POP)
            return 3;
#endif
    }
    printf("GL SHIM FASTDISPATCH ORACLE DONE\n");
    return 0;
}
