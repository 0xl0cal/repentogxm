/* ARM-only harness for the production tinfl state-read/reset test.
 * Freestanding byte libc avoids linking a platform runtime into Unicorn. */
#include <stddef.h>
#include <stdint.h>
#include "host_vita_archive_miniz_native.h"

void *memcpy(void *destination, const void *source, size_t bytes)
{
    volatile uint8_t *d = (volatile uint8_t *)destination;
    const volatile uint8_t *s = (const volatile uint8_t *)source;
    size_t i;
    for (i = 0; i < bytes; ++i)
        d[i] = s[i];
    return destination;
}

void *memset(void *destination, int value, size_t bytes)
{
    volatile uint8_t *d = (volatile uint8_t *)destination;
    size_t i;
    for (i = 0; i < bytes; ++i)
        d[i] = (uint8_t)value;
    return destination;
}

int np_test_step(const uintptr_t *arguments)
{
    return isaac_vita_archive_miniz_native(
        (void *)arguments[0], (const uint8_t *)arguments[1],
        (uint32_t *)arguments[2], (uint8_t *)arguments[3],
        (uint8_t *)arguments[4], (uint32_t *)arguments[5],
        (uint32_t)arguments[6]);
}
