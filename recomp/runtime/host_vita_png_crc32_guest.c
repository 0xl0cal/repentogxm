#include "host_vita_png_crc32_guest.h"

#include <stdint.h>

#include "host_vita_png_crc32_native.h"

#if !defined(ISAAC_VITA_PNG_CRC32_FASTPATH) || \
    !ISAAC_VITA_PNG_CRC32_FASTPATH
#error The PNG CRC32 guest shim must only be compiled when enabled
#endif

enum {
    ISAAC_PNG_CRC32_RETURN_READ = 0x005c3cadU,
    ISAAC_PNG_CRC32_RETURN_SKIP = 0x005c3d48U,
    ISAAC_PNG_CRC32_TABLE_RVA = 0x0072e9b0U
};

#ifndef ISAAC_VITA_PNG_CRC32_TABLE_ADDRESS
#define ISAAC_VITA_PNG_CRC32_TABLE_ADDRESS \
    (GUEST_IMAGE_BASE + ISAAC_PNG_CRC32_TABLE_RVA)
#endif

int isaac_vita_png_crc32_guest_try(CPU *__restrict c)
{
    uint32_t return_rva;
    uint32_t data;
    uint32_t size;
    uint32_t result;

    if (c == NULL || !guest_stack_contains(c, c->esp, 8U))
        return 0;

    return_rva = ld32(c->esp);
    if (return_rva != ISAAC_PNG_CRC32_RETURN_READ &&
            return_rva != ISAAC_PNG_CRC32_RETURN_SKIP)
        return 0;

    data = c->edx;
    size = ld32(c->esp + 4U);
    if (data != 0U && size != 0U &&
            data > UINT32_MAX - (size - 1U))
        return 0;

    result = isaac_vita_png_crc32_native(
        c->ecx, (const uint8_t *)(uintptr_t)data, size,
        (const uint32_t *)(uintptr_t)ISAAC_VITA_PNG_CRC32_TABLE_ADDRESS);

    c->eax = result;
    if (data != 0U) {
        c->ecx = result;
        c->edx = data + size;
        if (size != 0U)
            SET_FLAGS(c, FLAG_SUB, 1U, 1U, 0U, 4U);
    }
    /* The translated caller owns the one stacked size argument. */
    (void)gpop_generated(c);
    return 1;
}
