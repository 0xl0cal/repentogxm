#include "host_vita_png_texel_init.h"
#include "host_vita_heap.h"
#include "host_vita_native_png.h"
#include "host_vita_texel_scratch.h"

#include <errno.h>

#if !ISAAC_VITA_PNG_TEXEL_INIT_ELISION || \
    !defined(ISAAC_VITA_HEAP_RANGE_LEASE)
#error PNG texel init elision requires its opt-in and exact heap leases
#endif

static IsaacVitaPngTexelInitSnapshot s_init;

static void png_init_inc(uint32_t *value)
{
    if (*value == UINT32_MAX)
        s_init.saturated = 1U;
    else
        ++*value;
}

void isaac_vita_png_texel_init_snapshot(IsaacVitaPngTexelInitSnapshot *out)
{
    if (out)
        *out = s_init;
}

/* Copied from frozen ImagePng 005a0e10: caller/local relations and the
 * 005a12de..005a12e2 memset(B,0,padded_width*padded_height*channels).
 * png/info sizes are the original malloc requests, not native C layouts.
 * No premultiply locals are modified. */
enum {
    PNG_CALLER_STREAM = 0x005a0c7eU,
    PNG_CALLER_FILE = 0x005a0d62U,
    PNG_OBJECT_VTABLE = 0x00766028U,
    PNG_OBJECT_BYTES = 0x8cU,
    PNG_STRUCT_BYTES = 0x20cU,
    PNG_INFO_BYTES = 0xb8U,
    PNG_FRAME_LOW = 0x430U,
    PNG_FRAME_BYTES = 0x438U,
    /* 005a0e13: sub esp,0x43c; saved ebx/esi/edi at 005a0e26/3c/3d;
     * 005a12de/e1: three arguments, 005a12e2: call return. The allocator
     * wrapper restores esp and ret 8 at 0059a5f3/f6 removes its arguments. */
    PNG_CLEAR_ESP_DELTA = 0x43cU + 12U + 12U + 4U,
    PNG_INFO_WIDTH = 0x00U,
    PNG_INFO_HEIGHT = 0x04U,
    PNG_INFO_ROWBYTES = 0x0cU,
    PNG_INFO_DEPTH = 0x18U,
    PNG_INFO_COLOR = 0x19U,
    PNG_ERROR_FN = 0x40U,
    PNG_WARNING_FN = 0x44U,
    PNG_ERROR_PTR = 0x48U
};

static uint32_t png_init_word(uint32_t address)
{
    uint32_t value;
    memcpy(&value, (const void *)(uintptr_t)address, sizeof value);
    return value;
}

static uint16_t png_init_half(uint32_t address)
{
    uint16_t value;
    memcpy(&value, (const void *)(uintptr_t)address, sizeof value);
    return value;
}

int isaac_vita_png_texel_init_try(CPU *__restrict c, uint32_t destination,
                                int value, uint32_t count)
{
    const int saved_errno = errno;
    uint32_t object, png, info, frame, ret, width, height, rowbytes, flags, mode;
    uint32_t tokens[3] = {0U, 0U, 0U};
    uint32_t bases[3];
    static const uint32_t sizes[3] = {
        PNG_OBJECT_BYTES, PNG_STRUCT_BYTES, PNG_INFO_BYTES
    };
    uint32_t i, failed_release = 0U;
    int result = 0;

    png_init_inc(&s_init.attempts);

    if (!c || c->fault || value != 0 || !count || !destination ||
        !guest_stack_contains(c, c->esp, 16U) ||
        png_init_word(c->esp) != ISAAC_VITA_PNG_TEXEL_INIT_RETURN_RVA ||
        c->ebp < PNG_CLEAR_ESP_DELTA ||
        c->esp != c->ebp - PNG_CLEAR_ESP_DELTA ||
        !guest_stack_contains(c, c->ebp - PNG_FRAME_LOW, PNG_FRAME_BYTES))
        goto done;
    frame = c->ebp;
    ret = png_init_word(frame + 4U);
    object = c->esi;
    png = c->ebx;
    info = png_init_word(frame - 0x414U);
    if ((ret != PNG_CALLER_STREAM && ret != PNG_CALLER_FILE) ||
        png_init_word(frame - 0x430U) != object ||
        png_init_word(frame - 0x418U) != png ||
        png_init_word(frame - 0x424U) != destination ||
        png_init_word(frame - 0x410U) != 4U ||
        c->edi != count || c->eax != destination)
        goto done;
    bases[0] = object;
    bases[1] = png;
    bases[2] = info;
    for (i = 0U; i < 3U; ++i) {
        if (!bases[i] || bases[i] > UINT32_MAX - sizes[i])
            goto release;
        tokens[i] = isaac_vita_guest_heap_lease_exact_range(
            (const void *)(uintptr_t)bases[i],
            (const void *)(uintptr_t)bases[i], sizes[i]);
        if (!tokens[i])
            goto release;
    }
    width = png_init_half(object + 0x80U);
    height = png_init_half(object + 0x82U);
    rowbytes = width * 4U;
    flags = png_init_word(png + ISAAC_NP_PNG_FLAGS);
    mode = png_init_word(png + ISAAC_NP_PNG_MODE);
    if (png_init_word(object) != GUEST_IMAGE_BASE + PNG_OBJECT_VTABLE ||
        !width || !height || width > ISAAC_NP_MAX_DIMENSION ||
        height > ISAAC_NP_MAX_DIMENSION ||
        width != png_init_half(object + 0x84U) ||
        height != png_init_half(object + 0x86U) ||
        /* Dword store at 005a1143, before read_update_info. */
        png_init_word(object + 0x88U) != 4U ||
        (uint64_t)rowbytes * height != count ||
        !(flags & ISAAC_NP_FLAG_ROW_INIT) ||
        (flags & (ISAAC_NP_FLAG_ZLIB_FINISHED | ISAAC_NP_FLAG_CRC_MASK)) ||
        !(mode & ISAAC_NP_MODE_HAVE_IDAT) ||
        (mode & (ISAAC_NP_MODE_AFTER_IDAT | ISAAC_NP_MODE_HAVE_IEND)) ||
        ld8(png + ISAAC_NP_PNG_INTERLACED) ||
        ld8(png + ISAAC_NP_PNG_PASS) ||
        png_init_word(png + ISAAC_NP_PNG_ROW_NUMBER) ||
        ld8(png + ISAAC_NP_PNG_BIT_DEPTH) != 8U ||
        ld8(png + ISAAC_NP_PNG_COLOR_TYPE) != 6U ||
        ld8(png + ISAAC_NP_PNG_CHANNELS) != 4U ||
        ld8(png + ISAAC_NP_PNG_PIXEL_DEPTH) != 32U ||
        png_init_word(png + ISAAC_NP_PNG_WIDTH) != width ||
        png_init_word(png + ISAAC_NP_PNG_HEIGHT) != height ||
        png_init_word(png + ISAAC_NP_PNG_ROWBYTES) != rowbytes ||
        png_init_word(png + ISAAC_NP_PNG_IROWBYTES) != rowbytes + 1U ||
        png_init_word(png + ISAAC_NP_PNG_IWIDTH) != width ||
        (png_init_word(png + ISAAC_NP_PNG_TRANSFORMATIONS) &
            ~ISAAC_NP_TRANSFORM_GAMMA) ||
        png_init_word(png + ISAAC_NP_PNG_READ_DATA_FN) !=
            GUEST_IMAGE_BASE + ISAAC_NP_DEFAULT_READ_FN_RVA ||
        png_init_word(png + ISAAC_NP_PNG_READ_ROW_FN) ||
        png_init_word(png + PNG_ERROR_FN) ||
        png_init_word(png + PNG_WARNING_FN) ||
        png_init_word(png + PNG_ERROR_PTR) ||
        png_init_word(info + PNG_INFO_WIDTH) != width ||
        png_init_word(info + PNG_INFO_HEIGHT) != height ||
        png_init_word(info + PNG_INFO_ROWBYTES) != rowbytes ||
        ld8(info + PNG_INFO_DEPTH) != 8U ||
        ld8(info + PNG_INFO_COLOR) != 6U)
        goto release;
    result = isaac_vita_texel_scratch_png_exact(destination, count);
release:
    for (i = 3U; i != 0U; --i)
        if (tokens[i - 1U] &&
            !isaac_vita_guest_heap_lease_release(tokens[i - 1U]))
            failed_release = bases[i - 1U];
    if (failed_release) {
        png_init_inc(&s_init.faults);
        result = -1;
        errno = saved_errno; /* guest_fault may leave through a longjmp. */
        guest_fault(c, failed_release, "PNG texel init metadata lease release failed");
    }
done:
    if (result > 0) {
        png_init_inc(&s_init.elided);
        if (UINT64_MAX - s_init.bytes < count) {
            s_init.bytes = UINT64_MAX;
            s_init.saturated = 1U;
        } else {
            s_init.bytes += count;
        }
    }
    errno = saved_errno;
    return result;
}
