/* Compile/link/static softfp ARM oracle for the Vita GL token resolver.
 * The ELF is intentionally not executed by the host test script. */
#include <stdint.h>
#include <string.h>

#include "gl_bridge.h"
#include "host_vita_gl.h"

#if UINTPTR_MAX != UINT32_MAX
#error Vita GL resolver oracle requires a 32-bit identity-mapped host
#endif

_Static_assert(ISAAC_VITA_GL_IMPORT_COUNT == 4U,
               "Vita GL import family changed");
_Static_assert(ISAAC_VITA_GL_MODULE_ALIAS_COUNT == 4U,
               "epoxy module alias whitelist changed");
_Static_assert(GUEST_GL_PROCEDURE_NAME_MAX == 63U,
               "bounded provider-name ABI changed");
_Static_assert(ISAAC_VITA_GL_LOAD_LIBRARY_IAT_RVA == 0x00606124U,
               "LoadLibraryA IAT evidence drifted");
_Static_assert(ISAAC_VITA_GL_LOAD_LIBRARY_CALL_RVA == 0x005990a7U &&
               ISAAC_VITA_GL_LOAD_LIBRARY_RETURN_RVA == 0x005990adU,
               "main LoadLibraryA call/return evidence drifted");
_Static_assert(ISAAC_VITA_GL_GET_PROC_IAT_RVA == 0x00606118U,
               "GetProcAddress IAT evidence drifted");
_Static_assert(ISAAC_VITA_GL_GET_PROC_CALL_RVA == 0x005990bcU &&
               ISAAC_VITA_GL_GET_PROC_RETURN_RVA == 0x005990c2U,
               "main GetProcAddress call/return evidence drifted");
_Static_assert(ISAAC_VITA_GL_FREE_LIBRARY_IAT_RVA == 0x00606104U,
               "FreeLibrary IAT evidence drifted");
_Static_assert(ISAAC_VITA_GL_WGL_GET_PROC_IAT_RVA == 0x006062fcU,
               "wglGetProcAddress IAT evidence drifted");
_Static_assert(ISAAC_VITA_GL_RESOLVER_FUNCTION_RVA == 0x00599070U,
               "epoxy resolver evidence drifted");
_Static_assert(ISAAC_VITA_GL_OPENGL32_NAME_RVA == 0x00764a4cU &&
               ISAAC_VITA_GL_GLES2_NAME_RVA == 0x00764a84U &&
               ISAAC_VITA_GL_GLES1_NAME_RVA == 0x00764a94U,
               "epoxy module-name evidence drifted");
_Static_assert(ISAAC_VITA_GL_DESKTOP_MAIN_CALL_RVA == 0x00570158U &&
               ISAAC_VITA_GL_DESKTOP_MAIN_RETURN_RVA == 0x0057015dU &&
               ISAAC_VITA_GL_DESKTOP_CACHE_RVA == 0x00802ff8U,
               "desktop main resolver evidence drifted");
_Static_assert(ISAAC_VITA_GL_GLES1_MAIN_CALL_RVA == 0x00573980U &&
               ISAAC_VITA_GL_GLES1_MAIN_RETURN_RVA == 0x00573985U &&
               ISAAC_VITA_GL_GLES1_CACHE_RVA == 0x00803000U,
               "GLES1 main resolver evidence drifted");
_Static_assert(ISAAC_VITA_GL_GLES2_MAIN_CALL_RVA == 0x005739bdU &&
               ISAAC_VITA_GL_GLES2_MAIN_RETURN_RVA == 0x005739c2U &&
               ISAAC_VITA_GL_GLES2_CACHE_RVA == 0x00803004U,
               "GLES2 main resolver evidence drifted");
_Static_assert(ISAAC_VITA_GL_DIRECT_FALLBACK_FUNCTION_RVA == 0x00599330U &&
               ISAAC_VITA_GL_DIRECT_FALLBACK_CALL_RVA == 0x00573a70U &&
               ISAAC_VITA_GL_DIRECT_FALLBACK_RETURN_RVA == 0x00573a75U,
               "direct GLES2 fallback caller evidence drifted");
_Static_assert(ISAAC_VITA_GL_DIRECT_FALLBACK_CACHE_RVA == 0x00803004U &&
               ISAAC_VITA_GL_DIRECT_FALLBACK_CACHE_RVA ==
                   ISAAC_VITA_GL_GLES2_CACHE_RVA,
               "direct GLES2 fallback cache evidence drifted");
_Static_assert(ISAAC_VITA_GL_DIRECT_LOAD_CALL_RVA == 0x00599368U &&
               ISAAC_VITA_GL_DIRECT_LOAD_RETURN_RVA == 0x0059936eU &&
               ISAAC_VITA_GL_DIRECT_GET_PROC_CALL_RVA == 0x00599379U &&
               ISAAC_VITA_GL_DIRECT_GET_PROC_RETURN_RVA == 0x0059937fU,
               "direct fallback loader call/return evidence drifted");

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    c->fault_addr = address;
    c->fault = what;
}

static guest_gl_addr oracle_glGetString(guest_gl_enum name)
{
    return name == 0x1f02U ? UINT32_C(0x98760000) : 0U;
}

static uint32_t oracle_import_call(CPU *cpu, const char *import_name,
                                   const uint32_t *arguments,
                                   uint32_t argument_count,
                                   unsigned *calls, int *ok)
{
    uint32_t original_esp = cpu->esp;
    uint32_t index;

    for (index = argument_count; index != 0U; --index)
        gpush(cpu, arguments[index - 1U]);
    gpush(cpu, UINT32_C(0xaabbccdd));
    if (!isaac_vita_gl_import_counted(cpu, import_name, calls) ||
        cpu->esp != original_esp)
        *ok = 0;
    return cpu->eax;
}

static int oracle_delegated_import(CPU *cpu, const char *import_name,
                                   const uint32_t *arguments,
                                   uint32_t argument_count,
                                   unsigned *calls)
{
    CPU original = *cpu;
    CPU entry;
    unsigned before = *calls;
    uint32_t index;
    int delegated;

    for (index = argument_count; index != 0U; --index)
        gpush(cpu, arguments[index - 1U]);
    gpush(cpu, UINT32_C(0x12345678));
    entry = *cpu;
    delegated = isaac_vita_gl_import_counted(cpu, import_name, calls) == 0 &&
        *calls == before && memcmp(cpu, &entry, sizeof entry) == 0;
    *cpu = original;
    return delegated;
}

static uint32_t oracle_dynamic_call(CPU *cpu, uint32_t token,
                                    const uint32_t *arguments,
                                    uint32_t argument_count,
                                    unsigned *calls, int *ok)
{
    uint32_t original_esp = cpu->esp;
    uint32_t index;

    for (index = argument_count; index != 0U; --index)
        gpush(cpu, arguments[index - 1U]);
    gpush(cpu, UINT32_C(0xddccbbaa));
    if (!isaac_vita_gl_dynamic_counted(cpu, token, calls) ||
        cpu->esp != original_esp)
        *ok = 0;
    return cpu->eax;
}

int main(void)
{
    static const char *const module_aliases[
        ISAAC_VITA_GL_MODULE_ALIAS_COUNT] = {
        "OPENGL32",
        "OpEnGl32.DlL",
        "LiBgLeSv1_cM.So.1",
        "lIbGlEsV2.So.2"
    };
    static const char *const rejected_modules[] = {
        "kernel32.dll",
        "libEGL.so.1",
        "libGLESv1_CM.so",
        "libGLESv2.so",
        "OPENGL32.dll.1"
    };
    static const char get_string_name[] = "glGetString";
    static const char create_shader_name[] = "glCreateShader";
    static const char missing_name[] = "glDefinitelyMissing";
    static const char wgl_name[] = "wglGetProcAddress";
    uint32_t stack[64];
    uint32_t args[2];
    CPU cpu;
    CPU snapshot;
    guest_gl_backend backend;
    unsigned imports = 0U;
    unsigned dynamics = 0U;
    unsigned before;
    uint32_t module;
    uint32_t gl_token;
    uint32_t create_shader_token;
    uint32_t wgl_token;
    size_t index;
    int ok = 1;

    memset(&cpu, 0, sizeof cpu);
    cpu.esp = (uint32_t)(uintptr_t)&stack[64];

    /* Every non-whitelisted module is delegated byte-for-byte untouched and
     * uncounted.  The enclosing first-fault dispatcher then reports it loudly
     * instead of this GL boundary silently manufacturing a handle. */
    for (index = 0U;
         index < sizeof rejected_modules / sizeof rejected_modules[0];
         ++index) {
        args[0] = (uint32_t)(uintptr_t)rejected_modules[index];
        if (!oracle_delegated_import(&cpu,
                                     ISAAC_VITA_GL_LOAD_LIBRARY_NAME,
                                     args, 1U, &imports))
            return 1;
    }

    /* The exact four aliases are case-insensitive, share one token, and each
     * increments the import count before returning the synthetic handle. */
    module = 0U;
    for (index = 0U; index < ISAAC_VITA_GL_MODULE_ALIAS_COUNT; ++index) {
        before = imports;
        args[0] = (uint32_t)(uintptr_t)module_aliases[index];
        module = oracle_import_call(&cpu, ISAAC_VITA_GL_LOAD_LIBRARY_NAME,
                                    args, 1U, &imports, &ok);
        if (module != ISAAC_VITA_GL_OPENGL32_TOKEN ||
            imports != before + 1U)
            ok = 0;
    }

    /* GetProcAddress and FreeLibrary retain their exact-token ownership.
     * Foreign module handles remain available to the normal loud dispatcher. */
    args[0] = UINT32_C(0x7f000099);
    args[1] = (uint32_t)(uintptr_t)get_string_name;
    if (!oracle_delegated_import(&cpu, ISAAC_VITA_GL_GET_PROC_NAME,
                                 args, 2U, &imports))
        return 1;
    if (!oracle_delegated_import(&cpu, ISAAC_VITA_GL_FREE_LIBRARY_NAME,
                                 args, 1U, &imports))
        return 1;

    args[0] = module;
    args[1] = (uint32_t)(uintptr_t)get_string_name;
    gl_token = oracle_import_call(&cpu, ISAAC_VITA_GL_GET_PROC_NAME,
                                  args, 2U, &imports, &ok);
    args[1] = (uint32_t)(uintptr_t)create_shader_name;
    create_shader_token = oracle_import_call(
        &cpu, ISAAC_VITA_GL_GET_PROC_NAME, args, 2U, &imports, &ok);
    args[1] = (uint32_t)(uintptr_t)wgl_name;
    wgl_token = oracle_import_call(&cpu, ISAAC_VITA_GL_GET_PROC_NAME,
                                   args, 2U, &imports, &ok);
    args[1] = (uint32_t)(uintptr_t)missing_name;
    if (oracle_import_call(&cpu, ISAAC_VITA_GL_GET_PROC_NAME,
                           args, 2U, &imports, &ok) != 0U)
        ok = 0;

    args[0] = (uint32_t)(uintptr_t)get_string_name;
    if (oracle_import_call(&cpu, ISAAC_VITA_GL_WGL_GET_PROC_NAME,
                           args, 1U, &imports, &ok) != gl_token)
        ok = 0;
    args[0] = module;
    if (oracle_import_call(&cpu, ISAAC_VITA_GL_FREE_LIBRARY_NAME,
                           args, 1U, &imports, &ok) != 1U)
        ok = 0;

    memset(&backend, 0, sizeof backend);
    backend.glGetString = oracle_glGetString;
    guest_gl_install_backend(&backend);
    args[0] = (uint32_t)(uintptr_t)get_string_name;
    if (oracle_dynamic_call(&cpu, wgl_token, args, 1U,
                            &dynamics, &ok) != gl_token)
        ok = 0;
    args[0] = 0x1f02U;
    if (oracle_dynamic_call(&cpu, gl_token, args, 1U,
                            &dynamics, &ok) != UINT32_C(0x98760000))
        ok = 0;
    guest_gl_install_backend(NULL);

    if (!ok || module != ISAAC_VITA_GL_OPENGL32_TOKEN ||
        gl_token != guest_gl_resolve(get_string_name) || !gl_token ||
        create_shader_token != guest_gl_resolve(create_shader_name) ||
        !create_shader_token ||
        wgl_token != ISAAC_VITA_GL_WGL_GET_PROC_TOKEN ||
        imports != 10U || dynamics != 2U)
        return 2;

    snapshot = cpu;
    if (isaac_vita_gl_dynamic_counted(
            &cpu, UINT32_C(0x12345678), &dynamics) != 0 ||
        dynamics != 2U || memcmp(&cpu, &snapshot, sizeof cpu) != 0)
        return 3;
    return 0;
}
