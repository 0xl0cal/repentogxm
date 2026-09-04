/* Fast standalone proof of both sides of the manual KAGE seam:
 *
 * - disabled mode retains the exact loud fault and leaves the guest return;
 * - inventory mode delegates to the preserved implementation;
 * - explicit PC mode performs a real WGL clear/present during setup and
 *   observes exact thiscall return-stack and EAX behaviour; the public
 *   Present wrapper delegates to the production-generated leaf seam.
 *
 * Generated originals and guest_fault are tiny test doubles here.  The full
 * harness separately pins dispatch to these public wrappers. */
#include "manual_kage.h"
#include "gl_bridge.h"
#include "gl_pc_backend.h"
#include "kage_pc_backend.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>

#define MANAGER_PAGE ((void *)(uintptr_t)0x307c0000u)
#define CACHE_PAGE   ((void *)(uintptr_t)0x30800000u)
#define SOUND_PAGE   ((void *)(uintptr_t)0x307e0000u)
#define MANAGER      0x307c7a30u
#define MANAGER_C8   (MANAGER + 0xc8u)
#define MANAGER_CC   (MANAGER + 0xccu)
#define MANAGER_D8   (MANAGER + 0xd8u)
#define MANAGER_E0   (MANAGER + 0xe0u)
#define MANAGER_E4   (MANAGER + 0xe4u)
#define GL_QUERY_SLOT (MANAGER + 0x0200u)
#define WIDTH_CACHE       0x308071a0u
#define HEIGHT_CACHE      0x308071a4u
#define WIDTH_COMPANION   0x308071a8u
#define HEIGHT_COMPANION  0x308071acu
#define SOUND_MANAGER      0x307e7d40u
#define SOUND_RUNNING      (SOUND_MANAGER + 0x04u)
#define SOUND_SUSPENDED    (SOUND_MANAGER + 0x05u)
#define SOUND_CONTEXT      (SOUND_MANAGER + 0x30u)
#define SOUND_DEVICE       (SOUND_MANAGER + 0x34u)
#define OPTIONS_CONFIG     0x307c1000u
#define OPTIONS_VSYNC      (OPTIONS_CONFIG + 0x55u)
#define KAGE_VSYNC_STATE   0x307c7b0du

#define GL_FRAMEBUFFER_VALUE              0x8d40u
#define GL_RENDERBUFFER_VALUE             0x8d41u
#define GL_DEPTH_ATTACHMENT_VALUE         0x8d00u
#define GL_DEPTH_COMPONENT24_VALUE        0x81a6u
#define GL_DRAW_FRAMEBUFFER_BINDING_VALUE 0x8ca6u
#define RENDER_TARGET_RETURN              0xabc65030u

int g_guest_gl_inventory_mode;
/* This standalone smoke deliberately omits guest.c and exercises the
 * coverage-disabled path through the public manual wrappers. */
unsigned char *g_guest_coverage_functions = NULL;
unsigned char *g_guest_coverage_imports = NULL;
unsigned char *g_guest_coverage_cases = NULL;
static unsigned inventory_calls;
static unsigned render_target_calls;
static unsigned render_target_saw_existing_renderbuffer;
static int render_target_abi_ok = 1;

#define DEFINE_ORIGINAL(symbol, rva, label)            \
    void guest_original_##symbol(CPU *__restrict c)    \
    {                                                   \
        (void)(rva);                                    \
        inventory_calls++;                              \
        (void)gpop(c);                                  \
        if ((rva) == GUEST_KAGE_VSYNC_RVA)              \
            c->esp += 4u;                               \
    }
GUEST_MANUAL_KAGE_FUNCTIONS(DEFINE_ORIGINAL)
#undef DEFINE_ORIGINAL

void guest_fault(CPU *__restrict c, uint32_t addr, const char *what)
{
    c->fault_addr = addr;
    c->fault = what;
}

static int smoke_gl_call(CPU *__restrict c, const char *name,
                         const uint32_t *arguments, uint32_t argument_count)
{
    uint32_t saved = c->esp;
    uint32_t token = guest_gl_resolve(name);
    uint32_t index;

    if (!token) {
        guest_fault(c, 0x00565030u, "smoke render target: registry miss");
        return 0;
    }
    for (index = argument_count; index != 0u; --index)
        gpush(c, arguments[index - 1u]);
    gpush(c, 0xcafe6503u);
    if (!guest_gl_dispatch(c, token)) {
        c->esp = saved;
        guest_fault(c, 0x00565030u, "smoke render target: dispatch miss");
        return 0;
    }
    if (c->fault) {
        c->esp = saved;
        return 0;
    }
    if (c->esp != saved) {
        c->esp = saved;
        guest_fault(c, 0x00565030u, "smoke render target: GL ABI drift");
        return 0;
    }
    return 1;
}

static uint32_t render_target_extent(uint32_t requested)
{
    uint32_t extent = 8u;
    while (extent < requested)
        extent <<= 1u;
    return extent;
}

static void render_target_fault(CPU *__restrict c, const char *message)
{
    render_target_abi_ok = 0;
    guest_fault(c, 0x00565030u, message);
}

/* Strict stand-in for the translated game-owned helper.  It verifies the
 * exact thiscall+RET4 boundary and current FBO, then creates and attaches a
 * real depth renderbuffer through the generated typed GL adapters. */
void sub_00565030(CPU *__restrict c)
{
    uint32_t entry = c->esp;
    uint32_t arguments[4];
    uint32_t saved_framebuffer;
    uint32_t old_renderbuffer;

    if (c->ecx != MANAGER || ld32(entry) != RENDER_TARGET_RETURN ||
        ld32(entry + 4u) != 1u) {
        render_target_fault(c,
                            "smoke render target: this/force/return mismatch");
        return;
    }

    st32(GL_QUERY_SLOT, 0xffffffffu);
    arguments[0] = GL_DRAW_FRAMEBUFFER_BINDING_VALUE;
    arguments[1] = GL_QUERY_SLOT;
    if (!smoke_gl_call(c, "glGetIntegerv", arguments, 2u)) {
        render_target_abi_ok = 0;
        return;
    }
    saved_framebuffer = ld32(GL_QUERY_SLOT);
    if (!saved_framebuffer || saved_framebuffer != ld32(MANAGER_C8)) {
        render_target_fault(c,
                            "smoke render target: expected real current FBO");
        return;
    }

    old_renderbuffer = ld32(MANAGER_CC);
    if (old_renderbuffer) {
        arguments[0] = 1u;
        arguments[1] = MANAGER_CC;
        if (!smoke_gl_call(c, "glDeleteRenderbuffers", arguments, 2u)) {
            render_target_abi_ok = 0;
            return;
        }
        render_target_saw_existing_renderbuffer++;
    }

    arguments[0] = GL_FRAMEBUFFER_VALUE;
    arguments[1] = ld32(MANAGER_C8);
    if (!smoke_gl_call(c, "glBindFramebuffer", arguments, 2u)) {
        render_target_abi_ok = 0;
        return;
    }
    arguments[0] = 1u;
    arguments[1] = MANAGER_CC;
    if (!smoke_gl_call(c, "glGenRenderbuffers", arguments, 2u) ||
        ld32(MANAGER_CC) == 0u) {
        render_target_abi_ok = 0;
        if (!c->fault)
            render_target_fault(c,
                                "smoke render target: real RBO generation failed");
        return;
    }
    arguments[0] = GL_RENDERBUFFER_VALUE;
    arguments[1] = ld32(MANAGER_CC);
    if (!smoke_gl_call(c, "glBindRenderbuffer", arguments, 2u)) {
        render_target_abi_ok = 0;
        return;
    }
    arguments[0] = GL_RENDERBUFFER_VALUE;
    arguments[1] = GL_DEPTH_COMPONENT24_VALUE;
    arguments[2] = render_target_extent(ld32(MANAGER_E0));
    arguments[3] = render_target_extent(ld32(MANAGER_E4));
    if (!smoke_gl_call(c, "glRenderbufferStorage", arguments, 4u)) {
        render_target_abi_ok = 0;
        return;
    }
    arguments[0] = GL_FRAMEBUFFER_VALUE;
    arguments[1] = GL_DEPTH_ATTACHMENT_VALUE;
    arguments[2] = GL_RENDERBUFFER_VALUE;
    arguments[3] = ld32(MANAGER_CC);
    if (!smoke_gl_call(c, "glFramebufferRenderbuffer", arguments, 4u)) {
        render_target_abi_ok = 0;
        return;
    }
    arguments[0] = GL_FRAMEBUFFER_VALUE;
    arguments[1] = saved_framebuffer;
    if (!smoke_gl_call(c, "glBindFramebuffer", arguments, 2u) ||
        c->esp != entry) {
        render_target_abi_ok = 0;
        if (!c->fault)
            render_target_fault(c, "smoke render target: internal ABI drift");
        return;
    }

    render_target_calls++;
    (void)gpop(c);                    /* exact RET 4 */
    c->esp += 4u;
}

static int current_draw_framebuffer(CPU *__restrict c, uint32_t *framebuffer)
{
    uint32_t arguments[2];

    st32(GL_QUERY_SLOT, 0xffffffffu);
    arguments[0] = GL_DRAW_FRAMEBUFFER_BINDING_VALUE;
    arguments[1] = GL_QUERY_SLOT;
    if (!smoke_gl_call(c, "glGetIntegerv", arguments, 2u))
        return 0;
    *framebuffer = ld32(GL_QUERY_SLOT);
    return 1;
}

static void prepare_call(CPU *c, uint32_t stack_top, uint32_t sentinel)
{
    c->esp = stack_top;
    c->ecx = MANAGER;
    c->fault = NULL;
    c->fault_addr = 0;
    gpush(c, sentinel);
}

static int returned_exactly(const CPU *c, uint32_t stack_top)
{
    return c->esp == stack_top && !c->fault;
}

int main(void)
{
    CPU cpu;
    void *manager_page;
    void *cache_page;
    void *sound_page;
    void *stack;
    uint32_t stack_top;
    uint32_t refresh;
    uint32_t draw_framebuffer = 0xffffffffu;
    uint32_t first_framebuffer;
    uint32_t first_renderbuffer;
    unsigned changes;
    unsigned original_calls;
    unsigned presents;
    int policy;
    int draw_query_ok;
    int ok = 1;

    memset(&cpu, 0, sizeof cpu);
    manager_page = VirtualAlloc(MANAGER_PAGE, 0x10000u,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    cache_page = VirtualAlloc(CACHE_PAGE, 0x10000u,
                              MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    sound_page = VirtualAlloc(SOUND_PAGE, 0x10000u,
                              MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    stack = VirtualAlloc(NULL, 0x10000u, MEM_RESERVE | MEM_COMMIT,
                         PAGE_READWRITE);
    if (manager_page != MANAGER_PAGE || cache_page != CACHE_PAGE ||
        sound_page != SOUND_PAGE || !stack) {
        printf("mapping: FAIL manager=%p cache=%p sound=%p stack=%p\n",
               manager_page, cache_page, sound_page, stack);
        return 1;
    }
    stack_top = (uint32_t)(uintptr_t)stack + 0x10000u;
    st32(MANAGER_C8, 0x1111aaaau);
    st32(MANAGER_CC, 0x2222bbbbu);
    st32(MANAGER_D8, 0xdeadbeefu);
    st32(MANAGER_E0, 73u);
    st32(MANAGER_E4, 41u);

    kage_pc_backend_set_mode(KAGE_PC_BACKEND_DISABLED);
    prepare_call(&cpu, stack_top, 0xa0010001u);
    sub_00561830(&cpu);
    ok = ok && cpu.fault_addr == 0x00561830u && cpu.fault &&
         strcmp(cpu.fault,
                "KAGE graphics backend unavailable: InitializeRenderDisplay") == 0 &&
         cpu.esp == stack_top - 4u && ld32(cpu.esp) == 0xa0010001u &&
         ld32(MANAGER_C8) == 0x1111aaaau &&
         ld32(MANAGER_CC) == 0x2222bbbbu &&
         ld32(MANAGER_D8) == 0xdeadbeefu &&
         !guest_kage_pc_handoff_ready();
    printf("default boundary : %s\n", ok ? "exact loud fault" : "FAIL");

    st8(SOUND_RUNNING, 0x5au);
    st8(SOUND_SUSPENDED, 0xa5u);
    st32(SOUND_CONTEXT, 0x11112222u);
    st32(SOUND_DEVICE, 0x33334444u);
    prepare_call(&cpu, stack_top, 0xa0010010u);
    cpu.ecx = SOUND_MANAGER;
    sub_0056dd70(&cpu);
    ok = ok && cpu.fault_addr == GUEST_KAGE_SOUND_INITIALIZE_RVA &&
         cpu.fault &&
         strcmp(cpu.fault, GUEST_KAGE_SOUND_INITIALIZE_FAULT) == 0 &&
         cpu.esp == stack_top - 4u && ld32(cpu.esp) == 0xa0010010u &&
         ld8(SOUND_RUNNING) == 0x5au && ld8(SOUND_SUSPENDED) == 0xa5u &&
         ld32(SOUND_CONTEXT) == 0x11112222u &&
         ld32(SOUND_DEVICE) == 0x33334444u;
    printf("default audio    : %s\n",
           cpu.fault_addr == GUEST_KAGE_SOUND_INITIALIZE_RVA
           ? "exact loud fault" : "FAIL");

    cpu.esp = stack_top;
    cpu.fault = NULL;
    cpu.fault_addr = 0;
    g_guest_gl_inventory_mode = 1;
    prepare_call(&cpu, stack_top, 0xa0010002u);
    sub_00561830(&cpu);
    ok = ok && returned_exactly(&cpu, stack_top) && inventory_calls == 1u &&
         !kage_pc_backend_ready() && !guest_kage_pc_handoff_ready();
    printf("inventory route  : %s\n",
           returned_exactly(&cpu, stack_top) && inventory_calls == 1u
           ? "delegated" : "FAIL");

    prepare_call(&cpu, stack_top, 0xa0010011u);
    cpu.ecx = SOUND_MANAGER;
    sub_0056dd70(&cpu);
    ok = ok && returned_exactly(&cpu, stack_top) && inventory_calls == 2u;
    printf("inventory audio  : %s\n",
           returned_exactly(&cpu, stack_top) && inventory_calls == 2u
           ? "delegated" : "FAIL");

    st8(OPTIONS_VSYNC, 0xa5u);
    st8(KAGE_VSYNC_STATE, 0x5au);
    cpu.esp = stack_top;
    cpu.ecx = OPTIONS_CONFIG;
    gpush(&cpu, 1u);
    gpush(&cpu, 0xa0010013u);
    sub_00481280(&cpu);
    ok = ok && returned_exactly(&cpu, stack_top) && inventory_calls == 3u &&
         ld8(OPTIONS_VSYNC) == 0xa5u && ld8(KAGE_VSYNC_STATE) == 0x5au;
    printf("inventory VSync : %s\n",
           returned_exactly(&cpu, stack_top) && inventory_calls == 3u
           ? "delegated" : "FAIL");

    g_guest_gl_inventory_mode = 0;
    kage_pc_backend_set_mode(KAGE_PC_BACKEND_HIDDEN);
    prepare_call(&cpu, stack_top, 0xa0010016u);
    sub_00560eb0(&cpu);
    ok = ok && cpu.fault_addr == 0x00560eb0u && cpu.fault &&
         strcmp(cpu.fault,
                "KAGE PC backend: Present requested before initialize") == 0 &&
         cpu.esp == stack_top - 4u && ld32(cpu.esp) == 0xa0010016u &&
         inventory_calls == 3u && kage_pc_backend_present_count() == 0u;
    printf("present pre-init : %s\n",
           cpu.fault_addr == 0x00560eb0u
           ? "exact loud fault" : "FAIL");

    st32(MANAGER_C8, 0u);
    st32(MANAGER_CC, 0u);
    prepare_call(&cpu, stack_top, 0xa0010003u);
    sub_00561830(&cpu);
    first_framebuffer = ld32(MANAGER_C8);
    first_renderbuffer = ld32(MANAGER_CC);
    draw_query_ok = !cpu.fault && gl_pc_backend_installed() &&
                    current_draw_framebuffer(&cpu, &draw_framebuffer);
    ok = ok && returned_exactly(&cpu, stack_top) && (cpu.eax & 0xffu) == 1u &&
         kage_pc_backend_ready() && gl_pc_backend_installed() &&
         gl_pc_backend_complete() &&
         guest_kage_pc_handoff_ready() &&
         gl_pc_backend_resolved_count() + gl_pc_backend_missing_count() ==
             GUEST_GL_SURFACE_COUNT &&
         first_framebuffer != 0u && first_renderbuffer != 0u &&
         render_target_calls == 1u && render_target_abi_ok &&
         draw_query_ok && draw_framebuffer == 0u &&
         ld32(MANAGER_D8) == 0u &&
         kage_pc_backend_clear_count() == 1u &&
         kage_pc_backend_present_count() == 1u;
    printf("initialize ABI   : %s (%s)\n",
           returned_exactly(&cpu, stack_top) && kage_pc_backend_ready()
           ? "RET/AL exact" : "FAIL",
           kage_pc_backend_gl_version());

    prepare_call(&cpu, stack_top, 0xa0010008u);
    sub_00561830(&cpu);
    draw_framebuffer = 0xffffffffu;
    draw_query_ok = !cpu.fault && gl_pc_backend_installed() &&
                    current_draw_framebuffer(&cpu, &draw_framebuffer);
    ok = ok && returned_exactly(&cpu, stack_top) &&
         ld32(MANAGER_C8) != 0u && ld32(MANAGER_CC) != 0u &&
         render_target_calls == 2u &&
         render_target_saw_existing_renderbuffer == 1u &&
         render_target_abi_ok && draw_query_ok && draw_framebuffer == 0u &&
         kage_pc_backend_ready() && gl_pc_backend_installed() &&
         gl_pc_backend_complete();
    printf("FBO/RBO re-init  : %s (first=%u/%u current=%u/%u)\n",
           returned_exactly(&cpu, stack_top) && render_target_calls == 2u &&
           render_target_saw_existing_renderbuffer == 1u &&
           draw_query_ok && draw_framebuffer == 0u
           ? "RET4/force/current/lifecycle exact" : "FAIL",
           first_framebuffer, first_renderbuffer,
           ld32(MANAGER_C8), ld32(MANAGER_CC));

    refresh = kage_pc_backend_refresh_rate();
    policy = guest_kage_vsync_policy_allows(refresh);
    ok = ok && refresh > 1u &&
         guest_kage_vsync_policy_allows(58u) &&
         guest_kage_vsync_policy_allows(59u) &&
         guest_kage_vsync_policy_allows(60u) &&
         guest_kage_vsync_policy_allows(61u) &&
         guest_kage_vsync_policy_allows(62u) &&
         guest_kage_vsync_policy_allows(120u) &&
         !guest_kage_vsync_policy_allows(57u) &&
         !guest_kage_vsync_policy_allows(63u) &&
         !guest_kage_vsync_policy_allows(75u) &&
         !guest_kage_vsync_policy_allows(90u) &&
         !guest_kage_vsync_policy_allows(144u);
    changes = kage_pc_backend_vsync_change_count();
    st8(OPTIONS_VSYNC, 0xa5u);
    st8(KAGE_VSYNC_STATE, 0x5au);
    cpu.ebp = 0x11112222u;
    cpu.ebx = 0x33334444u;
    cpu.esi = 0x55556666u;
    cpu.edi = 0x77778888u;
    cpu.esp = stack_top;
    cpu.ecx = OPTIONS_CONFIG;
    gpush(&cpu, 1u);
    gpush(&cpu, 0xa0010013u);
    sub_00481280(&cpu);
    ok = ok && returned_exactly(&cpu, stack_top) &&
         ld8(OPTIONS_VSYNC) == 1u &&
         ld8(KAGE_VSYNC_STATE) == (uint8_t)policy &&
         kage_pc_backend_vsync_enabled() == policy &&
         kage_pc_backend_vsync_change_count() == changes + 1u &&
         cpu.ebp == 0x11112222u && cpu.ebx == 0x33334444u &&
         cpu.esi == 0x55556666u && cpu.edi == 0x77778888u;
    cpu.esp = stack_top;
    cpu.ecx = OPTIONS_CONFIG;
    gpush(&cpu, 1u);
    gpush(&cpu, 0xa0010014u);
    sub_00481280(&cpu);
    ok = ok && returned_exactly(&cpu, stack_top) &&
         ld8(OPTIONS_VSYNC) == 1u &&
         ld8(KAGE_VSYNC_STATE) == (uint8_t)policy &&
         kage_pc_backend_vsync_change_count() == changes + 1u;
    cpu.esp = stack_top;
    cpu.ecx = OPTIONS_CONFIG;
    gpush(&cpu, 0u);
    gpush(&cpu, 0xa0010015u);
    sub_00481280(&cpu);
    ok = ok && returned_exactly(&cpu, stack_top) &&
         ld8(OPTIONS_VSYNC) == 0u && ld8(KAGE_VSYNC_STATE) == 0u &&
         !kage_pc_backend_vsync_enabled() &&
         kage_pc_backend_vsync_change_count() ==
             changes + 1u + (policy ? 1u : 0u);
    printf("VSync ABI       : %s (%u Hz, policy=%d)\n",
           returned_exactly(&cpu, stack_top) &&
           ld8(OPTIONS_VSYNC) == 0u && ld8(KAGE_VSYNC_STATE) == 0u
           ? "RET4/request/policy/WGL exact" : "FAIL", refresh, policy);

    st8(SOUND_RUNNING, 0x5au);
    st8(SOUND_SUSPENDED, 0xa5u);
    st32(SOUND_CONTEXT, 0x11112222u);
    st32(SOUND_DEVICE, 0x33334444u);
    prepare_call(&cpu, stack_top, 0xa0010012u);
    cpu.ecx = SOUND_MANAGER;
    cpu.eax = 0x123456ffu;
    sub_0056dd70(&cpu);
    ok = ok && returned_exactly(&cpu, stack_top) &&
         cpu.eax == 0x12345600u && ld8(SOUND_RUNNING) == 0u &&
         ld8(SOUND_SUSPENDED) == 0u &&
         ld32(SOUND_CONTEXT) == 0u && ld32(SOUND_DEVICE) == 0u &&
         kage_pc_backend_ready() && gl_pc_backend_installed();
    printf("PC audio disable : %s\n",
           returned_exactly(&cpu, stack_top) &&
           ld8(SOUND_RUNNING) == 0u && ld8(SOUND_SUSPENDED) == 0u &&
           ld32(SOUND_CONTEXT) == 0u &&
           ld32(SOUND_DEVICE) == 0u ? "RET/AL/state exact" : "FAIL");

    st32(WIDTH_CACHE, 0x11111111u);
    st32(HEIGHT_CACHE, 0x22222222u);
    st32(WIDTH_COMPANION, 0x33333333u);
    st32(HEIGHT_COMPANION, 0x44444444u);
    prepare_call(&cpu, stack_top, 0xa0010004u);
    sub_00560f20(&cpu);
    ok = ok && returned_exactly(&cpu, stack_top) && cpu.eax == 73u &&
         ld32(WIDTH_CACHE) == 73u &&
         ld32(HEIGHT_COMPANION) == 41u &&
         ld32(HEIGHT_CACHE) == 0x22222222u &&
         ld32(WIDTH_COMPANION) == 0x33333333u;

    st32(WIDTH_CACHE, 0x55555555u);
    st32(HEIGHT_CACHE, 0x66666666u);
    st32(WIDTH_COMPANION, 0x77777777u);
    st32(HEIGHT_COMPANION, 0x88888888u);
    prepare_call(&cpu, stack_top, 0xa0010005u);
    sub_00560f90(&cpu);
    ok = ok && returned_exactly(&cpu, stack_top) && cpu.eax == 41u &&
         ld32(WIDTH_COMPANION) == 73u &&
         ld32(HEIGHT_CACHE) == 41u &&
         ld32(WIDTH_CACHE) == 0x55555555u &&
         ld32(HEIGHT_COMPANION) == 0x88888888u;
    printf("dimension ABI    : %s\n",
           returned_exactly(&cpu, stack_top) && cpu.eax == 41u &&
           ld32(WIDTH_COMPANION) == 73u && ld32(HEIGHT_CACHE) == 41u
           ? "73 x 41 + exact cache writes" : "FAIL");

    original_calls = inventory_calls;
    presents = kage_pc_backend_present_count();
    prepare_call(&cpu, stack_top, 0xa0010006u);
    sub_00560eb0(&cpu);
    ok = ok && returned_exactly(&cpu, stack_top) &&
         inventory_calls == original_calls + 1u &&
         kage_pc_backend_present_count() == presents;
    printf("present delegate : %s (setup presents=%u)\n",
           returned_exactly(&cpu, stack_top) &&
           inventory_calls == original_calls + 1u &&
           kage_pc_backend_present_count() == presents
           ? "original body owns leaf" : "FAIL",
           kage_pc_backend_present_count());

    prepare_call(&cpu, stack_top, 0xa0010007u);
    sub_00560e30(&cpu);
    ok = ok && returned_exactly(&cpu, stack_top) &&
         !kage_pc_backend_ready() && !gl_pc_backend_installed() &&
         !gl_pc_backend_complete() &&
         !guest_kage_pc_handoff_ready() &&
         ld32(MANAGER_C8) == 0u && ld32(MANAGER_CC) == 0u &&
         ld32(MANAGER_D8) == 0u;
    printf("shutdown ABI     : %s\n",
           returned_exactly(&cpu, stack_top) && !kage_pc_backend_ready() &&
           ld32(MANAGER_C8) == 0u && ld32(MANAGER_CC) == 0u
           ? "RET/delete/zero exact" : "FAIL");

    VirtualFree(stack, 0, MEM_RELEASE);
    VirtualFree(cache_page, 0, MEM_RELEASE);
    VirtualFree(sound_page, 0, MEM_RELEASE);
    VirtualFree(manager_page, 0, MEM_RELEASE);
    printf("VERDICT          : %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
