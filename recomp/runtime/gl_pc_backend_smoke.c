/* End-to-end proof: hidden WGL -> typed native PC table -> generated guest
 * x86 stdcall adapters -> real driver calls. */
#include "gl_pc_backend.h"
#include "gl_bridge.h"
#include "kage_pc_backend.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define GL_VERSION_VALUE          0x1f02u
#define GL_EXTENSIONS_VALUE       0x1f03u
#define GL_COLOR_BUFFER_BIT_VALUE 0x00004000u
#define GL_RGBA_VALUE             0x1908u
#define GL_UNSIGNED_BYTE_VALUE    0x1401u

void guest_fault(CPU *__restrict c, uint32_t addr, const char *what)
{
    c->fault = what;
    c->fault_addr = addr;
}

static uint32_t float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static int dispatch_call(CPU *cpu, uint32_t stack_base, const char *name,
                         const uint32_t *slots, size_t slot_count,
                         uint32_t *result)
{
    uint32_t token = guest_gl_resolve(name);
    uint32_t start = stack_base + 0x100u;
    size_t index;

    memset(cpu, 0, sizeof *cpu);
    cpu->esp = start;
    st32(start, 0xc001c0deu);
    for (index = 0; index < slot_count; ++index)
        st32(start + 4u + (uint32_t)index * 4u, slots[index]);
    if (!token || !guest_gl_dispatch(cpu, token)) {
        printf("dispatch %-12s: registry miss\n", name);
        return 0;
    }
    if (cpu->fault) {
        printf("dispatch %-12s: fault 0x%08x %s\n", name,
               cpu->fault_addr, cpu->fault);
        return 0;
    }
    if (cpu->esp != start + 4u + (uint32_t)slot_count * 4u) {
        printf("dispatch %-12s: bad ESP 0x%08x expected 0x%08x\n",
               name, cpu->esp,
               start + 4u + (uint32_t)slot_count * 4u);
        return 0;
    }
    if (result)
        *result = cpu->eax;
    return 1;
}

static int close_component(unsigned actual, unsigned expected)
{
    return actual + 2u >= expected && actual <= expected + 2u;
}

int main(void)
{
    CPU cpu;
    void *stack;
    uint32_t stack_base;
    uint32_t result = 0u;
    uint32_t version_args[1] = { GL_VERSION_VALUE };
    uint32_t extension_args[2] = { GL_EXTENSIONS_VALUE, 0u };
    uint32_t colour_args[4] = {
        0u, 0u, 0u, 0u
    };
    uint32_t clear_args[1] = { GL_COLOR_BUFFER_BIT_VALUE };
    uint32_t read_args[7];
    uint8_t pixel[4] = { 0u, 0u, 0u, 0u };
    size_t index;
    int ok = 1;

    stack = VirtualAlloc(NULL, 0x10000u, MEM_RESERVE | MEM_COMMIT,
                         PAGE_READWRITE);
    if (!stack) {
        printf("mapping      : FAIL Win32 %lu\n", (unsigned long)GetLastError());
        return 1;
    }
    stack_base = (uint32_t)(uintptr_t)stack;

    ok = ok && !gl_pc_backend_installed();
    if (!kage_pc_backend_set_mode(KAGE_PC_BACKEND_HIDDEN) ||
        !kage_pc_backend_initialize(64u, 64u)) {
        printf("WGL context  : FAIL %s\n", kage_pc_backend_last_error());
        VirtualFree(stack, 0, MEM_RELEASE);
        return 1;
    }
    printf("WGL context  : %s, accelerated=%s\n",
           kage_pc_backend_gl_version(),
           kage_pc_backend_accelerated() ? "yes" : "no");
    if (kage_pc_backend_gl_proc("glGetString") == 0u ||
        kage_pc_backend_gl_proc("glGetStringi") == 0u ||
        kage_pc_backend_gl_proc("glBindFramebuffer") == 0u ||
        kage_pc_backend_gl_proc("glDefinitelyNotIsaac") != 0u) {
        printf("resolver     : FAIL core/extension/unknown exactness\n");
        ok = 0;
    }

    if (!gl_pc_backend_install()) {
        printf("install      : FAIL %s\n", gl_pc_backend_last_error());
        ok = 0;
    }
    printf("native census: resolved=%u missing=%u complete=%s\n",
           (unsigned)gl_pc_backend_resolved_count(),
           (unsigned)gl_pc_backend_missing_count(),
           gl_pc_backend_complete() ? "yes" : "no");
    for (index = 0; index < gl_pc_backend_missing_count(); ++index)
        printf("missing      : %s\n", gl_pc_backend_missing_symbol(index));
    ok = ok && gl_pc_backend_installed() && gl_pc_backend_complete() &&
         gl_pc_backend_resolved_count() == GUEST_GL_SURFACE_COUNT;

    if (!dispatch_call(&cpu, stack_base, "glGetString", version_args, 1u,
                       &result) || !result) {
        printf("glGetString  : FAIL\n");
        ok = 0;
    } else {
        const char *version = (const char *)(uintptr_t)result;
        printf("glGetString  : %s\n", version);
        ok = ok && version[0] != '\0';
    }

    if (!dispatch_call(&cpu, stack_base, "glGetStringi", extension_args, 2u,
                       &result) || !result) {
        printf("glGetStringi : FAIL\n");
        ok = 0;
    } else {
        const char *extension = (const char *)(uintptr_t)result;
        printf("glGetStringi : %s\n", extension);
        ok = ok && extension[0] != '\0';
    }

    colour_args[0] = float_bits(0.2f);
    colour_args[1] = float_bits(0.4f);
    colour_args[2] = float_bits(0.6f);
    colour_args[3] = float_bits(1.0f);
    ok = dispatch_call(&cpu, stack_base, "glClearColor", colour_args, 4u,
                       NULL) && ok;
    ok = dispatch_call(&cpu, stack_base, "glClear", clear_args, 1u,
                       NULL) && ok;

    read_args[0] = 32u;
    read_args[1] = 32u;
    read_args[2] = 1u;
    read_args[3] = 1u;
    read_args[4] = GL_RGBA_VALUE;
    read_args[5] = GL_UNSIGNED_BYTE_VALUE;
    read_args[6] = (uint32_t)(uintptr_t)pixel;
    ok = dispatch_call(&cpu, stack_base, "glReadPixels", read_args, 7u,
                       NULL) && ok;
    printf("dispatch pixel: %u,%u,%u,%u\n",
           pixel[0], pixel[1], pixel[2], pixel[3]);
    ok = ok && close_component(pixel[0], 51u) &&
         close_component(pixel[1], 102u) &&
         close_component(pixel[2], 153u) && pixel[3] >= 253u;

    if (!kage_pc_backend_present()) {
        printf("present      : FAIL %s\n", kage_pc_backend_last_error());
        ok = 0;
    }
    gl_pc_backend_uninstall();
    ok = ok && !gl_pc_backend_installed();
    kage_pc_backend_shutdown();
    VirtualFree(stack, 0, MEM_RELEASE);
    printf("VERDICT      : %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
