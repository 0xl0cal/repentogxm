/* A second, explicitly PNG-owned tinfl entry. Archive decoding retains the
 * original translation unit, symbol, state ABI and scalar Adler code. */
#include "host_vita_native_png_adler.h"
#include "host_vita_native_png_lz_neon.h"

#ifndef ISAAC_VITA_NATIVE_PNG_FIXED_TABLES
#define ISAAC_VITA_NATIVE_PNG_FIXED_TABLES 0
#endif
#if ISAAC_VITA_NATIVE_PNG_FIXED_TABLES
#include "host_vita_native_png_fixed_tables.h"
#define ISAAC_MINIZ_FIXED_LIT_LOOKUP isaac_np_fixed_lit_lookup
#define ISAAC_MINIZ_FIXED_DIST_LOOKUP isaac_np_fixed_dist_lookup
#endif

#if !ISAAC_VITA_NATIVE_PNG_ADLER_NEON
#error "PNG-only tinfl entry must only be built with native PNG Adler enabled"
#endif

#if ISAAC_VITA_NATIVE_PNG_REUSE_TINFL
#define ISAAC_MINIZ_NATIVE_ENTRY isaac_vita_png_miniz_reuse
#define ISAAC_MINIZ_HISTORY_ARGUMENT , uint32_t *history_unsafe
#define ISAAC_MINIZ_HISTORY_UNSAFE(condition) \
    do { if ((condition) && history_unsafe) *history_unsafe = 1U; } while (0)
#else
#define ISAAC_MINIZ_NATIVE_ENTRY isaac_vita_png_miniz_native
#endif
#define ISAAC_MINIZ_ADLER32_UPDATE isaac_np_adler32_update
#if ISAAC_VITA_NATIVE_PNG_LZ_NEON
#if ISAAC_VITA_NATIVE_PNG_LZ_D4_NEON
#define ISAAC_MINIZ_MATCH_COPY(out, src, n, d, history) \
    (isaac_np_lz_d4_try(out, src, n, d, history) || \
     isaac_np_lz_match_try(out, src, n, d))
#else
/* Discard history at preprocessing time: the old helper's ABI/code remains
 * exactly the four-argument call when the D4 extension is OFF. */
#define ISAAC_MINIZ_MATCH_COPY(out, src, n, d, history) \
    isaac_np_lz_match_try(out, src, n, d)
#endif
#endif
#include "host_vita_archive_miniz_native.c"
#undef ISAAC_MINIZ_FIXED_LIT_LOOKUP
#undef ISAAC_MINIZ_FIXED_DIST_LOOKUP
#undef ISAAC_MINIZ_MATCH_COPY
#undef ISAAC_MINIZ_ADLER32_UPDATE

#if ISAAC_VITA_NATIVE_PNG_REUSE_TINFL
/* Preserve the existing PNG-only ABI for other callers and host checks. The
 * observed variant changes neither the frozen state nor decoder acceptance. */
int isaac_vita_png_miniz_native(
    void *state, const uint8_t *input, uint32_t *input_size,
    uint8_t *output_start, uint8_t *output_next, uint32_t *output_size,
    uint32_t flags)
{
    return isaac_vita_png_miniz_reuse(state, input, input_size, output_start,
        output_next, output_size, flags, NULL);
}
#endif
