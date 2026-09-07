#include "host_vita_wav_buffered_rewind.h"
#include "host_vita_heap.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#if !defined(ISAAC_VITA_WAV_BUFFERED_REWIND) || !ISAAC_VITA_WAV_BUFFERED_REWIND
#error Compile the WAV buffered rewind owner only when enabled
#endif
#if !defined(ISAAC_VITA_HEAP_RANGE_LEASE)
#error WAV buffered rewind requires exact requested-size heap leases
#endif
#ifndef ISAAC_VITA_WAV_BUFFERED_REWIND_BUILD_ID
#define ISAAC_VITA_WAV_BUFFERED_REWIND_BUILD_ID "wav-rewind:unstamped"
#endif

/* The same existing logger declaration used by host_vita_native_png.c. */
void isaac_vita_log(const char *format, ...);

/* Copied from frozen constructor 0059bfd7, destructor 0059c091, Seek
 * 0059c690 and WAV Load 005a3b30, not a guessed C++ struct layout. */
enum {
    WAV_REWIND_RETURN = 0x005a3b8dU,
    ARCHIVED_FILE_VTABLE = 0x00765128U,
    ARCHIVED_FILE_BYTES = 0x0c2cU,
    FILE_POSITION = 0x18U,
    FILE_OUTPUT = 0x81cU,
    FILE_CURSOR = 0xc1cU,
    FILE_FILLED = 0xc20U,
    FILE_EOF = 0xc28U,
    BLOCK_BYTES = 0x400U,
    /* WAV aligns EBP, subtracts 0x1c, saves three registers, then pushes
     * two seek arguments and the return: (EBP & ~7) - 0x34. */
    WAV_SEEK_STACK_DELTA = 0x34U,
    WAV_DATA_SIZE_AT_SEEK = 0x18U,
    WAV_FORMAT_AT_SEEK = 0x20U
};

static atomic_flag s_reported = ATOMIC_FLAG_INIT;

static uint32_t wav_word(uint32_t address)
{
    uint32_t value;
    memcpy(&value, (const void *)(uintptr_t)address, sizeof value);
    return value;
}

static uint16_t wav_half(uint32_t address)
{
    uint16_t value;
    memcpy(&value, (const void *)(uintptr_t)address, sizeof value);
    return value;
}

/* Admission only, not a replacement RIFF parser. Require the already parsed
 * PCM16 fmt chunk first, followed by zero or more even-size non-fmt chunks
 * wholly in the first decoded block, ending exactly after a data header.
 * All unusual, partial and malformed shapes execute the original Seek. */
static int wav_first_header(uint32_t buffer, uint32_t position,
                            uint32_t format, uint32_t expected_size)
{
    uint32_t at = 36U;
    uint32_t channels, rate, align;
    if (position < 44U || position > BLOCK_BYTES ||
        wav_word(buffer) != 0x46464952U || /* RIFF */
        wav_word(buffer + 8U) != 0x45564157U || /* WAVE */
        wav_word(buffer + 12U) != 0x20746d66U || /* fmt */
        wav_word(buffer + 16U) != 16U ||
        wav_half(buffer + 20U) != 1U ||
        wav_half(buffer + 34U) != 16U ||
        memcmp((const void *)(uintptr_t)(buffer + 20U),
               (const void *)(uintptr_t)format, 16U) != 0)
        return 0;
    channels = wav_half(buffer + 22U);
    rate = wav_word(buffer + 24U);
    align = channels * 2U;
    if ((channels != 1U && channels != 2U) || !rate ||
        rate > UINT32_MAX / align ||
        wav_half(buffer + 32U) != align ||
        wav_word(buffer + 28U) != rate * align ||
        !expected_size || expected_size % align)
        return 0;
    while (at <= position - 8U) {
        uint32_t tag = wav_word(buffer + at);
        uint32_t length = wav_word(buffer + at + 4U);
        at += 8U;
        if (tag == 0x61746164U) { /* data */
            return at == position && length == expected_size &&
                length <= UINT32_MAX - position &&
                wav_word(buffer + 4U) == position + length - 8U;
        }
        if (tag == 0x20746d66U || (length & 1U) ||
            length > position - at)
            return 0;
        at += length;
    }
    return 0;
}

int isaac_vita_wav_buffered_rewind_try(CPU *__restrict c)
{
    const int saved_errno = errno;
    uint32_t object, token = 0U, position = 0U, filled = 0U;
    int result = 0;

    if (!c || c->fault || !guest_stack_contains(c, c->esp, 0x30U) ||
        wav_word(c->esp) != WAV_REWIND_RETURN ||
        wav_word(c->esp + 4U) != 0U || wav_word(c->esp + 8U) != 0U ||
        (c->ebp & ~7U) < WAV_SEEK_STACK_DELTA ||
        c->esp != (c->ebp & ~7U) - WAV_SEEK_STACK_DELTA)
        goto done;
    object = c->ecx;
    if (!object || object > UINT32_MAX - ARCHIVED_FILE_BYTES ||
        c->esi != object || c->eax != GUEST_IMAGE_BASE + ARCHIVED_FILE_VTABLE)
        goto done;
    token = isaac_vita_guest_heap_lease_exact_range(
        (const void *)(uintptr_t)object, (const void *)(uintptr_t)object,
        ARCHIVED_FILE_BYTES);
    if (!token)
        goto done;
    position = wav_word(object + FILE_POSITION);
    filled = wav_word(object + FILE_FILLED);
    if (wav_word(object) != GUEST_IMAGE_BASE + ARCHIVED_FILE_VTABLE ||
        !position || position != wav_word(object + FILE_CURSOR) ||
        position > filled || filled > BLOCK_BYTES ||
        ld8(object + FILE_EOF) != 0U ||
        !wav_first_header(object + FILE_OUTPUT, position,
                          c->esp + WAV_FORMAT_AT_SEEK,
                          wav_word(c->esp + WAV_DATA_SIZE_AT_SEEK)))
        goto release;

    /* The only live output at this exact void-call site is the stream's
     * logical position. Keep the already decoded first block, its decoder
     * state and the inner FILE cursor after that block. When replay consumes
     * it, the unchanged original Read refills the NEXT block, not block zero.
     * EOF is admitted clear and left clear; no stream error is cleared here.
     * EAX/ECX/EDX are overwritten at 005a3b8d/91/96 before the next call,
     * and flags are dead. Callee-saved registers remain exactly untouched. */
    st32(object + FILE_POSITION, 0U);
    st32(object + FILE_CURSOR, 0U);
    c->esp += 12U; /* The original Seek ends with ret 8 at 0059c73b. */
    result = 1;
release:
    if (!isaac_vita_guest_heap_lease_release(token)) {
        if (!result)
            c->esp += 12U;
        errno = saved_errno;
        guest_fault(c, object, "WAV rewind metadata lease release failed");
        result = 1; /* Never run the original against an unreleased lease. */
    }
    if (result && !c->fault &&
        !atomic_flag_test_and_set_explicit(&s_reported, memory_order_relaxed)) {
        isaac_vita_log("[kage-vita] WAV buffered rewind engaged bid=%.32s oldpos=%u filled=%u retained=existing-first-block\n",
            ISAAC_VITA_WAV_BUFFERED_REWIND_BUILD_ID,
            (unsigned)position, (unsigned)filled);
    }
done:
    errno = saved_errno;
    return result;
}
