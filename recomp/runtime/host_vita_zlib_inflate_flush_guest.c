#include "host_vita_zlib_inflate_flush_guest.h"

#include <stddef.h>
#include <stdint.h>

#include "guest_pe.h"
#include "host_vita_heap.h"
#include "host_vita_zlib_inflate_flush_native.h"

#if !defined(ISAAC_VITA_PNG_INFLATE_FLUSH_FASTPATH)
#error The guest zlib shim must only be compiled for its opt-in fast path
#endif
#if !defined(ISAAC_VITA_HEAP_RANGE_LEASE)
#error The guest zlib shim requires exact requested-size heap leases
#endif

enum {
    INFLATE_BLOCKS_BYTES = 0x40U,
    INFLATE_STATE_BYTES = 0x18U,
    INFLATE_WINDOW_BYTES = 0x8000U,
    PNG_STRUCT_PROVEN_BYTES = 0xacU,
    PNG_ZSTREAM_OFFSET = 0x6cU,
    PNG_OUTPUT_OFFSET = 0xa4U,
    PNG_OUTPUT_SIZE_OFFSET = 0xa8U,
    ZSTREAM_NEXT_OUT_OFFSET = 0x0cU,
    ZSTREAM_AVAIL_OUT_OFFSET = 0x10U,
    ZSTREAM_TOTAL_OUT_OFFSET = 0x14U,
    ZSTREAM_STATE_OFFSET = 0x1cU,
    ZSTREAM_ADLER_OFFSET = 0x30U,
    INFLATE_STATE_BLOCKS_OFFSET = 0x14U,
    BLOCKS_WINDOW_OFFSET = 0x28U,
    BLOCKS_END_OFFSET = 0x2cU,
    BLOCKS_READ_OFFSET = 0x30U,
    BLOCKS_WRITE_OFFSET = 0x34U,
    BLOCKS_CHECKFN_OFFSET = 0x38U,
    BLOCKS_CHECK_OFFSET = 0x3cU,
    ZLIB_ADLER32_RVA = 0x005cf3d0U,
    ZLIB_ADLER32_COVERAGE_ID = 12124U,
    MEMCPY_IAT_RVA = 0x00606488U,
    MEMCPY_IMPORT_ID = 281U,
    MEMCPY_THUNK_COVERAGE_ID = 12570U,
    PNG_OUTPUT_BYTES = 0x2000U,
    LEASE_COUNT = 5U
};

static uint32_t load_u32le(uint32_t address)
{
    const uint8_t *bytes = (const uint8_t *)(uintptr_t)address;

    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static void store_u32le(uint32_t address, uint32_t value)
{
    uint8_t *bytes = (uint8_t *)(uintptr_t)address;

    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static int return_site_is_exact(uint32_t return_rva)
{
    switch (return_rva) {
    case 0x005ce9cbU:
    case 0x005cf12fU:
    case 0x005cf2b3U:
    case 0x005cf331U:
    case 0x005cf36aU:
    case 0x005d70d9U:
    case 0x005d71beU:
    case 0x005d7268U:
    case 0x005d72f8U:
    case 0x005d732bU:
    case 0x005d736fU:
    case 0x005d73a4U:
    case 0x005d73ccU:
        return 1;
    default:
        return 0;
    }
}

static int release_leases(uint32_t tokens[LEASE_COUNT])
{
    size_t index;
    int ok = 1;

    for (index = LEASE_COUNT; index != 0U; --index) {
        if (tokens[index - 1U] != 0U) {
            ok = isaac_vita_guest_heap_lease_release(
                tokens[index - 1U]) && ok;
        }
    }
    return ok;
}

static int release_for_fallback(
    CPU *__restrict c, uint32_t tokens[LEASE_COUNT], uint32_t fault_address)
{
    if (release_leases(tokens))
        return 0;
    guest_fault(c, fault_address,
                "native zlib inflate_flush lease release failed");
    (void)gpop_generated(c);
    return 1;
}

int isaac_vita_zlib_inflate_flush_guest_try(CPU *__restrict c)
{
    uint32_t tokens[LEASE_COUNT] = { 0U, 0U, 0U, 0U, 0U };
    isaac_vita_zlib_inflate_flush_state native;
    uint32_t return_rva;
    uint32_t blocks;
    uint32_t zstream;
    uint32_t png;
    uint32_t inflate_state;
    uint32_t window;
    uint32_t window_end;
    uint32_t read;
    uint32_t write;
    uint32_t checkfn;
    uint32_t output;
    uint32_t next_out;
    uint32_t segment_count;
    uint32_t segment;
    int handled;

    if (c == NULL || !guest_stack_contains(c, c->esp, 8U))
        return 0;
    return_rva = load_u32le(c->esp);
    if (!return_site_is_exact(return_rva))
        return 0;

    blocks = c->ecx;
    zstream = c->edx;
    if (blocks == 0U || zstream < PNG_ZSTREAM_OFFSET)
        return 0;
    png = zstream - PNG_ZSTREAM_OFFSET;

    tokens[0] = isaac_vita_guest_heap_lease_exact_range(
        (const void *)(uintptr_t)png,
        (const void *)(uintptr_t)png, PNG_STRUCT_PROVEN_BYTES);
    if (tokens[0] == 0U)
        return 0;

    inflate_state = load_u32le(zstream + ZSTREAM_STATE_OFFSET);
    output = load_u32le(png + PNG_OUTPUT_OFFSET);
    if (load_u32le(png + PNG_OUTPUT_SIZE_OFFSET) != PNG_OUTPUT_BYTES ||
            inflate_state == 0U || output == 0U) {
        return release_for_fallback(c, tokens, png);
    }

    tokens[1] = isaac_vita_guest_heap_lease_exact_range(
        (const void *)(uintptr_t)inflate_state,
        (const void *)(uintptr_t)inflate_state, INFLATE_STATE_BYTES);
    if (tokens[1] == 0U ||
            load_u32le(inflate_state + INFLATE_STATE_BLOCKS_OFFSET) != blocks) {
        return release_for_fallback(c, tokens, inflate_state);
    }
    tokens[2] = isaac_vita_guest_heap_lease_exact_range(
        (const void *)(uintptr_t)blocks,
        (const void *)(uintptr_t)blocks, INFLATE_BLOCKS_BYTES);
    if (tokens[2] == 0U)
        return release_for_fallback(c, tokens, blocks);

    window = load_u32le(blocks + BLOCKS_WINDOW_OFFSET);
    window_end = load_u32le(blocks + BLOCKS_END_OFFSET);
    read = load_u32le(blocks + BLOCKS_READ_OFFSET);
    write = load_u32le(blocks + BLOCKS_WRITE_OFFSET);
    checkfn = load_u32le(blocks + BLOCKS_CHECKFN_OFFSET);
    next_out = load_u32le(zstream + ZSTREAM_NEXT_OUT_OFFSET);
    if (window == 0U || window > UINT32_MAX - INFLATE_WINDOW_BYTES ||
            window_end != window + INFLATE_WINDOW_BYTES ||
            read < window || read > window_end ||
            write < window || write > window_end ||
            (checkfn != 0U &&
             checkfn != GUEST_PE_VITA_TARGET_BASE + ZLIB_ADLER32_RVA) ||
            output > UINT32_MAX - PNG_OUTPUT_BYTES ||
            next_out < output ||
            next_out > output + PNG_OUTPUT_BYTES) {
        return release_for_fallback(c, tokens, blocks);
    }

    tokens[3] = isaac_vita_guest_heap_lease_exact_range(
        (const void *)(uintptr_t)window,
        (const void *)(uintptr_t)window, INFLATE_WINDOW_BYTES);
    tokens[4] = isaac_vita_guest_heap_lease_exact_range(
        (const void *)(uintptr_t)output,
        (const void *)(uintptr_t)output, PNG_OUTPUT_BYTES);
    if (tokens[3] == 0U || tokens[4] == 0U)
        return release_for_fallback(c, tokens, window);

    native.window = (uint8_t *)(uintptr_t)window;
    native.window_capacity = INFLATE_WINDOW_BYTES;
    native.read_offset = read - window;
    native.write_offset = write - window;
    native.output = (uint8_t *)(uintptr_t)output;
    native.output_capacity = PNG_OUTPUT_BYTES;
    native.next_out_offset = next_out - output;
    native.avail_out = load_u32le(zstream + ZSTREAM_AVAIL_OUT_OFFSET);
    native.total_out = load_u32le(zstream + ZSTREAM_TOTAL_OUT_OFFSET);
    native.check = load_u32le(blocks + BLOCKS_CHECK_OFFSET);
    native.result = (int32_t)load_u32le(c->esp + 4U);
    native.check_enabled = checkfn != 0U;

    handled = isaac_vita_zlib_inflate_flush_native_try(
        &native, &segment_count);
    if (handled != ISAAC_VITA_ZLIB_INFLATE_FLUSH_HANDLED)
        return release_for_fallback(c, tokens, blocks);

    store_u32le(blocks + BLOCKS_READ_OFFSET,
                window + native.read_offset);
    store_u32le(blocks + BLOCKS_WRITE_OFFSET,
                window + native.write_offset);
    store_u32le(zstream + ZSTREAM_NEXT_OUT_OFFSET,
                output + native.next_out_offset);
    store_u32le(zstream + ZSTREAM_AVAIL_OUT_OFFSET, native.avail_out);
    store_u32le(zstream + ZSTREAM_TOTAL_OUT_OFFSET, native.total_out);
    if (native.check_enabled != 0U) {
        store_u32le(blocks + BLOCKS_CHECK_OFFSET, native.check);
        store_u32le(zstream + ZSTREAM_ADLER_OFFSET, native.check);
    }

    for (segment = 0U; segment < segment_count; ++segment) {
        if (native.check_enabled != 0U &&
                !guest_note_authenticated_translated_call(
                    ZLIB_ADLER32_RVA, ZLIB_ADLER32_COVERAGE_ID)) {
            (void)release_leases(tokens);
            guest_fault(c, ZLIB_ADLER32_RVA,
                        "native inflate_flush Adler target disappeared");
            (void)gpop_generated(c);
            return 1;
        }
        if (!guest_note_authenticated_import_call(
                MEMCPY_IAT_RVA, MEMCPY_IMPORT_ID,
                MEMCPY_THUNK_COVERAGE_ID)) {
            (void)release_leases(tokens);
            guest_fault(c, MEMCPY_IAT_RVA,
                        "native inflate_flush memcpy import disappeared");
            (void)gpop_generated(c);
            return 1;
        }
    }

    c->eax = (uint32_t)native.result;
    if (!release_leases(tokens)) {
        guest_fault(c, blocks,
                    "native zlib inflate_flush lease release failed");
        (void)gpop_generated(c);
        return 1;
    }
    (void)gpop_generated(c);
    return 1;
}
