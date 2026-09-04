/* Vita-side synthetic OPENGL32 loader and typed guest-GL token boundary.
 *
 * Never return a native function pointer to translated x86.  The guest's
 * epoxy thunks try desktop GL, GLES1 and GLES2 module names, ask
 * GetProcAddress for an ASCII symbol, and tail-call the result.  We preserve
 * that control flow with one stable module token; gl_bridge owns the exact
 * x86/APIENTRY argument decoding. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "gl_bridge.h"
#include "host_vita_gl.h"
#include "host_vita_import_id.h"

static uint32_t vita_gl_arg(CPU *__restrict c, uint32_t index)
{
    /* ESP points at the x86 return address while an import/dynamic call is
     * active. */
    return ld32(guest_stack_address(
        c, c->esp + 4U + index * 4U, 4U, 0U));
}

static void vita_gl_stdcall_return(CPU *__restrict c,
                                   uint32_t argument_bytes)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, argument_bytes, 0U);
}

/* Probe-only decoder for the three shared KERNEL32 imports.  Failure must not
 * fault or modify CPU state: another Vita subsystem still owns non-GL frames
 * with the same import name. */
static int vita_gl_copy_ascii(uint32_t address,
                              char output[ISAAC_VITA_GL_PROCEDURE_NAME_MAX +
                                          1U])
{
    uint32_t i;

    if (!address || (address >> 16U) == 0U)
        return 0;
    for (i = 0U; i <= ISAAC_VITA_GL_PROCEDURE_NAME_MAX; ++i) {
        uint8_t value;
        if (address > UINT32_MAX - i)
            return 0;
        value = ld8(address + i);
        if (!value) {
            output[i] = '\0';
            return 1;
        }
        if (value > 0x7fU || i == ISAAC_VITA_GL_PROCEDURE_NAME_MAX)
            return 0;
        output[i] = (char)value;
    }
    return 0;
}

static unsigned char vita_gl_ascii_fold(unsigned char value)
{
    if (value >= 'A' && value <= 'Z')
        return (unsigned char)(value + ('a' - 'A'));
    return value;
}

static int vita_gl_ascii_case_equal(const char *left, const char *right)
{
    for (;;) {
        unsigned char a = vita_gl_ascii_fold((unsigned char)*left++);
        unsigned char b = vita_gl_ascii_fold((unsigned char)*right++);
        if (a != b)
            return 0;
        if (!a)
            return 1;
    }
}

static int vita_gl_module_is_supported(const char *name)
{
    /* This is deliberately a whitelist, not a prefix/suffix probe.  libEGL
     * has different ownership and must reach the normal loud import fault. */
    return vita_gl_ascii_case_equal(name, ISAAC_VITA_GL_OPENGL32_NAME) ||
           vita_gl_ascii_case_equal(name,
                                    ISAAC_VITA_GL_OPENGL32_DLL_NAME) ||
           vita_gl_ascii_case_equal(name, ISAAC_VITA_GL_GLES1_NAME) ||
           vita_gl_ascii_case_equal(name, ISAAC_VITA_GL_GLES2_NAME);
}

static uint32_t vita_gl_resolve_procedure(const char *name)
{
    if (strcmp(name, "wglGetProcAddress") == 0)
        return ISAAC_VITA_GL_WGL_GET_PROC_TOKEN;
    return guest_gl_resolve(name);
}

#if !defined(ISAAC_VITA_GL_SHIM_FASTDISPATCH)
static int vita_gl_token_is_typed(uint32_t token)
{
    const guest_gl_symbol *symbols;
    size_t count;
    size_t i;

    symbols = guest_gl_symbols(&count);
    for (i = 0U; i < count; ++i)
        if (symbols[i].token == token)
            return 1;
    return 0;
}
#endif

static void vita_gl_wglGetProcAddress(CPU *__restrict c)
{
    uint32_t name = vita_gl_arg(c, 0U);
    char procedure[ISAAC_VITA_GL_PROCEDURE_NAME_MAX + 1U];

    /* PC prior art deliberately rejects NULL, ordinal-shaped and malformed
     * names with a NULL result.  Unknown WGL extensions do the same; notably
     * this makes wglSwapIntervalEXT optional while vitaGL owns vsync. */
    c->eax = vita_gl_copy_ascii(name, procedure) && procedure[0]
        ? guest_gl_resolve(procedure) : 0U;
    vita_gl_stdcall_return(c, 4U);
}

const char *isaac_vita_gl_wgl_import_name(uint32_t index)
{
    return index == 0U ? ISAAC_VITA_GL_WGL_GET_PROC_NAME : NULL;
}

int isaac_vita_gl_wgl_import_indexed(CPU *__restrict c, uint32_t index,
                                     unsigned *call_count)
{
    if (index != 0U)
        return 0;
    if (call_count)
        ++*call_count;
    vita_gl_wglGetProcAddress(c);
    return 1;
}

static int vita_gl_import_dispatch(CPU *__restrict c, const char *name,
                                   unsigned *call_count)
{
    uint32_t argument;
    char value[ISAAC_VITA_GL_PROCEDURE_NAME_MAX + 1U];

    if (!c || !name)
        return 0;

    if (strcmp(name, ISAAC_VITA_GL_LOAD_LIBRARY_NAME) == 0) {
        argument = vita_gl_arg(c, 0U);
        if (!vita_gl_copy_ascii(argument, value) ||
            !vita_gl_module_is_supported(value))
            return 0;
        /* Count a claimed alias before changing the synthetic guest frame. */
        if (call_count)
            ++*call_count;
        c->eax = ISAAC_VITA_GL_OPENGL32_TOKEN;
        vita_gl_stdcall_return(c, 4U);
        return 1;
    }

    if (strcmp(name, ISAAC_VITA_GL_GET_PROC_NAME) == 0) {
        if (vita_gl_arg(c, 0U) != ISAAC_VITA_GL_OPENGL32_TOKEN)
            return 0;
        argument = vita_gl_arg(c, 1U);
        if (!vita_gl_copy_ascii(argument, value))
            return 0;
        if (call_count)
            ++*call_count;
        c->eax = vita_gl_resolve_procedure(value);
        vita_gl_stdcall_return(c, 8U);
        return 1;
    }

    if (strcmp(name, ISAAC_VITA_GL_FREE_LIBRARY_NAME) == 0) {
        if (vita_gl_arg(c, 0U) != ISAAC_VITA_GL_OPENGL32_TOKEN)
            return 0;
        if (call_count)
            ++*call_count;
        c->eax = 1U;
        vita_gl_stdcall_return(c, 4U);
        return 1;
    }

    if (strcmp(name, ISAAC_VITA_GL_WGL_GET_PROC_NAME) == 0) {
        return isaac_vita_gl_wgl_import_indexed(c, 0U, call_count);
    }
    return 0;
}

int isaac_vita_gl_import(CPU *__restrict c, const char *name)
{
    return vita_gl_import_dispatch(c, name, NULL);
}

int isaac_vita_gl_import_counted(CPU *__restrict c, const char *name,
                                 unsigned *call_count)
{
    return vita_gl_import_dispatch(c, name, call_count);
}

#if defined(ISAAC_VITA_GL_SHIM_FASTDISPATCH)
static int vita_gl_dynamic_dispatch(CPU *__restrict c, uint32_t token,
                                    unsigned *call_count)
{
    if (!c)
        return 0;
    if (token == ISAAC_VITA_GL_WGL_GET_PROC_TOKEN) {
        if (call_count)
            ++*call_count;
        vita_gl_wglGetProcAddress(c);
        return 1;
    }
    /* Generated typed GL tokens occupy the exact 0x7e namespace. */
    if ((token & UINT32_C(0xff000000)) != UINT32_C(0x7e000000))
        return 0;
    /* One direct-mapped lookup in gl_bridge decides membership, counts
     * before the handler (a missing Vita callback faults inside the adapter
     * and production guest_fault does not return) and runs the guarded
     * adapter: the same outcome as the legacy scan + count + dispatch below
     * for every token (recomp/test_gl_shim_fastdispatch.py differential). */
    return guest_gl_dispatch_counted(c, token, call_count);
}
#else
static int vita_gl_dynamic_dispatch(CPU *__restrict c, uint32_t token,
                                    unsigned *call_count)
{
    int typed;

    if (!c)
        return 0;
    typed = 0;
    if (token != ISAAC_VITA_GL_WGL_GET_PROC_TOKEN) {
        /* Generated typed GL tokens occupy the exact 0x7e namespace. */
        if ((token & UINT32_C(0xff000000)) != UINT32_C(0x7e000000))
            return 0;
        typed = vita_gl_token_is_typed(token);
        if (!typed)
            return 0;
    }

    /* Count before either handler: a missing Vita callback faults inside
     * guest_gl_dispatch and production guest_fault does not return. */
    if (call_count)
        ++*call_count;
    if (token == ISAAC_VITA_GL_WGL_GET_PROC_TOKEN) {
        vita_gl_wglGetProcAddress(c);
        return 1;
    }
    if (!guest_gl_dispatch(c, token)) {
        guest_fault(c, token,
                    "typed Vita GL token was rejected by gl_bridge");
    }
    return 1;
}
#endif /* ISAAC_VITA_GL_SHIM_FASTDISPATCH */

int isaac_vita_gl_dynamic(CPU *__restrict c, uint32_t token)
{
    return vita_gl_dynamic_dispatch(c, token, NULL);
}

int isaac_vita_gl_dynamic_counted(CPU *__restrict c, uint32_t token,
                                  unsigned *call_count)
{
    return vita_gl_dynamic_dispatch(c, token, call_count);
}

_Static_assert((ISAAC_VITA_GL_OPENGL32_TOKEN & UINT32_C(0xff000000)) ==
               UINT32_C(0x7f000000),
               "OPENGL32 token left the synthetic module family");
_Static_assert((ISAAC_VITA_GL_WGL_GET_PROC_TOKEN & UINT32_C(0xff000000)) ==
               UINT32_C(0x7d000000),
               "WGL resolver token left the dynamic-function family");
