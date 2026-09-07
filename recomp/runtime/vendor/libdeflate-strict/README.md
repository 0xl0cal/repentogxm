# Private strict PNG decompressor subset

Imported from Eric Biggers' libdeflate v1.26, commit
`92e6a0db9fa848d742f9eb286c92afc60f2c3dda`:
https://github.com/ebiggers/libdeflate/tree/92e6a0db9fa848d742f9eb286c92afc60f2c3dda
MIT license, preserved in `COPYING` and source headers. Only eight decompressor
source/header files are imported; no compressor, gzip, utilities, allocator,
CPU-feature discovery or upstream build system is linked.

Local changes are deliberately fail-closed:

- `build_decode_table` refuses every incomplete/overfull tree, including legal
  singleton/empty cases, for precode, literal/length and distance tables.
- Dynamic alphabets must contain at most 286 literal/length and 30 distance
  symbols; EOB 256 must be present before table construction overwrites the lens
  union. This excludes the upstream reserved-symbol mappings.
- Every fixed-Huffman block refuses before static-table reuse. Its reserved
  symbols otherwise have permissive real-symbol mappings; the hot decoder loop
  is not changed to add ERROR entries.
- Only a single final block is admitted. A nonfinal block refuses before output,
  so a later block cannot introduce a different, incompatible alphabet.
- Dynamic-table construction must finish without virtual input bytes
  (`overread_count == 0` at `have_decode_tables`). A truncated first IDAT cannot
  select an alphabet different from the original streaming decoder's alphabet
  when the latter continues reading real bytes from another IDAT.
- `ISAAC_NP_LD_PORTABLE` disables x86 runtime dispatch. `ISAAC_NP_LD_NO_ALLOC`
  removes allocation/free endpoints. The private adapter always defines both.

Stored blocks and admitted dynamic blocks retain upstream checks, including
repeat bounds, history bounds, input/output bounds and Adler validation. The
upstream literal `#if 0` disabling safety checks remains zero. This is a bounded
baseline-compatibility design, not a proof of every malformed RFC input: smaller
advertised zlib windows are not enforced by either this decoder or the existing
PNG tinfl NON_WRAPPING mode.

`host_vita_native_png_libdeflate.c` includes the two C files in one private TU,
prefixes its linked decoder symbols, and supplies the existing PNG Adler routine
instead of importing another checksum/dispatch implementation. Its concrete
context is automatic typed storage, fully zeroed like upstream allocation.
Pinned ARM context size is 11,564 bytes (4-byte alignment), capped at 12 KiB by a
static assertion. Actual GCC total stack frame is a separate build check. The
main native thread requests 4 MiB, but that is not measured free stack headroom.
No raw heap allocator, mutable global decoder context, retained image or pointer,
new memory reserve, or staging-tail alias assumption is introduced.

The default-OFF `ISAAC_VITA_NATIVE_PNG_LIBDEFLATE_STRICT` requires native PNG and
its existing PNG Adler option; it never enables either implicitly. Only a
positive, completely staged original first IDAT of at most 64 KiB is probed,
after unchanged NP_READ/CRC/accounting and before the first tinfl call. Success
requires full input consumption and the exact filtered-raw output size. Every
refusal, including nonexact success, runs virgin original tinfl on the same input
and rewrites private output from its start. A failed probe can have written
private bytes: the restrictions above are therefore required for retry, not
merely for acceptance. For an admitted dynamic header, the same complete tables
contain no reserved length/distance symbols; original tinfl uses positive,
already-produced history and overwrites the prefix before reading it. Stored
blocks validate their actual header and byte bounds before copying. Fixed,
nonfinal, incomplete and virtual-header cases decline before producing output.
This is a bounded source-level fallback argument, not exhaustive equivalence on
all invalid DEFLATE streams. The original final CRC stream
boundary, unfilter/gamma/last-row work, failure rewind and texture publication
remain unchanged. A libdeflate refusal is not a translated-image fallback.

When PNG_WINDOW_PROFILE is enabled, the existing `ph120.pngf` line appends
`strict(attempt,success,refusal)=a,s,r`. These are unsigned modular cumulative
snapshot deltas; `a == s+r` modulo 2^32. Attempts count eligible calls including
decoder refusals; success means exact raw inflate, not successful unfilter,
complete image publication or GPU use. Absent fields mean disabled/old build,
not measured zero engagement. No new clocks or per-image log family is added.
OFF/explicit zero retains old owner, snapshot and record bodies.

Tests reuse `recomp/test_vita_native_png.py --strict` with the actual core and
its existing synthetic/real PNG, CRC, chunking, gamma and fallback checks. The
separate artifact reduction preserves alignment canaries, source identities and
current-adapter benchmark results. Older prototype differentials/timings are not
measurements of this adapter. Host timings
do not predict Cortex-A9/Vita or end-to-end room-transition improvement.
