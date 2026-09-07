/* PNG-only fail-closed libdeflate subset. See vendor/libdeflate-strict/README.md.
 * One TU exposes the upstream concrete context to this typed local-stack
 * adapter, without a public allocator or any shared mutable decoder state. */
#include "host_vita_native_png_libdeflate.h"
#include "host_vita_native_png_adler.h"

#if !defined(ISAAC_VITA_NATIVE_PNG_LIBDEFLATE_STRICT) || \
    !ISAAC_VITA_NATIVE_PNG_LIBDEFLATE_STRICT
#error "Compile the private libdeflate adapter only for strict PNG ON"
#endif
#if !ISAAC_VITA_NATIVE_PNG_ADLER_NEON
#error "The strict PNG probe requires the existing PNG Adler implementation"
#endif

#define ISAAC_NP_LD_NO_ALLOC 1
#define ISAAC_NP_LD_PORTABLE 1
#define libdeflate_deflate_decompress_ex isaac_np_ld_deflate_decompress_ex
#define libdeflate_deflate_decompress isaac_np_ld_deflate_decompress
#define libdeflate_zlib_decompress_ex isaac_np_ld_zlib_decompress_ex
#define libdeflate_zlib_decompress isaac_np_ld_zlib_decompress
#define libdeflate_adler32 isaac_np_ld_adler32
#include "vendor/libdeflate-strict/lib/deflate_decompress.c"
#include "vendor/libdeflate-strict/lib/zlib_decompress.c"

/* zlib_decompress_ex calls this only for a successfully decoded bounded
 * output. Both the seed and data semantics match the existing PNG checksum. */
LIBDEFLATEAPI uint32_t libdeflate_adler32(uint32_t seed, const void *data,
                                       size_t bytes)
{
    return isaac_np_adler32_update(seed, (const uint8_t *)data, (uint32_t)bytes);
}

static int np_ld_range(uintptr_t start, uint32_t bytes)
{
    return bytes <= UINTPTR_MAX - start;
}

static int np_ld_overlap(uintptr_t a, uint32_t an, uintptr_t b, uint32_t bn)
{
    return a < b + bn && b < a + an;
}

int isaac_np_libdeflate_try(const uint8_t *input, uint32_t input_bytes,
                          uint8_t *output, uint32_t output_bytes)
{
    const uintptr_t in = (uintptr_t)input, out = (uintptr_t)output;
    struct libdeflate_decompressor d;
    size_t consumed = 0U, produced = 0U;
    enum libdeflate_result status;

    _Static_assert(sizeof(struct libdeflate_decompressor) <= 12U * 1024U,
                   "pinned decoder context exceeded its bounded scratch budget");
    if (!input || !output || input_bytes == 0U ||
        input_bytes > 65536U || output_bytes == 0U ||
        !np_ld_range(in, input_bytes) || !np_ld_range(out, output_bytes) ||
        np_ld_overlap(in, input_bytes, out, output_bytes))
        return 0;

    /* Matches upstream alloc_decompressor_ex initialization without allocating
     * or depending on unused caller storage / opaque read-context aliases. */
    memset(&d, 0, sizeof(d));
    status = libdeflate_zlib_decompress_ex(&d, input, input_bytes, output,
                                         output_bytes, &consumed, &produced);
    return status == LIBDEFLATE_SUCCESS && consumed == input_bytes &&
           produced == output_bytes;
}
