# stb_vorbis v1.04 (vendored)

Upstream: https://github.com/nothings/stb/blob/b8e0530fdfbe/stb_vorbis.c
(commit `b8e0530fdfbe`, 2014-08-28, "update contributor list & version
number"; header line `Ogg Vorbis audio decoder - v1.04 - public domain`).
Upstream file SHA-256 before the local patch:
`0ac65132886109ec5b9f3dc633b881ea95a33de4daa01d44751fa9492d08bc61`.

Written by Sean Barrett, placed in the public domain by the author (see the
file header).

## Why this exact revision

The Repentance PE statically links this decoder as
`KAGE\Source\Sound\Base\External\Ogg\Ogg.cpp` (the `_wassert` file operand at
RVA 0x7686e8).  Its assertion expressions and line numbers, its 0x5f8-byte
`stb_vorbis` state (old five-field `ProbedPage`, `eof` at +0x70,
`channel_buffers` at +0x330, `channel_buffer_start` at +0x5f0), the 0x830-byte
`Codebook` stride (`STB_VORBIS_FAST_HUFFMAN_SHORT`), the float codebooks
(`STB_VORBIS_CODEBOOK_FLOATS`), the four-byte `setup_malloc` rounding and the
magic-number float-to-int16 conversion (`0xbc400000` addend in
`convert_channels_short_interleaved`) all match v1.04 and no other released
version.  The game's only source edit replaces `FILE *f` with a KAGE stream
object (same pointer size; vtable slots +0x08 tell, +0x0c seek, +0x14 read),
which the runtime reproduces through the `fgetc`/`fread`/`fseek`/`ftell`
macros defined before this file is included (see
`recomp/runtime/host_vita_native_vorbis.c`).

## Local patches

1. `#include <assert.h>` may be replaced by the header named in
   `ISAAC_NV_ASSERT_HEADER`, so the runtime can turn a failed decoder
   assertion into a bounded receipt plus a stream-local abort instead of
   `abort()`.
2. The eight libc allocator call sites (`setup_malloc`, `setup_free`,
   `setup_temp_malloc`, `setup_temp_free`, `stb_vorbis_decode_filename`,
   `stb_vorbis_decode_memory`) call `ISAAC_NV_MALLOC` / `ISAAC_NV_REALLOC` /
   `ISAAC_NV_FREE`, which default to `malloc`/`realloc`/`free`.  The Vita
   runtime poisons the raw libc allocators in every translation unit; it maps
   the seam to a fail-closed stub because every state it decodes owns a
   `stb_vorbis_alloc` buffer and never reaches these calls.

No decoding statement is changed.
