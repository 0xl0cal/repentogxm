# Native PNG row-unfilter proof (not a production switch)

Status: **GO only for a default-OFF physical-Vita A/B experiment; NO-GO for a
production default.**  Bundle6 attributed 26.4% of its sampled pre-Exit PNG
outer time to the translated row-unfilter region.  That is an observed upper
bound from a sampled nested profiler, not a production speedup estimate.  The
helper is now connected through a frozen entry shim and the explicit
`ISAAC_VITA_PNG_NATIVE_UNFILTER` switch, which defaults to OFF.

The frozen input for every address below is the 8,650,240-byte PE with SHA-256
`31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404`.

## Exact frozen seam

| Meaning | Half-open RVA range | Size | SHA-256 |
|---|---:|---:|---|
| executable filter region | `[005c6bf0,005c6f84)` | 916 | `09b4b16da837b7e8e6f0add69704517d3f96990b642bc0d9e4c60d4df2bc1746` |
| filter jump table | `[005c6f84,005c6f98)` | 20 | `073997e6a32211be2bf5a9e53a24c264c38ff33e73a0c22c6759a4ca5a37f7db` |
| post-table alignment | `[005c6f98,005c6fa0)` | 8 | eight `cc` bytes |
| row-info construction through call | `[005b1788,005b17fc)` | 116 | `377f2cfd510d555fc9485f581df8da9cee81cdec9dfe1cd088a1fb07a8f69cd8` |
| argument setup through call | `[005b17e5,005b17fc)` | 23 | `1486545689b70d544690f8eeeeeedc71de1cf2448e8e048a0ba463ad17d6a0cf` |
| call instruction itself | `[005b17f7,005b17fc)` | 5 | `db269ad2e16d02fabae3d38a590a6df64127f8ac7f5c9078859cf41f98a0cd20` |

The 944-byte `[005c6bf0,005c6fa0)` root-to-root span includes the table and
padding; it is deliberately not called the function body.

The private x86 calling convention at RVA `005b17f7` is:

- `ECX = png_struct*`;
- `EDX = png_row_info*` (`lea edx,[esi+f8]` at `005b178f`);
- stack arguments are `row`, `previous_row`, and `filter`, in that push order;
- the caller passes `row_buf+1` and `prev_row+1`;
- the call instruction targets RVA `005c6bf0`.

The caller constructs the 32-bit `png_row_info` layout in place.  The filter
body reads `rowbytes` as a 32-bit value at offset `+4`, reads `pixel_depth` as a
byte at `+11`, and computes `bpp = (pixel_depth + 7) >> 3`.  This is the standard
12-byte 32-bit layout (`width`, `rowbytes`, four byte fields).  The ImagePng loop
reaches the row pipeline through the direct call at RVA `005a139d`.

The five jump-table entries are, in filter-number order:

1. `005c6cb4`: None/shared return;
2. `005c6c0e`: Sub;
3. `005c6cbb`: Up;
4. `005c6d55`: Average;
5. `005c6ecf`: Paeth.

Values above four branch to `005c6f79`, load the frozen "Bad adaptive filter
type" message and call RVA `005c3420`.  A native helper must never consume that
case; fallback is required so the original error behavior remains observable.

The continuation at RVA `005b17fc` immediately overwrites `EAX`, prepares a
second call, and finally adds `0x18` to `ESP`, cleaning both sets of three stack
arguments together.  The entry shim therefore leaves all three arguments on
the guest stack and consumes only the generated return word on HANDLED.  It
preserves every guest register and runs after the function-coverage event.  On
every ordinary rejection it changes neither `CPU`, stack nor row bytes and the
original translated body executes.  It does not model the seam as an ordinary
C call with native stack cleanup.  If switch-edge coverage is armed, the shim
is bypassed so the original dispatch records its exact existing case event.

Before reading row metadata or bytes, the shim pins exact requested-size heap
ranges for `png_struct`, `row_buf` and (when required) `prev_row`.  From the
frozen caller it proposes `ECX`, `row-1` and `previous_row-1` as their exact
bases, then accepts them only if an exact heap-ledger lookup proves each full
requested range.  This avoids scanning the full ownership table.  Non-wrapping
range, short-row, filter, metadata and overlap checks all fail back to the
translated implementation.  A lease-release failure is a runtime fault and
can never execute a second unfilter pass.

## Upstream equivalence and the NEON boundary

The valid-filter scalar equations match the official
[libpng 1.4.20 `png_read_filter_row`](https://github.com/pnggroup/libpng/blob/v1.4.20/pngrutil.c)
and the [PNG filter definitions](https://www.w3.org/TR/png-3/#9Filters).
Sub uses the already reconstructed byte at
`i-bpp`; Up adds the byte above; Average floors `(left+above)/2`; Paeth keeps the
specified tie order left, above, upper-left.  All additions wrap modulo 256.
Only filters 0 through 4 are claimed equivalent.

Current libpng dispatches its ARM implementations from
[`arm/arm_init.c`](https://github.com/pnggroup/libpng/blob/v1.6.37/arm/arm_init.c):
Up has a general NEON implementation, while Sub/Average/Paeth select specialized
three- or four-byte-pixel kernels.  VitaSDK's own
[`libpng/VITABUILD`](https://github.com/vitasdk/packages/blob/master/libpng/VITABUILD)
configures libpng with `PNG_ARM_NEON=on`, so ARMv7 NEON itself is established
Vita prior art.

Copying those upstream kernels here is still a **NO-GO**.  For example,
[`filter_neon_intrinsics.c`](https://github.com/pnggroup/libpng/blob/v1.6.37/arm/filter_neon_intrinsics.c)
iterates Up in complete 16-byte vectors while `rp < row + rowbytes`, and the
specialized kernels similarly consume whole blocks.  Upstream pairs that with
libpng-owned aligned row allocations and padding.  The exact frozen allocator's
equivalent slack/alignment contract has not been pinned, so importing the raw
kernels could read or write beyond the logical guest row.

The experimental helper takes the narrower safe path:

- explicit row-info, row, and previous-row capacities;
- reject malformed depths, short rows, invalid filters, and overlapping row
  ranges without changing a byte;
- bounded 16-byte NEON chunks plus a scalar tail for Up only;
- scalar Sub/Average/Paeth, preserving their left-byte dependencies;
- compile-time NEON selection.  A Vita opt-in without the target NEON macro is a
  compile error; host differential tests use scalar code.

Runtime CPU probing is unnecessary for this port: the Vita target is already
compiled for `cortex-a9`, `neon`, Thumb-2 and softfp.  The resulting object must
still keep the same softfp build contract as the rest of the executable.

## Oracle result

`host_vita_png_unfilter_native_oracle.c` covers every combination of:

- filters 0 through 4;
- `bpp` 1 through 8, using both pixel-depth endpoints that round to each `bpp`;
- every row length 1 through 271, which exhausts all 16-byte vector-tail
  residues many times;
- zero, `ff`, arithmetic, and deterministic pseudo-random byte patterns;
- separated, adjacent, exact-alias, and both one-byte-overlap layouts.

Alias and short-row cases assert fallback plus whole-arena immutability.  Further
hostile cases cover nulls, one-byte-short capacities, invalid filters, depth zero
and 65, zero and `UINT32_MAX` row sizes, unaligned row-info, and the default-off
path.  The independent reference expresses the PNG equations directly rather
than calling the implementation helpers.

Verified locally without a Vita/VM build or deployment:

- default-off host oracle: 2 cases, PASS;
- enabled host oracle: 433,613 cases, PASS;
- AddressSanitizer + UndefinedBehaviorSanitizer enabled oracle: 433,613 cases,
  PASS;
- guest entry hostile oracle: 22 rejection/handled cases plus two injected
  lease-release failures, PASS; fallback preserves the full `CPU`, guest stack
  and both row arenas, while HANDLED changes only the row and guest `ESP`;
- local Clang ARMv7-A/Cortex-A9 Thumb softfp cross-object: PASS; disassembly has
  bounded `vld1.8`, `vadd.i8`, and `vst1.8` in the Up path.
- frozen PE plus generated-function contract oracle: PASS.
- CMake ON/OFF owner/source/definition scope and raw-gate hostile oracles: PASS.

With both experiment and PNG profiling enabled, bounded `pngprof2.f` records
report `native=attempts/handled/fallback` and eight rejection counters in
`stack,abi,filter,metadata,range,alias,helper,release` order.  The static log
bound remains below the logger's 384-byte body.

The Bundle5 raw ELF's translated `sub_005c6bf0` symbol is 4,788 bytes.  The
local ARM cross-object's native-core public entry plus its three out-of-line
helpers totals 1,308 bytes; the new boundary shim is not included in that
comparison.  This is static evidence that native C avoids substantial
translated CPU-state machinery; it is not a timing result.

## Expected impact and physical A/B gate

Expected direct overlap is only PNG row unfiltering before texture upload.  It
does not accelerate archive reads, zlib inflate, color transforms, image-object
construction, or `glTexImage2D`.  Therefore no percentage of the observed
room-transition stalls is defensible from this change.  The 4,788-byte
translated state-machine body and Bundle6 share make a hardware A/B worth
running, but only paired physical measurements can establish the magnitude or
whether it is visible to an end user.

The host-side range, ABI, fallback, frozen-PE/codegen, CMake and raw-gate proofs
authorize building that experiment.  They do not replace a Vita build/VPK gate,
hardware correctness run, accepted/fallback/reject telemetry review, or paired
timing.  The switch remains excluded from release builds until those results
justify a separate production decision.
