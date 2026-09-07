#include "guest.h"
#include "host_vita_heap.h"

#if (!defined(ISAAC_VITA_LIGHT_SURFACE_RASTER_416) || \
     !ISAAC_VITA_LIGHT_SURFACE_RASTER_416) && \
    (!defined(ISAAC_VITA_RENDER_SURFACE_RASTER_432) || \
     !ISAAC_VITA_RENDER_SURFACE_RASTER_432)
#error The surface raster wrapper is an opt-in runtime object
#endif
#if !defined(ISAAC_VITA_HEAP_RANGE_LEASE)
#error The surface result mutation requires exact heap leases
#endif

_Static_assert(GUEST_IMAGE_BASE == 0x98000000U,
               "Light surface seam requires the frozen relocated image");

void __real_sub_0056cf30(CPU *__restrict c);
void isaac_vita_log(const char *format, ...);

static uint32_t light_word(uint32_t address)
{
    uint32_t result;
    memcpy(&result, (const void *)(uintptr_t)address, sizeof result);
    return result;
}

static int light_image_word_is(uint32_t rva, uint32_t expected)
{
    uint32_t address = GUEST_IMAGE_BASE + rva;
    return guest_image_contains(address, 4U) &&
           light_word(address) == expected;
}

#if defined(ISAAC_VITA_LIGHT_SURFACE_RASTER_416) && \
    ISAAC_VITA_LIGHT_SURFACE_RASTER_416
/* Frozen PE SHA256:
 * 31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404.
 * Exact cross-TU call: guest_0108 003a92d8 -> guest_0168 0056cf30.
 * Light binds with screen-space flag 1 (003ca717), so 00565240 keeps the
 * world projection. Its composites 003ccdd8/003cd22f/003e120f use explicit
 * full-size quads. Shadow binds with flag 0 and is deliberately excluded.
 * The factory keeps its original 480x270 arguments and 480x272 backing.
 * Only the fresh result's logical extent and matching UV ratios change,
 * before 003a92ed publishes retained Light. Keeping backing dimensions equal
 * to Shadow avoids stock shared.h's size-change RT/depth retirement path.
 * Native allocation, texture, stride, GL state and lifetime are unchanged.
 */
enum {
    LIGHT_RETURN = 0x003a92ddU,
    LIGHT_STACK_BYTES = 48U,
    LIGHT_NAME_RVA = 0x0074d750U,
    LIGHT_RETAINED_RVA = 0x007fed64U,
    LIGHT_ALIGN8_RVA = 0x007fd66aU,
    LIGHT_VTABLE_RVA = 0x00766258U,
    LIGHT_OBJECT_BYTES = 0x90U,
    LIGHT_WIDTH = 416U,
    LIGHT_HEIGHT = 234U,
    LIGHT_ORIGINAL_WIDTH = 480U,
    LIGHT_ORIGINAL_HEIGHT = 270U,
    LIGHT_BACKING_HEIGHT = 272U,
    /* IEEE binary32 divisions, matching 005b60c2/005b6103: original
     * 480/480 and 270/272, then reduced 416/480 and 234/272. */
    LIGHT_ORIGINAL_U = 0x3f800000U,
    LIGHT_ORIGINAL_V = 0x3f7e1e1eU,
    LIGHT_U = 0x3f5ddddeU,
    LIGHT_V = 0x3f5c3c3cU
};

static const unsigned char s_light_name[] = "Light Overlay Surface";
static unsigned s_light_result_logged;
static unsigned s_light_not_applied_logged;

static void light_not_applied(const char *reason)
{
    if (!s_light_not_applied_logged) {
        s_light_not_applied_logged = 1U;
        isaac_vita_log("KAGE Light surface raster: not-applied reason=%s", reason);
    }
}

static int light_admit(const CPU *c, uint32_t *output)
{
    uint32_t stack, result;
    unsigned i;
    const uint32_t name = GUEST_IMAGE_BASE + LIGHT_NAME_RVA;
    const uint32_t align8 = GUEST_IMAGE_BASE + LIGHT_ALIGN8_RVA;

    if (!c || c->fault ||
        !guest_stack_contains(c, c->esp, LIGHT_STACK_BYTES))
        return 0;
    stack = c->esp;
    if (light_word(stack) != LIGHT_RETURN ||
        light_word(stack + 8U) != 480U ||
        light_word(stack + 12U) != 270U ||
        light_word(stack + 16U) != 480U ||
        light_word(stack + 20U) != name ||
        !guest_image_contains(name, sizeof s_light_name) ||
        memcmp((const void *)(uintptr_t)name, s_light_name,
               sizeof s_light_name) != 0 ||
        !guest_image_contains(align8, 1U) ||
        *(const uint8_t *)(uintptr_t)align8 != 1U ||
        !light_image_word_is(LIGHT_RETAINED_RVA, 0U) ||
        /* Exact current float bits, not a float-to-int approximation. */
        !light_image_word_is(0x007fec08U, 0x43f00000U) ||
        !light_image_word_is(0x007fec68U, 0x43870000U) ||
        !light_image_word_is(0x007aa92cU, 0x43f00000U) ||
        !light_image_word_is(0x007aa930U, 0x43870000U))
        return 0;
    for (i = 0U; i < 4U; ++i)
        if (light_word(stack + 32U + 4U * i) != 0U)
            return 0;
    result = light_word(stack + 4U);
    if (!guest_stack_contains(c, result, 8U) ||
        ((uint64_t)result < (uint64_t)stack + LIGHT_STACK_BYTES &&
         (uint64_t)stack < (uint64_t)result + 8U))
        return 0;
    *output = result;
    return 1;
}

static void light_apply_result(CPU *c, uint32_t stack, uint32_t output)
{
    uint32_t object, token, texture, width, height, padded_width, padded_height;
    const uint32_t new_width = LIGHT_WIDTH, new_height = LIGHT_HEIGHT;
    const uint32_t new_u = LIGHT_U, new_v = LIGHT_V;
    int matched;

    /* The factory returns the output shared-pointer address in EAX and
     * consumes 44 argument bytes. Its caller still owns that output slot.
     * A rejected result remains completely original and is never replayed.
     * Recheck and apply on EVERY fresh creation; only the log is one-time.
     */
    if (c->fault || c->esp != stack + LIGHT_STACK_BYTES || c->eax != output ||
        !guest_stack_contains(c, output, 8U)) {
        light_not_applied("factory-exit");
        return;
    }
    object = light_word(output);
    if (!object || object > UINT32_MAX - LIGHT_OBJECT_BYTES) {
        light_not_applied("result-pointer");
        return;
    }
    token = isaac_vita_guest_heap_lease_exact_range(
        (const void *)(uintptr_t)object, (const void *)(uintptr_t)object,
        LIGHT_OBJECT_BYTES);
    if (!token) {
        light_not_applied("result-ownership");
        return;
    }
    texture = light_word(object + 0x78U);
    width = light_word(object + 0x80U);
    height = light_word(object + 0x84U);
    padded_width = light_word(object + 0x88U);
    padded_height = light_word(object + 0x8cU);
    matched = light_word(object) == GUEST_IMAGE_BASE + LIGHT_VTABLE_RVA &&
        texture != 0U && width == LIGHT_ORIGINAL_WIDTH &&
        height == LIGHT_ORIGINAL_HEIGHT &&
        padded_width == LIGHT_ORIGINAL_WIDTH && padded_height == LIGHT_BACKING_HEIGHT &&
        light_word(object + 8U) == LIGHT_ORIGINAL_U &&
        light_word(object + 12U) == LIGHT_ORIGINAL_V;
    if (matched) {
        /* All four stores are inside this exact live allocation lease.
         * The native texture/storage words +0x78/+0x88/+0x8c do not change.
         * 00564db0 queries +0x80/+0x84 for the viewport; 0055f990 samples
         * +8/+0xc over the unchanged explicit full-screen destination quad.
         */
        memcpy((void *)(uintptr_t)(object + 8U), &new_u, sizeof new_u);
        memcpy((void *)(uintptr_t)(object + 12U), &new_v, sizeof new_v);
        memcpy((void *)(uintptr_t)(object + 0x80U), &new_width, sizeof new_width);
        memcpy((void *)(uintptr_t)(object + 0x84U), &new_height, sizeof new_height);
    }
    if (!isaac_vita_guest_heap_lease_release(token)) {
        if (matched)
            isaac_vita_log("KAGE Light surface raster: applied-but-lease-release-failed");
        else
            light_not_applied("lease-release");
        guest_fault(c, object, "Light surface raster result lease release failed");
        return;
    }
    if (!matched) {
        light_not_applied("result-layout");
        return;
    }
    if (s_light_result_logged)
        return;
    s_light_result_logged = 1U;
    isaac_vita_log(
        "KAGE Light surface raster: applied object=0x%08x texture=%u "
        "logical=%ux%u backing=%ux%u original=480x270 scale=13/15 storage=unchanged",
        (unsigned)object, (unsigned)texture, (unsigned)new_width, (unsigned)new_height,
        (unsigned)padded_width, (unsigned)padded_height);
}
#endif

#if defined(ISAAC_VITA_RENDER_SURFACE_RASTER_432) && \
    ISAAC_VITA_RENDER_SURFACE_RASTER_432
/* Same frozen PE as Light, but a distinct exact cross-TU factory owner:
 * guest_0143 004acba8 -> guest_0168 0056cf30, before 004acbbd publishes
 * [987fed7c]. Never classify by dimensions or the texture name alone.
 *
 * Manager::Render binds this image with flag1 at 004b070b, preserving the
 * virtual 480x270 projection. It restores default at 004b0f25 and samples
 * this image into an explicit full-size quad at 004b100c. Game HUD is drawn
 * BEFORE that composite (002cef99): this experiment reduces its raster too.
 * Physical storage, clear extent, alpha/blend, scanout and auxiliary surfaces
 * stay original. Even 432x242 avoids native viewport's odd-size truncation.
 */
enum {
    RENDER_RETURN = 0x004acbadU,
    RENDER_STACK_BYTES = 48U,
    RENDER_NAME_RVA = 0x0075882cU,
    RENDER_RETAINED_RVA = 0x007fed7cU,
    RENDER_ALIGN8_RVA = 0x007fd66aU,
    RENDER_VTABLE_RVA = 0x00766258U,
    RENDER_OBJECT_BYTES = 0x90U,
    RENDER_WIDTH = 432U,
    RENDER_HEIGHT = 242U,
    RENDER_ORIGINAL_WIDTH = 480U,
    RENDER_ORIGINAL_HEIGHT = 270U,
    RENDER_BACKING_HEIGHT = 272U,
    /* Binary32 of the constructor's logical/physical divisions. */
    RENDER_ORIGINAL_U = 0x3f800000U,
    RENDER_ORIGINAL_V = 0x3f7e1e1eU,
    RENDER_U = 0x3f666666U,
    RENDER_V = 0x3f63c3c4U
};

/* Exact NUL-terminated bytes at frozen PE RVA 0075882c, pushed by
 * 004acb8f. "Render Surface" alone mismatches the space before "(Manager)"
 * and rejects every real creation even when a self-derived fixture passes. */
static const unsigned char s_render_name[] = "Render Surface (Manager)";
static unsigned s_render_result_logged;
static unsigned s_render_not_applied_logged;

static void render_not_applied(const char *reason)
{
    if (!s_render_not_applied_logged) {
        s_render_not_applied_logged = 1U;
        isaac_vita_log("KAGE Render surface raster: not-applied reason=%s", reason);
    }
}

static int render_globals_match(void)
{
    const uint32_t align8 = GUEST_IMAGE_BASE + RENDER_ALIGN8_RVA;
    return guest_image_contains(align8, 1U) &&
        *(const uint8_t *)(uintptr_t)align8 == 1U &&
        light_image_word_is(RENDER_RETAINED_RVA, 0U) &&
        light_image_word_is(0x007fec6cU, 480U) &&
        light_image_word_is(0x007fec0cU, 270U) &&
        light_image_word_is(0x007fec08U, 0x43f00000U) &&
        light_image_word_is(0x007fec68U, 0x43870000U) &&
        light_image_word_is(0x007aa92cU, 0x43f00000U) &&
        light_image_word_is(0x007aa930U, 0x43870000U);
}

static int render_admit(const CPU *c, uint32_t *output)
{
    uint32_t stack, result;
    unsigned i;
    const uint32_t name = GUEST_IMAGE_BASE + RENDER_NAME_RVA;

    if (!c || c->fault || !guest_stack_contains(c, c->esp, 4U) ||
        light_word(c->esp) != RENDER_RETURN)
        return 0; /* Unrelated factory calls are deliberately silent. */
    stack = c->esp;
    if (!guest_stack_contains(c, stack, RENDER_STACK_BYTES) || c->ebp < 48U) {
        render_not_applied("entry-stack");
        return 0;
    }
    /* 004acb7b + the leaf 00560b80 preserve ECX=EBP-0x30. The pushed
     * +16 word is that scratch address, NOT Light's extra width. +24/+28
     * are uninitialized padding, never read by this factory; leave alone. */
    if (light_word(stack + 8U) != RENDER_ORIGINAL_WIDTH ||
        light_word(stack + 12U) != RENDER_ORIGINAL_HEIGHT ||
        c->ecx != c->ebp - 48U || light_word(stack + 16U) != c->ecx ||
        !guest_stack_contains(c, c->ecx, 16U) ||
        light_word(stack + 20U) != name ||
        !guest_image_contains(name, sizeof s_render_name) ||
        memcmp((const void *)(uintptr_t)name, s_render_name,
               sizeof s_render_name) != 0 || !render_globals_match()) {
        render_not_applied("entry-contract");
        return 0;
    }
    for (i = 0U; i < 4U; ++i) {
        if (light_word(stack + 32U + 4U * i) != 0U) {
            render_not_applied("entry-clear");
            return 0;
        }
    }
    result = light_word(stack + 4U);
    if (result != c->ebp - 28U || !guest_stack_contains(c, result, 8U) ||
        ((uint64_t)result < (uint64_t)stack + RENDER_STACK_BYTES &&
         (uint64_t)stack < (uint64_t)result + 8U)) {
        render_not_applied("entry-output");
        return 0;
    }
    *output = result;
    return 1;
}

static void render_apply_result(CPU *c, uint32_t stack, uint32_t output)
{
    uint32_t object, token, texture;
    const uint32_t new_width = RENDER_WIDTH, new_height = RENDER_HEIGHT;
    const uint32_t new_u = RENDER_U, new_v = RENDER_V;
    int matched;

    if (c->fault || c->esp != stack + RENDER_STACK_BYTES || c->eax != output ||
        c->ebp != output + 28U || !guest_stack_contains(c, output, 8U)) {
        render_not_applied("factory-exit");
        return;
    }
    /* Also reject a changed owner/global context across factory callbacks. */
    if (!render_globals_match()) {
        render_not_applied("factory-context");
        return;
    }
    object = light_word(output);
    if (!object || object > UINT32_MAX - RENDER_OBJECT_BYTES) {
        render_not_applied("result-pointer");
        return;
    }
    token = isaac_vita_guest_heap_lease_exact_range(
        (const void *)(uintptr_t)object, (const void *)(uintptr_t)object,
        RENDER_OBJECT_BYTES);
    if (!token) {
        render_not_applied("result-ownership");
        return;
    }
    texture = light_word(object + 0x78U);
    matched = light_word(object) == GUEST_IMAGE_BASE + RENDER_VTABLE_RVA &&
        texture != 0U && light_word(object + 0x80U) == RENDER_ORIGINAL_WIDTH &&
        light_word(object + 0x84U) == RENDER_ORIGINAL_HEIGHT &&
        light_word(object + 0x88U) == RENDER_ORIGINAL_WIDTH &&
        light_word(object + 0x8cU) == RENDER_BACKING_HEIGHT &&
        light_word(object + 8U) == RENDER_ORIGINAL_U &&
        light_word(object + 12U) == RENDER_ORIGINAL_V;
    if (matched) {
        memcpy((void *)(uintptr_t)(object + 8U), &new_u, sizeof new_u);
        memcpy((void *)(uintptr_t)(object + 12U), &new_v, sizeof new_v);
        memcpy((void *)(uintptr_t)(object + 0x80U), &new_width, sizeof new_width);
        memcpy((void *)(uintptr_t)(object + 0x84U), &new_height, sizeof new_height);
    }
    if (!isaac_vita_guest_heap_lease_release(token)) {
        if (matched)
            isaac_vita_log("KAGE Render surface raster: applied-but-lease-release-failed");
        else
            render_not_applied("lease-release");
        guest_fault(c, object, "Render surface raster result lease release failed");
        return;
    }
    if (!matched) {
        render_not_applied("result-layout");
        return;
    }
    if (!s_render_result_logged) {
        s_render_result_logged = 1U;
        isaac_vita_log(
            "KAGE Render surface raster: applied object=0x%08x texture=%u "
            "logical=432x242 backing=480x272 original=480x270 "
            "storage=unchanged hud=raster-scaled",
            (unsigned)object, (unsigned)texture);
    }
}
#endif

void __wrap_sub_0056cf30(CPU *__restrict c)
{
    uint32_t output;
    uint32_t stack;

#if defined(ISAAC_VITA_LIGHT_SURFACE_RASTER_416) && \
    ISAAC_VITA_LIGHT_SURFACE_RASTER_416
    if (light_admit(c, &output)) {
        stack = c->esp;
        __real_sub_0056cf30(c);
        light_apply_result(c, stack, output);
        return;
    }
#endif
#if defined(ISAAC_VITA_RENDER_SURFACE_RASTER_432) && \
    ISAAC_VITA_RENDER_SURFACE_RASTER_432
    if (render_admit(c, &output)) {
        stack = c->esp;
        __real_sub_0056cf30(c);
        render_apply_result(c, stack, output);
        return;
    }
#endif
    /* Do not restore consumed arguments in either path: the real factory
     * reuses them as locals. Every admitted recreation is processed. */
    __real_sub_0056cf30(c);
}
