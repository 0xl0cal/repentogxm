/* Exact native replay of frozen Shader::SetUniformInt (RVA 00567430).
 * Original bytes/CFG/vtable/GL slots are pinned by the executed differential
 * test. No location/value/texture/animation cache, shader or ordering change.
 * Only the four dynamic GL adapter crossings and translated stack glue go.
 * Copying native GL callbacks is allowed only after all four were proved to
 * be the actual Vita wrappers; arbitrary/partial backends take the original. */
#include "guest.h"
#include "gl_bridge.h"
#include "gl_vita_backend.h"

#if !defined(ISAAC_VITA_SHADER_SAMPLER_FASTPATH) || !ISAAC_VITA_SHADER_SAMPLER_FASTPATH
#error Compile the sampler replay only when explicitly enabled
#endif
#if GUEST_STACK_REQUIRED != 1
#error Sampler replay requires the bound synthetic stack
#endif

void __real_sub_00567430(CPU *__restrict c);

/* Original VA minus 00400000, copied from the frozen decoder output. */
#define SAMPLER_VTABLE (GUEST_IMAGE_BASE + 0x0075e0a8u)
#define SAMPLER_ACTIVE_SLOT (GUEST_IMAGE_BASE + 0x007c293cu)
#define SAMPLER_BIND_SLOT (GUEST_IMAGE_BASE + 0x007c2978u)
#define SAMPLER_GET_SLOT (GUEST_IMAGE_BASE + 0x007bf9bcu)
#define SAMPLER_INT_SLOT (GUEST_IMAGE_BASE + 0x007bf96cu)

static uint32_t s_sampler_tokens[4];
static int sampler_tokens_ready(void)
{
    if (s_sampler_tokens[3]) return 1;
    s_sampler_tokens[0] = guest_gl_resolve("glActiveTexture");
    s_sampler_tokens[1] = guest_gl_resolve("glBindTexture");
    s_sampler_tokens[2] = guest_gl_resolve("glGetUniformLocation");
    if (!s_sampler_tokens[0] || !s_sampler_tokens[1] || !s_sampler_tokens[2]) return 0;
    s_sampler_tokens[3] = guest_gl_resolve("glUniform1i");
    return s_sampler_tokens[3] != 0u;
}

static int sampler_not_stack(const CPU *c, uint32_t address, uint32_t bytes)
{
    return address && address <= UINT32_MAX - bytes &&
        (address + bytes <= c->stack_floor || address >= c->stack_ceiling);
}

static int sampler_try(CPU *__restrict c)
{
    const guest_gl_backend *backend;
    uint32_t self, unit, image, name, image_kind, texture;
    uint32_t capacity, offset;
    guest_gl_int location;
    if (!c || !guest_stack_fast_bound(c)) return 0;
    capacity = c->stack_ceiling - c->stack_floor;
    offset = c->esp - c->stack_floor;
    /* 3 saved words + pre-pushed Uniform1i unit + GetUniformLocation's
     * program/name/return words: deepest generated ESP is entry-28. */
    if (capacity < 16u || offset > capacity - 16u || offset < 28u) return 0;
    /* The runtime stack is a separate memblock. A custom image-backed stack
     * could alias the shader/name when original pushes write into it. */
    if (!(c->stack_ceiling <= GUEST_IMAGE_BASE ||
          c->stack_floor >= GUEST_IMAGE_BASE + 0x0085f000u)) return 0;
    self = c->ecx;
    /* The shader/name pointers accepted by this fast path are image-owned,
     * not aliasing the caller's/deepest synthetic stack. Other shapes remain
     * valid inputs to the unchanged translated fallback, not new errors. */
    /* SizeOfImage=0085f000, decoded from the SHA-pinned owned PE. */
    if (self < GUEST_IMAGE_BASE || self - GUEST_IMAGE_BASE > 0x0085f000u - 44u ||
        ld32(self) != SAMPLER_VTABLE) return 0;
    name = ld32(c->esp + 4u);
    image = ld32(c->esp + 12u);
    if (name < GUEST_IMAGE_BASE || name - GUEST_IMAGE_BASE >= 0x0085f000u ||
        !sampler_not_stack(c, image, 128u)) return 0;
    if (!sampler_tokens_ready() || !guest_gl_dynamic_first_holds() ||
        ld32(SAMPLER_ACTIVE_SLOT) != s_sampler_tokens[0] ||
        ld32(SAMPLER_BIND_SLOT) != s_sampler_tokens[1] ||
        ld32(SAMPLER_GET_SLOT) != s_sampler_tokens[2] ||
        ld32(SAMPLER_INT_SLOT) != s_sampler_tokens[3]) return 0;
    backend = guest_gl_installed_backend();
    if (!gl_vita_backend_sampler_is_native(backend) || !guest_gl_owned_enter(c)) return 0;
    unit = ld32(c->esp + 8u); /* ESI is captured before the first original call. */
    backend->glActiveTexture((guest_gl_enum)(unit + 0x84c0u));
    /* The original reloads image AFTER ActiveTexture. Never hoist these reads
     * or assume a nonstandard unit must bind Texture0. Image capability==2,
     * unit 1 selects +0x7c; other nonzero/nonone units intentionally do NOT bind. */
    image = ld32(c->esp + 12u);
    image_kind = ld32(image + 0x20u);
    if (image_kind != 2u || unit == 0u) {
        texture = ld32(image + 0x78u);
        backend->glBindTexture(0xde1u, texture);
    } else if (unit == 1u) {
        texture = ld32(image + 0x7cu);
        backend->glBindTexture(0xde1u, texture);
    }
    /* Program/name reads are likewise at the original post-bind boundary. */
    name = ld32(c->esp + 4u);
    location = backend->glGetUniformLocation(ld32(self + 0x28u), name);
    backend->glUniform1i(location, (guest_gl_int)unit);
    guest_gl_owned_leave(c, s_sampler_tokens[3]);
#if GUEST_GENERATED_STACK_GUARD
    guest_stack_note_low(c, c->esp - 28u); /* checked translated pushes */
#else
    /* Raw generated pushes do not note their return word; the adapters'
     * first GetUniformLocation argument is the lowest checked word. */
    guest_stack_note_low(c, c->esp - 24u);
#endif
    GUEST_PROFILE_NOTE_FUNCTION_ENTRY();
    c->eax = (uint32_t)location;
    c->esp += 16u; /* pop saved EDI/ESI/EBP; ret 0xc: net entry ESP +16. */
    return 1;
}

void __wrap_sub_00567430(CPU *__restrict c)
{
    if (!sampler_try(c)) __real_sub_00567430(c);
}
