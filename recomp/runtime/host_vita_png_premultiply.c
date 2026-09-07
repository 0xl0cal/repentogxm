#include "guest.h"
#include "host_vita_heap.h"
#include "host_vita_texel_scratch.h"
#include "host_vita_png_premultiply.h"
#include "host_vita_png_premultiply_neon.h"
#include "kage_vita_deep_profile.h"
#include <errno.h>

#if !defined(ISAAC_VITA_PNG_PREMULTIPLY_NATIVE) || \
    !defined(ISAAC_VITA_HEAP_RANGE_LEASE)
#error PNG premultiply is an opt-in exact-object/scratch-owned seam
#endif

/* Frozen ImagePng 005a142c..005a1527. Tables and their full SHA256 hashes are
 * already documented by isaac_laser_p8_reference.h. Read the actual guest
 * tables; only the exhaustively equivalent LINEAR NEON option substitutes
 * integer arithmetic. The gamma table must not be approximated. */
enum {
    PREMUL_GAMMA_TABLE = 0x00626a40U,
    PREMUL_LINEAR_TABLE = 0x00636a40U,
    PREMUL_ENABLED = 0x007c7a4fU,
    PREMUL_GAMMA = 0x007c7a50U,
    PREMUL_TABLE_BYTES = 65536U,
    PREMUL_OBJECT_BYTES = 0x8cU,
    PREMUL_FRAME_LOW = 0x430U,
    PREMUL_FRAME_BYTES = 0x438U
};
static struct {
    uint32_t attempts, handled, stack, object, shape, scratch;
} s_premul;
void isaac_vita_log(const char *format, ...);

static uint32_t premul_word(uint32_t address)
{
    uint32_t value;
    memcpy(&value, (const void *)(uintptr_t)address, sizeof value);
    return value;
}

static uint16_t premul_half(uint32_t address)
{
    uint16_t value;
    memcpy(&value, (const void *)(uintptr_t)address, sizeof value);
    return value;
}

static void premul_count(uint32_t *value)
{
    if (*value != UINT32_MAX)
        ++*value;
}

static void premul_status(void)
{
    if (s_premul.attempts == 1u || (s_premul.attempts & 63u) == 0u)
        isaac_vita_log("KAGE VITA PNG PREMULTIPLY: attempt=%u handled=%u "
            "reject(stack,object,shape,scratch)=%u,%u,%u,%u "
            "policy=exact-scratch-prefix-final-row-guest",
            s_premul.attempts, s_premul.handled, s_premul.stack,
            s_premul.object, s_premul.shape, s_premul.scratch);
}

void isaac_vita_png_premultiply_rows(const IsaacVitaPngPremultiply *p)
{
    KAGE_VITA_DEEP_SCOPE_BYTES(KVD_PNG_PREMULTIPLY,
        (uint64_t)p->rows * p->width * 4u);
    uint32_t y, x;
#if ISAAC_PNG_PREMULTIPLY_NEON_AVAILABLE && ISAAC_VITA_PNG_PREMULTIPLY_LINEAR_NEON
    /* This is the pinned PE's immutable .rdata table, selected by the
     * existing guest guard. Other tables/aliases keep the original lookups. */
    const int linear_table = p->table == (const uint8_t *)(uintptr_t)
        (GUEST_IMAGE_BASE + PREMUL_LINEAR_TABLE);
#endif
    for (y = 0u; y < p->rows; ++y) {
        uint8_t *pixel = (uint8_t *)(uintptr_t)p->base + (size_t)y * p->stride;
#if ISAAC_PNG_PREMULTIPLY_NEON_AVAILABLE
        uint32_t scalar_until = 0U;
#endif
        for (x = 0u; x < p->width; ++x, pixel += 4) {
#if ISAAC_PNG_PREMULTIPLY_NEON_AVAILABLE
            /* Classify once per complete fixed 16-pixel block, not a
             * sliding window after each mixed pixel. Tails stay scalar. */
            if (x >= scalar_until && p->width - x >= 16U) {
                scalar_until = x + 16U;
                if (isaac_png_premultiply_uniform16(pixel)) {
                    x += 15U;
                    pixel += 60U;
                    continue;
                }
#if ISAAC_VITA_PNG_PREMULTIPLY_LINEAR_NEON
                if (linear_table) {
                    isaac_png_premultiply_linear16(pixel);
                    x += 15U;
                    pixel += 60U;
                    continue;
                }
#endif
            }
#endif
            const uint32_t alpha = pixel[3];
            if (alpha == 255u)
                continue;
            if (alpha == 0u) {
                pixel[0] = pixel[1] = pixel[2] = 0u;
            } else {
                const uint32_t index = alpha << 8;
                const uint32_t r = pixel[0], g = pixel[1], b = pixel[2];
                pixel[0] = p->table[index + r];
                pixel[1] = p->table[index + g];
                pixel[2] = p->table[index + b];
            }
        }
    }
}

/* Called once, after 005a145a and before the existing loop-head label.
 * Success advances only the row index/local to the final row. Its original
 * instructions recreate all final registers, flags and locals. Zero means
 * no guest CPU/memory changes; a failed lease release is terminal, not retry. */
int isaac_vita_png_premultiply_guest_try(CPU *__restrict c)
{
    const int saved_errno = errno;
    uint32_t object, frame, buffer, token = 0u, width, height, padded_width;
    uint32_t padded_height, ret, table_address, result = 0u;
    uint64_t bytes, buffer_end;
    IsaacVitaPngPremultiply p;
    premul_count(&s_premul.attempts);
    if (!c || c->fault || c->ebp < PREMUL_FRAME_LOW ||
        !guest_stack_contains(c, c->ebp - PREMUL_FRAME_LOW, PREMUL_FRAME_BYTES) ||
        !guest_stack_contains(c, c->esp, 4u)) {
        premul_count(&s_premul.stack);
        goto done;
    }
    frame = c->ebp;
    object = c->edi;
    ret = premul_word(frame + 4u);
    if ((ret != 0x005a0c7eU && ret != 0x005a0d62U) ||
        object > UINT32_MAX - PREMUL_OBJECT_BYTES || c->ecx != 0u ||
        c->eax != 0u || c->esi != object + 0x80u ||
        premul_word(frame - 0x430u) != object ||
        premul_word(frame - 0x428u) != object + 0x80u ||
        premul_word(frame - 0x410u) != 0u) {
        premul_count(&s_premul.stack);
        goto done;
    }
    token = isaac_vita_guest_heap_lease_exact_range(
        (const void *)(uintptr_t)object, (const void *)(uintptr_t)object,
        PREMUL_OBJECT_BYTES);
    if (!token) {
        premul_count(&s_premul.object);
        goto done;
    }
    buffer = premul_word(frame - 0x424u);
    width = premul_half(object + 0x80u);
    height = premul_half(object + 0x82u);
    padded_width = premul_half(object + 0x84u);
    padded_height = premul_half(object + 0x86u);
    bytes = (uint64_t)padded_width * padded_height * 4u;
    buffer_end = (uint64_t)buffer + bytes;
    if (!width || height < 2u || width > padded_width ||
        height > padded_height || !bytes || bytes > UINT32_MAX ||
        !buffer || buffer_end > UINT32_MAX ||
        ((uint64_t)buffer < (uint64_t)object + PREMUL_OBJECT_BYTES &&
         (uint64_t)object < buffer_end) ||
        ((uint64_t)buffer < c->stack_ceiling && c->stack_floor < buffer_end) ||
        premul_word(object + 0x18u) != 2u ||
        !guest_image_contains(GUEST_IMAGE_BASE + PREMUL_ENABLED, 2u) ||
        !ld8(GUEST_IMAGE_BASE + PREMUL_ENABLED)) {
        premul_count(&s_premul.shape);
        goto release;
    }
    table_address = GUEST_IMAGE_BASE + (ld8(GUEST_IMAGE_BASE + PREMUL_GAMMA)
        ? PREMUL_GAMMA_TABLE : PREMUL_LINEAR_TABLE);
    if (!guest_image_contains(table_address, PREMUL_TABLE_BYTES) ||
        ((uint64_t)buffer < (uint64_t)table_address + PREMUL_TABLE_BYTES &&
         table_address < buffer_end)) {
        premul_count(&s_premul.shape);
        goto release;
    }
    p.base = buffer; p.bytes = (uint32_t)bytes; p.width = width;
    p.rows = height - 1u; p.stride = padded_width * 4u;
    p.table = (const uint8_t *)(uintptr_t)table_address;
    if (!isaac_vita_texel_scratch_png_premultiply(&p)) {
        premul_count(&s_premul.scratch);
        goto release;
    }
    result = 1u;
release:
    if (!isaac_vita_guest_heap_lease_release(token)) {
        errno = saved_errno; /* guest_fault may leave through a longjmp. */
        guest_fault(c, object, "PNG premultiply object lease release failed");
        premul_status();
        errno = saved_errno;
        return -1;
    }
    if (result) {
        c->ecx = height - 1u;
        st32(frame - 0x410u, c->ecx);
        premul_count(&s_premul.handled);
    }
done:
    premul_status();
    errno = saved_errno;
    return (int)result;
}
