/* Native WGL/OpenGL implementation of guest_gl_backend.
 *
 * The generated guest bridge owns x86 stack decoding.  This file is the one
 * platform edge where 32-bit identity-mapped guest addresses become native
 * PC pointers, and where typed APIENTRY pointers returned by KAGE's resolver
 * are called.  It is intentionally unusable in a 64-bit process: an array of
 * guest pointers (notably glShaderSource) is only layout-identical to a native
 * pointer array when both pointer widths are four bytes. */
#include "gl_pc_backend.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gl_bridge.h"
#include "kage_pc_backend.h"

#define GL_PC_MAX_SYMBOLS GUEST_GL_SURFACE_COUNT

static size_t s_gl_pc_resolved_count;
static size_t s_gl_pc_missing_count;
static const char *s_gl_pc_missing[GL_PC_MAX_SYMBOLS];
static int s_gl_pc_installed;
static char s_gl_pc_error[192];

static void gl_pc_set_error(const char *message)
{
    if (!message)
        message = "unknown PC GL backend error";
#if defined(_MSC_VER)
    _snprintf_s(s_gl_pc_error, sizeof s_gl_pc_error, _TRUNCATE,
                "%s", message);
#else
    snprintf(s_gl_pc_error, sizeof s_gl_pc_error, "%s", message);
#endif
}

static void gl_pc_record_resolved(void)
{
    s_gl_pc_resolved_count++;
}

static void gl_pc_record_missing(const char *name)
{
    if (s_gl_pc_missing_count < GL_PC_MAX_SYMBOLS)
        s_gl_pc_missing[s_gl_pc_missing_count++] = name;
}

#if defined(_WIN32) && UINTPTR_MAX == UINT32_MAX

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Exact Khronos scalar layout, without importing a second GL header into the
 * guest ABI translation unit. */
typedef unsigned int  GLenum;
typedef unsigned int  GLbitfield;
typedef unsigned int  GLuint;
typedef int           GLint;
typedef int           GLsizei;
typedef unsigned char GLboolean;
typedef float         GLfloat;
typedef double        GLdouble;
typedef char          GLchar;
typedef unsigned char GLubyte;

static void gl_pc_debug_after_call(const char *name)
{
    kage_pc_backend_note_guest_gl_call(name);
}

#include "gl_pc_backend_generated.inc"

typedef char gl_pc_surface_count_must_match[
    GL_PC_BACKEND_GENERATED_COUNT == GUEST_GL_SURFACE_COUNT ? 1 : -1];

int gl_pc_backend_install(void)
{
    guest_gl_backend backend;

    if (!kage_pc_backend_ready()) {
        gl_pc_set_error("PC GL backend: KAGE WGL context is not ready");
        return 0;
    }

    memset(&backend, 0, sizeof backend);
    memset(&s_gl_pc_native, 0, sizeof s_gl_pc_native);
    memset((void *)s_gl_pc_missing, 0, sizeof s_gl_pc_missing);
    s_gl_pc_resolved_count = 0u;
    s_gl_pc_missing_count = 0u;
    s_gl_pc_installed = 0;

    gl_pc_resolve_generated();
    gl_pc_bind_generated(&backend);
    guest_gl_install_backend(&backend);
    s_gl_pc_installed = 1;

    if (s_gl_pc_missing_count) {
#if defined(_MSC_VER)
        _snprintf_s(s_gl_pc_error, sizeof s_gl_pc_error, _TRUNCATE,
                    "PC GL backend: %u of %u native symbols missing",
                    (unsigned)s_gl_pc_missing_count,
                    (unsigned)GUEST_GL_SURFACE_COUNT);
#else
        snprintf(s_gl_pc_error, sizeof s_gl_pc_error,
                 "PC GL backend: %u of %u native symbols missing",
                 (unsigned)s_gl_pc_missing_count,
                 (unsigned)GUEST_GL_SURFACE_COUNT);
#endif
    } else {
        s_gl_pc_error[0] = '\0';
    }
    return 1;
}

#else

int gl_pc_backend_install(void)
{
    gl_pc_set_error(
        "PC GL backend: requires a 32-bit Windows identity-pointer build");
    return 0;
}

#endif

void gl_pc_backend_uninstall(void)
{
    /* Call this before destroying the WGL context; it prevents stale driver
     * function pointers from remaining reachable through guest dispatch. */
    guest_gl_install_backend(NULL);
    s_gl_pc_installed = 0;
    s_gl_pc_resolved_count = 0u;
    s_gl_pc_missing_count = 0u;
    memset((void *)s_gl_pc_missing, 0, sizeof s_gl_pc_missing);
}

int gl_pc_backend_installed(void) { return s_gl_pc_installed; }
int gl_pc_backend_complete(void)
{
    return s_gl_pc_installed && s_gl_pc_missing_count == 0u;
}
size_t gl_pc_backend_resolved_count(void) { return s_gl_pc_resolved_count; }
size_t gl_pc_backend_missing_count(void) { return s_gl_pc_missing_count; }
const char *gl_pc_backend_missing_symbol(size_t index)
{
    return index < s_gl_pc_missing_count ? s_gl_pc_missing[index] : NULL;
}
const char *gl_pc_backend_last_error(void)
{
    return s_gl_pc_error[0] ? s_gl_pc_error : "PC GL backend: no error";
}
